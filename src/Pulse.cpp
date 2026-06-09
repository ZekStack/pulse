#include "Pulse.h"

#include "internal/PulseMutex.h"
#include "internal/PulseTaskSupport.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>

#include <esp_timer.h>
#include <freertos/queue.h>

namespace {
constexpr PulseTimerId kInvalidTimerId = 0;
constexpr uint32_t kWaitPollMs = 10;

enum class PulseCommandType : uint8_t {
	Add,
	Clear,
	Pause,
	Resume,
	Restart,
	Shutdown,
};

struct PulseCommand {
	PulseCommandType type = PulseCommandType::Add;
	PulseTimerId id = kInvalidTimerId;
};

uint64_t pulseNowMs() {
	return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

bool addWouldOverflow(uint32_t left, uint32_t right) {
	return left > UINT32_MAX - right;
}

bool isActiveState(PulseTimerState state) {
	return state == PulseTimerState::Running || state == PulseTimerState::Paused;
}
} // namespace

struct PulseTimerRecord {
	PulseTimerId id = kInvalidTimerId;
	PulseTimerType type = PulseTimerType::Timeout;
	PulseTimerState state = PulseTimerState::Running;
	PulseCallback callback;
	PulseCountdownCallback countdownCallback;
	uint32_t delayMs = 0;
	uint32_t intervalMs = 0;
	uint32_t durationMs = 0;
	uint32_t tickMs = 0;
	uint32_t elapsedMs = 0;
	uint32_t remainingDelayMs = 0;
	uint64_t nextDueMs = 0;
};

struct PulseImpl {
	PulseConfig config{};
	PulseMutex mutex;
	std::unique_ptr<std::shared_ptr<PulseTimerRecord>[]> timers;
	std::unique_ptr<std::shared_ptr<PulseTimerRecord>[]> activeTimers;
	uint32_t timerCapacity = 0;
	uint32_t timerCount = 0;
	uint32_t activeTimerCount = 0;
	QueueHandle_t commandQueue = nullptr;
	TaskHandle_t taskHandle = nullptr;
	bool initialized = false;
	bool stopping = false;
	bool taskStopped = true;
	bool createdWithCaps = false;
	PulseStackType actualStackType = PulseStackType::Internal;
	PulseTimerId nextTimerId = 1;
	uint32_t executedCallbackCount = 0;
	uint32_t droppedCommandCount = 0;
	uint32_t lateCallbackCount = 0;
	size_t stackHighWaterMarkBytes = 0;

	std::shared_ptr<PulseTimerRecord> findTimerLocked(PulseTimerId id) {
		for (uint32_t index = 0; index < timerCount; index++) {
			auto &timer = timers[index];
			if (timer && timer->id == id) {
				return timer;
			}
		}
		return nullptr;
	}

	bool hasTimerLocked(PulseTimerId id) {
		return findTimerLocked(id) != nullptr;
	}

	uint32_t countTypeLocked(PulseTimerType type) const {
		uint32_t count = 0;
		for (uint32_t index = 0; index < timerCount; index++) {
			const auto &timer = timers[index];
			if (timer && timer->type == type && isActiveState(timer->state)) {
				count++;
			}
		}
		return count;
	}

	bool limitReachedLocked(PulseTimerType type) const {
		if (type == PulseTimerType::Timeout) {
			return countTypeLocked(type) >= config.maxTimeouts;
		}
		if (type == PulseTimerType::Interval) {
			return countTypeLocked(type) >= config.maxIntervals;
		}
		return countTypeLocked(type) >= config.maxCountdowns;
	}

	PulseTimerResult validateTimerCreateLocked(
	    PulseTimerType type,
	    const char *limitMessage
	) const {
		if (!initialized || stopping) {
			return PulseTimerResult::failure(
			    PulseStatus::NotInitialized,
			    "pulse is not initialized"
			);
		}
		if (limitReachedLocked(type)) {
			return PulseTimerResult::failure(PulseStatus::Busy, limitMessage);
		}
		if (timerCount >= timerCapacity) {
			return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
		}
		return PulseTimerResult::success(kInvalidTimerId);
	}

	bool allocateTimerIdLocked(PulseTimerId &out) {
		for (uint32_t attempts = 0; attempts < UINT32_MAX; attempts++) {
			const PulseTimerId id = nextTimerId++;
			if (id == kInvalidTimerId) {
				continue;
			}
			if (!hasTimerLocked(id)) {
				out = id;
				return true;
			}
		}
		return false;
	}

	bool addRegistryLocked(const std::shared_ptr<PulseTimerRecord> &timer) {
		if (!timer || timerCount >= timerCapacity) {
			return false;
		}
		timers[timerCount++] = timer;
		return true;
	}

	void removeActiveLocked(PulseTimerId id) {
		uint32_t writeIndex = 0;
		for (uint32_t readIndex = 0; readIndex < activeTimerCount; readIndex++) {
			auto &timer = activeTimers[readIndex];
			if (timer && timer->id != id) {
				if (writeIndex != readIndex) {
					activeTimers[writeIndex] = timer;
				}
				writeIndex++;
			}
		}
		for (uint32_t index = writeIndex; index < activeTimerCount; index++) {
			activeTimers[index].reset();
		}
		activeTimerCount = writeIndex;
	}

	void removeRegistryLocked(PulseTimerId id) {
		removeActiveLocked(id);
		uint32_t writeIndex = 0;
		for (uint32_t readIndex = 0; readIndex < timerCount; readIndex++) {
			auto &timer = timers[readIndex];
			if (timer && timer->id != id) {
				if (writeIndex != readIndex) {
					timers[writeIndex] = timer;
				}
				writeIndex++;
			}
		}
		for (uint32_t index = writeIndex; index < timerCount; index++) {
			timers[index].reset();
		}
		timerCount = writeIndex;
	}

	void insertActiveLocked(const std::shared_ptr<PulseTimerRecord> &timer) {
		if (!timer || timer->state != PulseTimerState::Running) {
			return;
		}
		removeActiveLocked(timer->id);
		if (activeTimerCount >= timerCapacity) {
			return;
		}
		activeTimers[activeTimerCount++] = timer;
		std::sort(
		    activeTimers.get(),
		    activeTimers.get() + activeTimerCount,
		    [](const std::shared_ptr<PulseTimerRecord> &left,
		       const std::shared_ptr<PulseTimerRecord> &right) {
			    if (!left) {
				    return false;
			    }
			    if (!right) {
				    return true;
			    }
			    if (left->nextDueMs == right->nextDueMs) {
				    return left->id < right->id;
			    }
			    return left->nextDueMs < right->nextDueMs;
		    }
		);
	}

	void incrementDroppedCommand() {
		PulseLock lock(mutex);
		if (lock) {
			droppedCommandCount++;
		}
	}

	PulseResult enqueueCommand(PulseCommandType type, PulseTimerId id, TickType_t timeout = 0) {
		PulseCommand command;
		command.type = type;
		command.id = id;

		if (commandQueue == nullptr || xQueueSend(commandQueue, &command, timeout) != pdTRUE) {
			incrementDroppedCommand();
			return PulseResult::failure(PulseStatus::QueueFull, "pulse command queue is full");
		}
		return PulseResult::success("command queued");
	}

	void processCommand(const PulseCommand &command) {
		const uint64_t nowMs = pulseNowMs();
		PulseLock lock(mutex);
		if (!lock) {
			return;
		}

		if (command.type == PulseCommandType::Shutdown) {
			stopping = true;
			return;
		}

		auto timer = findTimerLocked(command.id);
		if (!timer) {
			return;
		}

		if (command.type == PulseCommandType::Add) {
			if (timer->state == PulseTimerState::Running) {
				insertActiveLocked(timer);
			}
			return;
		}

		if (command.type == PulseCommandType::Clear) {
			removeRegistryLocked(command.id);
			return;
		}

		if (command.type == PulseCommandType::Pause) {
			if (timer->state == PulseTimerState::Running) {
				timer->remainingDelayMs =
				    timer->nextDueMs > nowMs ? static_cast<uint32_t>(timer->nextDueMs - nowMs) : 1;
				timer->state = PulseTimerState::Paused;
				removeActiveLocked(timer->id);
			}
			return;
		}

		if (command.type == PulseCommandType::Resume) {
			if (timer->state == PulseTimerState::Paused) {
				const uint32_t delayMs = timer->remainingDelayMs > 0 ? timer->remainingDelayMs : 1;
				timer->nextDueMs = nowMs + delayMs;
				timer->remainingDelayMs = 0;
				timer->state = PulseTimerState::Running;
				insertActiveLocked(timer);
			}
			return;
		}

		if (command.type == PulseCommandType::Restart) {
			timer->state = PulseTimerState::Running;
			timer->remainingDelayMs = 0;
			if (timer->type == PulseTimerType::Countdown) {
				timer->elapsedMs = 0;
				timer->nextDueMs = nowMs + timer->tickMs;
			} else if (timer->type == PulseTimerType::Interval) {
				timer->nextDueMs = nowMs + timer->intervalMs;
			} else {
				timer->nextDueMs = nowMs + timer->delayMs;
			}
			insertActiveLocked(timer);
		}
	}

	TickType_t nextWaitTicks() {
		PulseLock lock(mutex);
		if (!lock || stopping) {
			return 0;
		}
		while (activeTimerCount > 0 && !activeTimers[0]) {
			removeActiveLocked(kInvalidTimerId);
		}
		if (activeTimerCount == 0) {
			return portMAX_DELAY;
		}
		const uint64_t nowMs = pulseNowMs();
		if (activeTimers[0]->nextDueMs <= nowMs) {
			return 0;
		}
		uint64_t remainingMs = activeTimers[0]->nextDueMs - nowMs;
		if (remainingMs > UINT32_MAX) {
			remainingMs = UINT32_MAX;
		}
		TickType_t ticks = pdMS_TO_TICKS(static_cast<uint32_t>(remainingMs));
		return ticks > 0 ? ticks : 1;
	}

	void processQueuedCommands(TickType_t waitTicks) {
		PulseCommand command;
		if (xQueueReceive(commandQueue, &command, waitTicks) != pdTRUE) {
			return;
		}
		processCommand(command);

		while (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
			processCommand(command);
		}
	}

	bool takeDueTimer(std::shared_ptr<PulseTimerRecord> &out, uint64_t &dueMs) {
		PulseLock lock(mutex);
		if (!lock) {
			return false;
		}
		const uint64_t nowMs = pulseNowMs();
		while (activeTimerCount > 0) {
			auto timer = activeTimers[0];
			if (!timer) {
				removeActiveLocked(kInvalidTimerId);
				continue;
			}
			if (timer->nextDueMs > nowMs) {
				return false;
			}
			removeActiveLocked(timer->id);
			if (timer->state != PulseTimerState::Running || !hasTimerLocked(timer->id)) {
				continue;
			}
			dueMs = timer->nextDueMs;
			if (nowMs > timer->nextDueMs) {
				lateCallbackCount++;
			}
			out = timer;
			return true;
		}
		return false;
	}

	void executeTimer(const std::shared_ptr<PulseTimerRecord> &timer, uint64_t dueMs) {
		if (!timer) {
			return;
		}

		PulseCallback callback;
		PulseCountdownCallback countdownCallback;
		PulseCountdownTick tick;
		bool finishedCountdown = false;
		bool shouldRun = false;

		{
			PulseLock lock(mutex);
			if (!lock || timer->state != PulseTimerState::Running || !hasTimerLocked(timer->id)) {
				return;
			}

			if (timer->type == PulseTimerType::Timeout) {
				callback = timer->callback;
				removeRegistryLocked(timer->id);
				shouldRun = static_cast<bool>(callback);
			} else if (timer->type == PulseTimerType::Interval) {
				callback = timer->callback;
				shouldRun = static_cast<bool>(callback);
			} else {
				const uint32_t remainingBefore =
				    timer->durationMs > timer->elapsedMs ? timer->durationMs - timer->elapsedMs : 0;
				const uint32_t stepMs = std::min(timer->tickMs, remainingBefore);
				timer->elapsedMs = std::min(timer->durationMs, timer->elapsedMs + stepMs);
				tick.elapsedMs = timer->elapsedMs;
				tick.remainingMs =
				    timer->durationMs > timer->elapsedMs ? timer->durationMs - timer->elapsedMs : 0;
				tick.elapsedSeconds = tick.elapsedMs / 1000;
				tick.remainingSeconds = tick.remainingMs / 1000;
				tick.isFinished = timer->elapsedMs >= timer->durationMs;
				finishedCountdown = tick.isFinished;
				countdownCallback = timer->countdownCallback;
				shouldRun = static_cast<bool>(countdownCallback);
			}
		}

		(void)dueMs;
		if (callback) {
			callback();
		} else if (countdownCallback) {
			countdownCallback(tick);
		}

		{
			PulseLock lock(mutex);
			if (!lock) {
				return;
			}
			if (shouldRun) {
				executedCallbackCount++;
			}

			if (timer->type == PulseTimerType::Interval) {
				if (timer->state == PulseTimerState::Running && hasTimerLocked(timer->id)) {
					timer->nextDueMs = pulseNowMs() + timer->intervalMs;
					insertActiveLocked(timer);
				}
				return;
			}

			if (timer->type == PulseTimerType::Countdown) {
				if (finishedCountdown) {
					removeRegistryLocked(timer->id);
				} else if (timer->state == PulseTimerState::Running && hasTimerLocked(timer->id)) {
					const uint32_t remainingMs =
					    timer->durationMs > timer->elapsedMs ? timer->durationMs - timer->elapsedMs : 0;
					const uint32_t nextDelayMs = std::min(timer->tickMs, remainingMs);
					timer->nextDueMs = pulseNowMs() + nextDelayMs;
					insertActiveLocked(timer);
				}
			}
		}
	}

	void processDueTimers() {
		while (true) {
			std::shared_ptr<PulseTimerRecord> timer;
			uint64_t dueMs = 0;
			if (!takeDueTimer(timer, dueMs)) {
				return;
			}
			executeTimer(timer, dueMs);
		}
	}

	void taskLoop() {
		while (true) {
			processQueuedCommands(nextWaitTicks());
			{
				PulseLock lock(mutex);
				if (lock && stopping) {
					break;
				}
			}
			processDueTimers();
		}
	}

	void markTaskStopped() {
		PulseLock lock(mutex);
		if (lock) {
			stackHighWaterMarkBytes = pulse_task_support::currentStackHighWaterMarkBytes();
			taskHandle = nullptr;
			taskStopped = true;
		}
	}

	static void taskEntry(void *arg) {
		PulseImpl *impl = static_cast<PulseImpl *>(arg);
		if (impl == nullptr) {
			vTaskDelete(nullptr);
			return;
		}
		impl->taskLoop();
		const bool withCaps = impl->createdWithCaps;
		impl->markTaskStopped();
		pulse_task_support::deleteCurrentTask(withCaps);
	}
};

PulseResult PulseResult::success(const char *message) {
	PulseResult result;
	result.result = true;
	result.status = PulseStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	return result;
}

PulseResult PulseResult::failure(PulseStatus status, const char *message) {
	PulseResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	return result;
}

PulseTimerResult PulseTimerResult::success(PulseTimerId id, const char *message) {
	PulseTimerResult result;
	result.result = true;
	result.status = PulseStatus::Ok;
	result.message = message != nullptr ? message : "ok";
	result.id = id;
	return result;
}

PulseTimerResult PulseTimerResult::failure(
    PulseStatus status,
    const char *message,
    PulseTimerId id
) {
	PulseTimerResult result;
	result.result = false;
	result.status = status;
	result.message = message != nullptr ? message : "error";
	result.id = id;
	return result;
}

Pulse::Pulse() : _impl(std::make_unique<PulseImpl>()) {
}

Pulse::~Pulse() {
	end(2000);
}

PulseResult Pulse::init(const PulseConfig &config) {
	if (!pulse_task_support::isValidStackSize(config.stackSizeBytes)) {
		return PulseResult::failure(
		    PulseStatus::InvalidArgument,
		    "stack size must be at least 1024 bytes and aligned"
		);
	}
	if (config.commandQueueSize == 0) {
		return PulseResult::failure(PulseStatus::InvalidArgument, "command queue size is required");
	}
	if (addWouldOverflow(config.maxTimeouts, config.maxIntervals) ||
	    addWouldOverflow(config.maxTimeouts + config.maxIntervals, config.maxCountdowns)) {
		return PulseResult::failure(PulseStatus::InvalidArgument, "timer capacity overflow");
	}
	const uint32_t timerCapacity =
	    config.maxTimeouts + config.maxIntervals + config.maxCountdowns;
	if (timerCapacity == 0) {
		return PulseResult::failure(
		    PulseStatus::InvalidArgument,
		    "at least one timer slot is required"
		);
	}

	bool usePsramStack = false;
	PulseStackType actualStackType = PulseStackType::Internal;
	if (config.stackType == PulseStackType::Psram) {
		if (!pulse_task_support::hasExternalStackSupport()) {
			return PulseResult::failure(
			    PulseStatus::TaskCreateFailed,
			    "PSRAM task stacks are not available"
			);
		}
		usePsramStack = true;
		actualStackType = PulseStackType::Psram;
	} else if (
	    config.stackType == PulseStackType::Auto && pulse_task_support::hasExternalStackSupport()
	) {
		usePsramStack = true;
		actualStackType = PulseStackType::Psram;
	}

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (_impl->initialized) {
			return PulseResult::failure(PulseStatus::AlreadyInitialized, "pulse already initialized");
		}
		_impl->config = config;
		_impl->actualStackType = actualStackType;
		_impl->stopping = false;
		_impl->taskStopped = false;
		_impl->createdWithCaps = false;
		_impl->executedCallbackCount = 0;
		_impl->droppedCommandCount = 0;
		_impl->lateCallbackCount = 0;
		_impl->stackHighWaterMarkBytes = 0;
		_impl->nextTimerId = 1;
		_impl->timerCapacity = timerCapacity;
		_impl->timerCount = 0;
		_impl->activeTimerCount = 0;
		_impl->timers.reset(
		    new (std::nothrow) std::shared_ptr<PulseTimerRecord>[timerCapacity]
		);
		_impl->activeTimers.reset(
		    new (std::nothrow) std::shared_ptr<PulseTimerRecord>[timerCapacity]
		);
		if (!_impl->timers || !_impl->activeTimers) {
			_impl->timers.reset();
			_impl->activeTimers.reset();
			_impl->timerCapacity = 0;
			_impl->taskStopped = true;
			return PulseResult::failure(
			    PulseStatus::OutOfMemory,
			    "failed to allocate timer storage"
			);
		}
	}

	_impl->commandQueue = xQueueCreate(config.commandQueueSize, sizeof(PulseCommand));
	if (_impl->commandQueue == nullptr) {
		PulseLock lock(_impl->mutex);
		if (lock) {
			_impl->taskStopped = true;
			_impl->timers.reset();
			_impl->activeTimers.reset();
			_impl->timerCapacity = 0;
		}
		return PulseResult::failure(PulseStatus::QueueCreateFailed, "failed to create command queue");
	}

	TaskHandle_t handle = nullptr;
	bool createdWithCaps = false;
	const BaseType_t created = pulse_task_support::createTask(
	    &PulseImpl::taskEntry,
	    config.taskName != nullptr ? config.taskName : "pulse-task",
	    config.stackSizeBytes,
	    _impl.get(),
	    config.priority,
	    &handle,
	    config.coreId,
	    usePsramStack,
	    createdWithCaps
	);
	if (created != pdPASS || handle == nullptr) {
		vQueueDelete(_impl->commandQueue);
		_impl->commandQueue = nullptr;
		PulseLock lock(_impl->mutex);
		if (lock) {
			_impl->taskStopped = true;
			_impl->timers.reset();
			_impl->activeTimers.reset();
			_impl->timerCapacity = 0;
		}
		return PulseResult::failure(PulseStatus::TaskCreateFailed, "failed to create pulse task");
	}

	{
		PulseLock lock(_impl->mutex);
		if (lock) {
			_impl->taskHandle = handle;
			_impl->createdWithCaps = createdWithCaps;
			_impl->initialized = true;
		}
	}
	return PulseResult::success("pulse initialized");
}

PulseResult Pulse::end(uint32_t timeoutMs) {
	QueueHandle_t queue = nullptr;
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized) {
			return PulseResult::success("pulse not initialized");
		}
		_impl->stopping = true;
		queue = _impl->commandQueue;
	}

	if (queue != nullptr) {
		PulseResult shutdownResult =
		    _impl->enqueueCommand(PulseCommandType::Shutdown, kInvalidTimerId, pdMS_TO_TICKS(timeoutMs));
		if (!shutdownResult) {
			if (shutdownResult.status == PulseStatus::QueueFull) {
				return PulseResult::failure(PulseStatus::Timeout, "pulse end timed out");
			}
			return shutdownResult;
		}
	}

	const uint64_t startMs = pulseNowMs();
	while (true) {
		bool stopped = false;
		{
			PulseLock lock(_impl->mutex);
			if (lock) {
				stopped = _impl->taskStopped || _impl->taskHandle == nullptr;
			}
		}
		if (stopped) {
			break;
		}
		if (pulseNowMs() - startMs >= timeoutMs) {
			return PulseResult::failure(PulseStatus::Timeout, "pulse end timed out");
		}
		vTaskDelay(pdMS_TO_TICKS(kWaitPollMs));
	}

	{
		PulseLock lock(_impl->mutex);
		if (lock) {
			for (uint32_t index = 0; index < _impl->timerCount; index++) {
				_impl->timers[index].reset();
			}
			for (uint32_t index = 0; index < _impl->activeTimerCount; index++) {
				_impl->activeTimers[index].reset();
			}
			_impl->timerCount = 0;
			_impl->activeTimerCount = 0;
			_impl->timerCapacity = 0;
			_impl->timers.reset();
			_impl->activeTimers.reset();
			_impl->initialized = false;
			_impl->stopping = false;
			_impl->taskStopped = true;
			_impl->nextTimerId = 1;
		}
	}
	if (_impl->commandQueue != nullptr) {
		vQueueDelete(_impl->commandQueue);
		_impl->commandQueue = nullptr;
	}
	return PulseResult::success("pulse ended");
}

PulseTimerResult Pulse::setTimeout(PulseCallback callback, uint32_t delayMs) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (delayMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "delay is required");
	}

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		PulseTimerResult preflight =
		    _impl->validateTimerCreateLocked(PulseTimerType::Timeout, "timeout limit reached");
		if (!preflight) {
			return preflight;
		}
	}

	PulseTimerRecord *rawTimer = new (std::nothrow) PulseTimerRecord();
	if (rawTimer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	std::shared_ptr<PulseTimerRecord> timer(rawTimer);

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		PulseTimerResult preflight =
		    _impl->validateTimerCreateLocked(PulseTimerType::Timeout, "timeout limit reached");
		if (!preflight) {
			return preflight;
		}
		if (!_impl->allocateTimerIdLocked(timer->id)) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "timer id exhausted");
		}
		timer->type = PulseTimerType::Timeout;
		timer->state = PulseTimerState::Running;
		timer->callback = callback;
		timer->delayMs = delayMs;
		timer->nextDueMs = pulseNowMs() + delayMs;
		if (!_impl->addRegistryLocked(timer)) {
			return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
		}
	}

	PulseResult queued = _impl->enqueueCommand(PulseCommandType::Add, timer->id);
	if (!queued) {
		PulseLock lock(_impl->mutex);
		if (lock) {
			_impl->removeRegistryLocked(timer->id);
		}
		return PulseTimerResult::failure(queued.status, queued.message, timer->id);
	}
	return PulseTimerResult::success(timer->id, "timeout scheduled");
}

PulseTimerResult Pulse::setInterval(PulseCallback callback, uint32_t intervalMs) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (intervalMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "interval is required");
	}

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		PulseTimerResult preflight =
		    _impl->validateTimerCreateLocked(PulseTimerType::Interval, "interval limit reached");
		if (!preflight) {
			return preflight;
		}
	}

	PulseTimerRecord *rawTimer = new (std::nothrow) PulseTimerRecord();
	if (rawTimer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	std::shared_ptr<PulseTimerRecord> timer(rawTimer);

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		PulseTimerResult preflight =
		    _impl->validateTimerCreateLocked(PulseTimerType::Interval, "interval limit reached");
		if (!preflight) {
			return preflight;
		}
		if (!_impl->allocateTimerIdLocked(timer->id)) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "timer id exhausted");
		}
		timer->type = PulseTimerType::Interval;
		timer->state = PulseTimerState::Running;
		timer->callback = callback;
		timer->intervalMs = intervalMs;
		timer->nextDueMs = pulseNowMs() + intervalMs;
		if (!_impl->addRegistryLocked(timer)) {
			return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
		}
	}

	PulseResult queued = _impl->enqueueCommand(PulseCommandType::Add, timer->id);
	if (!queued) {
		PulseLock lock(_impl->mutex);
		if (lock) {
			_impl->removeRegistryLocked(timer->id);
		}
		return PulseTimerResult::failure(queued.status, queued.message, timer->id);
	}
	return PulseTimerResult::success(timer->id, "interval scheduled");
}

PulseTimerResult Pulse::setCountdown(
    const PulseCountdownConfig &config,
    PulseCountdownCallback callback
) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (config.durationMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "duration is required");
	}
	if (config.tickMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "tick is required");
	}
	if (config.tickMs > config.durationMs) {
		return PulseTimerResult::failure(
		    PulseStatus::InvalidArgument,
		    "tick must be less than or equal to duration"
		);
	}

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		PulseTimerResult preflight =
		    _impl->validateTimerCreateLocked(PulseTimerType::Countdown, "countdown limit reached");
		if (!preflight) {
			return preflight;
		}
	}

	PulseTimerRecord *rawTimer = new (std::nothrow) PulseTimerRecord();
	if (rawTimer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	std::shared_ptr<PulseTimerRecord> timer(rawTimer);

	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		PulseTimerResult preflight =
		    _impl->validateTimerCreateLocked(PulseTimerType::Countdown, "countdown limit reached");
		if (!preflight) {
			return preflight;
		}
		if (!_impl->allocateTimerIdLocked(timer->id)) {
			return PulseTimerResult::failure(PulseStatus::InternalError, "timer id exhausted");
		}
		timer->type = PulseTimerType::Countdown;
		timer->state = PulseTimerState::Running;
		timer->countdownCallback = callback;
		timer->durationMs = config.durationMs;
		timer->tickMs = config.tickMs;
		timer->elapsedMs = 0;
		timer->nextDueMs = pulseNowMs() + config.tickMs;
		if (!_impl->addRegistryLocked(timer)) {
			return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
		}
	}

	PulseResult queued = _impl->enqueueCommand(PulseCommandType::Add, timer->id);
	if (!queued) {
		PulseLock lock(_impl->mutex);
		if (lock) {
			_impl->removeRegistryLocked(timer->id);
		}
		return PulseTimerResult::failure(queued.status, queued.message, timer->id);
	}
	return PulseTimerResult::success(timer->id, "countdown scheduled");
}

PulseResult Pulse::clear(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		if (!_impl->hasTimerLocked(id)) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
	}
	return _impl->enqueueCommand(PulseCommandType::Clear, id);
}

PulseResult Pulse::clearTimeout(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		auto timer = _impl->findTimerLocked(id);
		if (!timer) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
		if (timer->type != PulseTimerType::Timeout) {
			return PulseResult::failure(PulseStatus::InvalidArgument, "timer is not a timeout");
		}
	}
	return clear(id);
}

PulseResult Pulse::clearInterval(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		auto timer = _impl->findTimerLocked(id);
		if (!timer) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
		if (timer->type != PulseTimerType::Interval) {
			return PulseResult::failure(PulseStatus::InvalidArgument, "timer is not an interval");
		}
	}
	return clear(id);
}

PulseResult Pulse::clearCountdown(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		auto timer = _impl->findTimerLocked(id);
		if (!timer) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
		if (timer->type != PulseTimerType::Countdown) {
			return PulseResult::failure(PulseStatus::InvalidArgument, "timer is not a countdown");
		}
	}
	return clear(id);
}

PulseResult Pulse::pause(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		if (!_impl->hasTimerLocked(id)) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
	}
	return _impl->enqueueCommand(PulseCommandType::Pause, id);
}

PulseResult Pulse::resume(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		if (!_impl->hasTimerLocked(id)) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
	}
	return _impl->enqueueCommand(PulseCommandType::Resume, id);
}

PulseResult Pulse::restart(PulseTimerId id) {
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (!_impl->initialized || _impl->stopping) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		if (!_impl->hasTimerLocked(id)) {
			return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
		}
	}
	return _impl->enqueueCommand(PulseCommandType::Restart, id);
}

PulseTimerState Pulse::getState(PulseTimerId id) {
	PulseLock lock(_impl->mutex);
	if (!lock) {
		return PulseTimerState::NotFound;
	}
	auto timer = _impl->findTimerLocked(id);
	return timer ? timer->state : PulseTimerState::NotFound;
}

PulseDiag Pulse::getDiagnostics() {
	PulseDiag diag;
	TaskHandle_t handle = nullptr;
	{
		PulseLock lock(_impl->mutex);
		if (!lock) {
			return diag;
		}
		handle = _impl->taskHandle;
		diag.commandQueueSize = _impl->config.commandQueueSize;
		diag.commandQueueUsed =
		    _impl->commandQueue != nullptr ?
		        static_cast<uint32_t>(uxQueueMessagesWaiting(_impl->commandQueue)) :
		        0;
		diag.executedCallbackCount = _impl->executedCallbackCount;
		diag.droppedCommandCount = _impl->droppedCommandCount;
		diag.lateCallbackCount = _impl->lateCallbackCount;
		diag.stackHighWaterMarkBytes = _impl->stackHighWaterMarkBytes;
		diag.requestedStackType = _impl->config.stackType;
		diag.actualStackType = _impl->actualStackType;

		for (uint32_t index = 0; index < _impl->timerCount; index++) {
			const auto &timer = _impl->timers[index];
			if (!timer) {
				continue;
			}
			if (timer->type == PulseTimerType::Timeout) {
				diag.timeoutCount++;
			} else if (timer->type == PulseTimerType::Interval) {
				diag.intervalCount++;
			} else {
				diag.countdownCount++;
			}
			if (timer->state == PulseTimerState::Running) {
				diag.runningCount++;
			} else if (timer->state == PulseTimerState::Paused) {
				diag.pausedCount++;
			}
		}
	}
	if (handle != nullptr) {
		diag.stackHighWaterMarkBytes = pulse_task_support::stackHighWaterMarkBytes(handle);
	}
	return diag;
}

const char *Pulse::statusToString(PulseStatus status) const {
	switch (status) {
	case PulseStatus::Ok:
		return "ok";
	case PulseStatus::NotInitialized:
		return "notInitialized";
	case PulseStatus::AlreadyInitialized:
		return "alreadyInitialized";
	case PulseStatus::InvalidArgument:
		return "invalidArgument";
	case PulseStatus::OutOfMemory:
		return "outOfMemory";
	case PulseStatus::TaskCreateFailed:
		return "taskCreateFailed";
	case PulseStatus::QueueCreateFailed:
		return "queueCreateFailed";
	case PulseStatus::TimerNotFound:
		return "timerNotFound";
	case PulseStatus::QueueFull:
		return "queueFull";
	case PulseStatus::Busy:
		return "busy";
	case PulseStatus::Timeout:
		return "timeout";
	case PulseStatus::InternalError:
		return "internalError";
	}
	return "unknown";
}

const char *Pulse::timerStateToString(PulseTimerState state) const {
	switch (state) {
	case PulseTimerState::Running:
		return "running";
	case PulseTimerState::Paused:
		return "paused";
	case PulseTimerState::NotFound:
		return "notFound";
	}
	return "unknown";
}

const char *Pulse::timerTypeToString(PulseTimerType type) const {
	switch (type) {
	case PulseTimerType::Timeout:
		return "timeout";
	case PulseTimerType::Interval:
		return "interval";
	case PulseTimerType::Countdown:
		return "countdown";
	}
	return "unknown";
}
