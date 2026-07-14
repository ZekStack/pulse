#include "Pulse.h"

#include "internal/PulseMutex.h"
#include "internal/PulseTaskSupport.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include <esp_timer.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace {
constexpr PulseTimerId kInvalidTimerId = 0;
constexpr uint32_t kWaitPollMs = 10;
constexpr EventBits_t kCompletionBit = BIT0;

enum class PulseLifecycleState : uint8_t {
	Uninitialized,
	Running,
	Stopping,
	Stopped,
};

enum class PulseExecutionState : uint8_t {
	Idle,
	ExecutingCallback,
};

enum class PulseCommandType : uint8_t {
	Add,
	Clear,
	Pause,
	Resume,
	Restart,
};

struct PulseCommand {
	PulseCommandType type = PulseCommandType::Add;
	PulseTimerId id = kInvalidTimerId;
	uint64_t generation = 0;
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

TickType_t waitTicksForMs(uint32_t milliseconds) {
	const TickType_t ticks = pdMS_TO_TICKS(milliseconds);
	return ticks > 0 ? ticks : 1;
}
} // namespace

struct PulseTimerRecord {
	PulseTimerId id = kInvalidTimerId;
	PulseTimerType type = PulseTimerType::Timeout;
	PulseTimerState state = PulseTimerState::Running;
	PulseExecutionState executionState = PulseExecutionState::Idle;
	PulseCallback callback;
	PulseCountdownCallback countdownCallback;
	uint32_t delayMs = 0;
	uint32_t intervalMs = 0;
	uint32_t durationMs = 0;
	uint32_t tickMs = 0;
	uint32_t elapsedMs = 0;
	uint32_t remainingDelayMs = 0;
	uint32_t callbackNextDelayMs = 0;
	uint32_t mutationGeneration = 0;
	uint64_t nextDueMs = 0;
};

struct PulseImpl {
	PulseImpl() : completionEvent(xEventGroupCreate()) {
	}

	~PulseImpl() {
		if (completionEvent != nullptr) {
			vEventGroupDelete(completionEvent);
		}
	}

	PulseImpl(const PulseImpl &) = delete;
	PulseImpl &operator=(const PulseImpl &) = delete;

	bool valid() const {
		return mutex.valid() && completionEvent != nullptr;
	}

	void retain() noexcept {
		referenceCount.fetch_add(1, std::memory_order_relaxed);
	}

	void release() noexcept {
		if (referenceCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			delete this;
		}
	}

	std::atomic<uint32_t> referenceCount{1};
	PulseConfig config{};
	PulseMutex mutex;
	EventGroupHandle_t completionEvent = nullptr;
	std::unique_ptr<std::shared_ptr<PulseTimerRecord>[]> timers;
	std::unique_ptr<std::shared_ptr<PulseTimerRecord>[]> activeTimers;
	uint32_t timerCapacity = 0;
	uint32_t timerCount = 0;
	uint32_t activeTimerCount = 0;
	QueueHandle_t commandQueue = nullptr;
	TaskHandle_t taskHandle = nullptr;
	PulseLifecycleState lifecycle = PulseLifecycleState::Uninitialized;
	uint64_t lifecycleGeneration = 0;
	bool createdWithCaps = false;
	PulseStackType actualStackType = PulseStackType::Internal;
	PulseTimerId nextTimerId = 1;
	uint32_t executedCallbackCount = 0;
	uint32_t droppedCommandCount = 0;
	uint32_t lateCallbackCount = 0;
	size_t finalStackHighWaterMarkBytes = 0;

	bool isCurrentTaskLocked() const {
		return taskHandle != nullptr && xTaskGetCurrentTaskHandle() == taskHandle;
	}

	bool isRunningGenerationLocked(uint64_t generation) const {
		return lifecycle == PulseLifecycleState::Running && lifecycleGeneration == generation;
	}

	PulseResult unavailableResultLocked() const {
		if (lifecycle == PulseLifecycleState::Uninitialized) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
		return PulseResult::failure(PulseStatus::Busy, "pulse lifecycle is not running");
	}

	PulseTimerResult unavailableTimerResultLocked() const {
		if (lifecycle == PulseLifecycleState::Uninitialized) {
			return PulseTimerResult::failure(
			    PulseStatus::NotInitialized,
			    "pulse is not initialized"
			);
		}
		return PulseTimerResult::failure(PulseStatus::Busy, "pulse lifecycle is not running");
	}

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
		if (lifecycle != PulseLifecycleState::Running) {
			return unavailableTimerResultLocked();
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

	std::shared_ptr<PulseTimerRecord> removeRegistryLocked(PulseTimerId id) {
		removeActiveLocked(id);
		std::shared_ptr<PulseTimerRecord> removed;
		uint32_t writeIndex = 0;
		for (uint32_t readIndex = 0; readIndex < timerCount; readIndex++) {
			auto &timer = timers[readIndex];
			if (timer && timer->id == id) {
				if (!removed) {
					removed = timer;
				}
				continue;
			}
			if (timer) {
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
		return removed;
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

	void notifyTaskLocked() {
		if (taskHandle != nullptr) {
			xTaskNotifyGive(taskHandle);
		}
	}

	PulseResult requestStopLocked() {
		if (lifecycle == PulseLifecycleState::Uninitialized ||
		    lifecycle == PulseLifecycleState::Stopped) {
			return PulseResult::success("pulse already stopped");
		}
		if (lifecycle == PulseLifecycleState::Running) {
			lifecycle = PulseLifecycleState::Stopping;
		}
		notifyTaskLocked();
		return PulseResult::success("pulse shutdown requested");
	}

	PulseResult enqueueCommandLocked(PulseCommandType type, PulseTimerId id) {
		if (lifecycle != PulseLifecycleState::Running) {
			return unavailableResultLocked();
		}
		if (commandQueue == nullptr || taskHandle == nullptr) {
			return PulseResult::failure(PulseStatus::InternalError, "pulse scheduler is unavailable");
		}

		PulseCommand command;
		command.type = type;
		command.id = id;
		command.generation = lifecycleGeneration;

		if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
			droppedCommandCount++;
			return PulseResult::failure(PulseStatus::QueueFull, "pulse command queue is full");
		}
		notifyTaskLocked();
		return PulseResult::success("command queued");
	}

	void processCommand(const PulseCommand &command) {
		std::shared_ptr<PulseTimerRecord> detached;
		{
			PulseLock lock(mutex);
			if (!lock || !isRunningGenerationLocked(command.generation)) {
				return;
			}

			auto timer = findTimerLocked(command.id);
			if (!timer) {
				return;
			}

			const uint64_t nowMs = pulseNowMs();
			if (command.type == PulseCommandType::Add) {
				if (timer->state == PulseTimerState::Running) {
					insertActiveLocked(timer);
				}
			} else if (command.type == PulseCommandType::Clear) {
				detached = removeRegistryLocked(command.id);
			} else if (command.type == PulseCommandType::Pause) {
				if (timer->state == PulseTimerState::Running) {
					if (timer->executionState == PulseExecutionState::ExecutingCallback &&
					    (timer->type == PulseTimerType::Interval ||
					     timer->type == PulseTimerType::Countdown)) {
						timer->remainingDelayMs =
						    timer->callbackNextDelayMs > 0 ? timer->callbackNextDelayMs : 1;
					} else {
						timer->remainingDelayMs =
						    timer->nextDueMs > nowMs ?
						        static_cast<uint32_t>(timer->nextDueMs - nowMs) :
						        1;
					}
					timer->state = PulseTimerState::Paused;
					timer->mutationGeneration++;
					removeActiveLocked(timer->id);
				}
			} else if (command.type == PulseCommandType::Resume) {
				if (timer->state == PulseTimerState::Paused) {
					const uint32_t delayMs =
					    timer->remainingDelayMs > 0 ? timer->remainingDelayMs : 1;
					timer->nextDueMs = nowMs + delayMs;
					timer->remainingDelayMs = 0;
					timer->state = PulseTimerState::Running;
					timer->mutationGeneration++;
					insertActiveLocked(timer);
				}
			} else if (command.type == PulseCommandType::Restart) {
				timer->state = PulseTimerState::Running;
				timer->remainingDelayMs = 0;
				timer->mutationGeneration++;
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
		detached.reset();
	}

	uint32_t queueUsageLocked() const {
		return commandQueue != nullptr ?
		           static_cast<uint32_t>(uxQueueMessagesWaiting(commandQueue)) :
		           0;
	}

	uint32_t queueUsage() {
		PulseLock lock(mutex);
		if (!lock || lifecycle != PulseLifecycleState::Running) {
			return 0;
		}
		return std::min(queueUsageLocked(), config.commandQueueSize);
	}

	void processQueuedCommandsSnapshot(uint32_t commandCount, uint64_t generation) {
		const uint32_t boundedCount = std::min(commandCount, config.commandQueueSize);
		for (uint32_t index = 0; index < boundedCount; index++) {
			QueueHandle_t queue = nullptr;
			{
				PulseLock lock(mutex);
				if (!lock || !isRunningGenerationLocked(generation)) {
					return;
				}
				queue = commandQueue;
			}
			if (queue == nullptr) {
				return;
			}

			PulseCommand command;
			if (xQueueReceive(queue, &command, 0) != pdTRUE) {
				return;
			}
			processCommand(command);
		}
	}

	TickType_t nextWaitTicks() {
		PulseLock lock(mutex);
		if (!lock || lifecycle != PulseLifecycleState::Running) {
			return 0;
		}
		if (queueUsageLocked() > 0) {
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
		return waitTicksForMs(static_cast<uint32_t>(remainingMs));
	}

	bool takeDueTimer(std::shared_ptr<PulseTimerRecord> &out, uint64_t &dueMs) {
		PulseLock lock(mutex);
		if (!lock || lifecycle != PulseLifecycleState::Running || queueUsageLocked() > 0) {
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

		PulseCountdownTick tick;
		bool finishedCountdown = false;
		bool shouldRun = false;
		bool terminal = false;
		uint32_t generationBeforeCallback = 0;
		uint64_t runGeneration = 0;

		{
			PulseLock lock(mutex);
			if (!lock || lifecycle != PulseLifecycleState::Running ||
			    timer->state != PulseTimerState::Running || !hasTimerLocked(timer->id)) {
				return;
			}

			runGeneration = lifecycleGeneration;
			generationBeforeCallback = timer->mutationGeneration;
			if (timer->type == PulseTimerType::Timeout) {
				terminal = true;
				shouldRun = static_cast<bool>(timer->callback);
				(void)removeRegistryLocked(timer->id);
			} else if (timer->type == PulseTimerType::Interval) {
				timer->executionState = PulseExecutionState::ExecutingCallback;
				timer->callbackNextDelayMs = timer->intervalMs;
				shouldRun = static_cast<bool>(timer->callback);
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
				terminal = finishedCountdown;
				shouldRun = static_cast<bool>(timer->countdownCallback);
				if (finishedCountdown) {
					(void)removeRegistryLocked(timer->id);
				} else {
					timer->executionState = PulseExecutionState::ExecutingCallback;
					timer->callbackNextDelayMs = std::min(timer->tickMs, tick.remainingMs);
				}
			}
		}

		(void)dueMs;
		if (timer->type == PulseTimerType::Countdown) {
			if (timer->countdownCallback) {
				timer->countdownCallback(tick);
			}
		} else if (timer->callback) {
			timer->callback();
		}

		{
			PulseLock lock(mutex);
			if (lock && shouldRun) {
				executedCallbackCount++;
			}
		}

		uint32_t queuedAtCallbackCompletion = 0;
		{
			PulseLock lock(mutex);
			if (lock && isRunningGenerationLocked(runGeneration)) {
				queuedAtCallbackCompletion =
				    std::min(queueUsageLocked(), config.commandQueueSize);
			}
		}
		if (queuedAtCallbackCompletion > 0) {
			processQueuedCommandsSnapshot(queuedAtCallbackCompletion, runGeneration);
		}

		{
			PulseLock lock(mutex);
			if (!lock) {
				return;
			}
			timer->executionState = PulseExecutionState::Idle;
			if (!isRunningGenerationLocked(runGeneration) || terminal ||
			    !hasTimerLocked(timer->id)) {
				return;
			}
			if (timer->state == PulseTimerState::Running &&
			    timer->mutationGeneration == generationBeforeCallback) {
				const uint32_t nextDelayMs =
				    timer->callbackNextDelayMs > 0 ? timer->callbackNextDelayMs : 1;
				timer->nextDueMs = pulseNowMs() + nextDelayMs;
				insertActiveLocked(timer);
			}
		}
	}

	bool running() {
		PulseLock lock(mutex);
		return lock && lifecycle == PulseLifecycleState::Running;
	}

	uint64_t currentGeneration() {
		PulseLock lock(mutex);
		return lock ? lifecycleGeneration : 0;
	}

	void taskLoop() {
		while (running()) {
			const uint64_t generation = currentGeneration();
			const uint32_t pendingCommands = queueUsage();
			if (pendingCommands > 0) {
				processQueuedCommandsSnapshot(pendingCommands, generation);
				continue;
			}

			std::shared_ptr<PulseTimerRecord> timer;
			uint64_t dueMs = 0;
			if (takeDueTimer(timer, dueMs)) {
				executeTimer(timer, dueMs);
				continue;
			}

			const TickType_t waitTicks = nextWaitTicks();
			if (waitTicks == 0) {
				continue;
			}
			ulTaskNotifyTake(pdTRUE, waitTicks);
		}
	}

	void quiesceAndDeleteSelf() {
		std::unique_ptr<std::shared_ptr<PulseTimerRecord>[]> detachedTimers;
		std::unique_ptr<std::shared_ptr<PulseTimerRecord>[]> detachedActiveTimers;
		QueueHandle_t detachedQueue = nullptr;
		bool withCaps = false;

		{
			PulseLock lock(mutex);
			if (lock) {
				detachedTimers = std::move(timers);
				detachedActiveTimers = std::move(activeTimers);
				detachedQueue = commandQueue;
				commandQueue = nullptr;
				timerCapacity = 0;
				timerCount = 0;
				activeTimerCount = 0;
				nextTimerId = 1;
				withCaps = createdWithCaps;
			}
		}

		detachedActiveTimers.reset();
		detachedTimers.reset();
		if (detachedQueue != nullptr) {
			vQueueDelete(detachedQueue);
		}

		const size_t finalStack = pulse_task_support::currentStackHighWaterMarkBytes();
		{
			PulseLock lock(mutex);
			if (lock) {
				finalStackHighWaterMarkBytes = finalStack;
				taskHandle = nullptr;
				lifecycle = PulseLifecycleState::Stopped;
				if (completionEvent != nullptr) {
					xEventGroupSetBits(completionEvent, kCompletionBit);
				}
			}
		}

		release();
		pulse_task_support::deleteCurrentTask(withCaps);
		for (;;) {
			vTaskDelay(portMAX_DELAY);
		}
	}

	static void taskEntry(void *arg) {
		PulseImpl *impl = static_cast<PulseImpl *>(arg);
		if (impl == nullptr) {
			vTaskDelete(nullptr);
			return;
		}
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		impl->taskLoop();
		impl->quiesceAndDeleteSelf();
	}
};

namespace {
PulseResult endImpl(PulseImpl *impl, uint32_t timeoutMs, bool waitForever) {
	if (impl == nullptr) {
		return PulseResult::success("pulse not initialized");
	}

	uint64_t targetGeneration = 0;
	{
		PulseLock lock(impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (impl->lifecycle == PulseLifecycleState::Uninitialized) {
			return PulseResult::success("pulse not initialized");
		}
		if (impl->isCurrentTaskLocked()) {
			return PulseResult::failure(PulseStatus::Busy, "end cannot wait from the pulse task");
		}
		targetGeneration = impl->lifecycleGeneration;
		if (impl->lifecycle == PulseLifecycleState::Running) {
			impl->requestStopLocked();
		}
	}

	const uint64_t startMs = pulseNowMs();
	while (true) {
		{
			PulseLock lock(impl->mutex);
			if (!lock) {
				return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
			}
			if (impl->lifecycleGeneration != targetGeneration) {
				return PulseResult::success("target pulse generation already ended");
			}
			if (impl->lifecycle == PulseLifecycleState::Uninitialized) {
				return PulseResult::success("pulse ended");
			}
			if (impl->lifecycle == PulseLifecycleState::Stopped) {
				impl->lifecycle = PulseLifecycleState::Uninitialized;
				return PulseResult::success("pulse ended");
			}
			if (impl->lifecycle != PulseLifecycleState::Stopping) {
				return PulseResult::failure(
				    PulseStatus::InternalError,
				    "unexpected pulse lifecycle state"
				);
			}
		}

		if (!waitForever && pulseNowMs() - startMs >= timeoutMs) {
			return PulseResult::failure(PulseStatus::Timeout, "pulse end timed out");
		}

		uint32_t waitMs = kWaitPollMs;
		if (!waitForever) {
			const uint64_t elapsed = pulseNowMs() - startMs;
			const uint64_t remaining = timeoutMs > elapsed ? timeoutMs - elapsed : 0;
			if (remaining == 0) {
				return PulseResult::failure(PulseStatus::Timeout, "pulse end timed out");
			}
			waitMs = static_cast<uint32_t>(std::min<uint64_t>(remaining, kWaitPollMs));
		}
		xEventGroupWaitBits(
		    impl->completionEvent,
		    kCompletionBit,
		    pdFALSE,
		    pdFALSE,
		    waitTicksForMs(waitMs)
		);
	}
}

PulseResult queueControl(
    PulseImpl *impl,
    PulseTimerId id,
    PulseCommandType commandType,
    bool enforceType,
    PulseTimerType expectedType
) {
	if (impl == nullptr) {
		return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
	}
	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	if (impl->lifecycle != PulseLifecycleState::Running) {
		return impl->unavailableResultLocked();
	}
	auto timer = impl->findTimerLocked(id);
	if (!timer) {
		return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
	}
	if (enforceType && timer->type != expectedType) {
		if (expectedType == PulseTimerType::Timeout) {
			return PulseResult::failure(PulseStatus::InvalidArgument, "timer is not a timeout");
		}
		if (expectedType == PulseTimerType::Interval) {
			return PulseResult::failure(PulseStatus::InvalidArgument, "timer is not an interval");
		}
		return PulseResult::failure(PulseStatus::InvalidArgument, "timer is not a countdown");
	}
	return impl->enqueueCommandLocked(commandType, id);
}
} // namespace

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

Pulse::~Pulse() {
	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		return;
	}

	bool selfDestruction = false;
	{
		PulseLock lock(impl->mutex);
		if (lock) {
			selfDestruction = impl->isCurrentTaskLocked();
			if (selfDestruction) {
				impl->requestStopLocked();
			}
		}
	}

	if (!selfDestruction) {
		while (true) {
			PulseResult result = endImpl(impl, 0, true);
			if (result) {
				break;
			}
			vTaskDelay(1);
		}
	}

	PulseImpl *owned = _impl.exchange(nullptr, std::memory_order_acq_rel);
	if (owned != nullptr) {
		owned->release();
	}
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

	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		PulseImpl *candidate = new (std::nothrow) PulseImpl();
		if (candidate == nullptr || !candidate->valid()) {
			if (candidate != nullptr) {
				candidate->release();
			}
			return PulseResult::failure(
			    PulseStatus::OutOfMemory,
			    "failed to allocate pulse implementation"
			);
		}
		PulseImpl *expected = nullptr;
		if (_impl.compare_exchange_strong(
		        expected,
		        candidate,
		        std::memory_order_acq_rel,
		        std::memory_order_acquire
		    )) {
			impl = candidate;
		} else {
			candidate->release();
			impl = expected;
		}
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

	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	if (impl->lifecycle == PulseLifecycleState::Running) {
		return PulseResult::failure(PulseStatus::AlreadyInitialized, "pulse already initialized");
	}
	if (impl->lifecycle != PulseLifecycleState::Uninitialized) {
		return PulseResult::failure(PulseStatus::Busy, "pulse lifecycle is not ready for init");
	}

	impl->config = config;
	impl->actualStackType = actualStackType;
	impl->createdWithCaps = false;
	impl->executedCallbackCount = 0;
	impl->droppedCommandCount = 0;
	impl->lateCallbackCount = 0;
	impl->finalStackHighWaterMarkBytes = 0;
	impl->nextTimerId = 1;
	impl->timerCapacity = timerCapacity;
	impl->timerCount = 0;
	impl->activeTimerCount = 0;
	impl->timers.reset(new (std::nothrow) std::shared_ptr<PulseTimerRecord>[timerCapacity]);
	impl->activeTimers.reset(
	    new (std::nothrow) std::shared_ptr<PulseTimerRecord>[timerCapacity]
	);
	if (!impl->timers || !impl->activeTimers) {
		impl->timers.reset();
		impl->activeTimers.reset();
		impl->timerCapacity = 0;
		return PulseResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer storage");
	}

	impl->commandQueue = xQueueCreate(config.commandQueueSize, sizeof(PulseCommand));
	if (impl->commandQueue == nullptr) {
		impl->timers.reset();
		impl->activeTimers.reset();
		impl->timerCapacity = 0;
		return PulseResult::failure(PulseStatus::QueueCreateFailed, "failed to create command queue");
	}

	xEventGroupClearBits(impl->completionEvent, kCompletionBit);
	impl->retain();
	TaskHandle_t handle = nullptr;
	bool createdWithCaps = false;
	const BaseType_t created = pulse_task_support::createTask(
	    &PulseImpl::taskEntry,
	    config.taskName != nullptr ? config.taskName : "pulse-task",
	    config.stackSizeBytes,
	    impl,
	    config.priority,
	    &handle,
	    config.coreId,
	    usePsramStack,
	    createdWithCaps
	);
	if (created != pdPASS || handle == nullptr) {
		impl->release();
		vQueueDelete(impl->commandQueue);
		impl->commandQueue = nullptr;
		impl->timers.reset();
		impl->activeTimers.reset();
		impl->timerCapacity = 0;
		return PulseResult::failure(PulseStatus::TaskCreateFailed, "failed to create pulse task");
	}

	impl->lifecycleGeneration++;
	if (impl->lifecycleGeneration == 0) {
		impl->lifecycleGeneration++;
	}
	impl->taskHandle = handle;
	impl->createdWithCaps = createdWithCaps;
	impl->lifecycle = PulseLifecycleState::Running;
	xTaskNotifyGive(handle);
	return PulseResult::success("pulse initialized");
}

PulseResult Pulse::end(uint32_t timeoutMs) {
	return endImpl(_impl.load(std::memory_order_acquire), timeoutMs, false);
}

PulseTimerResult Pulse::setTimeout(PulseCallback callback, uint32_t delayMs) {
	if (!callback) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "callback is required");
	}
	if (delayMs == 0) {
		return PulseTimerResult::failure(PulseStatus::InvalidArgument, "delay is required");
	}
	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		return PulseTimerResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
	}

	PulseTimerRecord *rawTimer = new (std::nothrow) PulseTimerRecord();
	if (rawTimer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	std::shared_ptr<PulseTimerRecord> timer(rawTimer);
	timer->callback = std::move(callback);

	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	PulseTimerResult preflight =
	    impl->validateTimerCreateLocked(PulseTimerType::Timeout, "timeout limit reached");
	if (!preflight) {
		return preflight;
	}
	if (!impl->allocateTimerIdLocked(timer->id)) {
		return PulseTimerResult::failure(PulseStatus::InternalError, "timer id exhausted");
	}
	timer->type = PulseTimerType::Timeout;
	timer->delayMs = delayMs;
	timer->nextDueMs = pulseNowMs() + delayMs;
	if (!impl->addRegistryLocked(timer)) {
		return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
	}
	PulseResult queued = impl->enqueueCommandLocked(PulseCommandType::Add, timer->id);
	if (!queued) {
		(void)impl->removeRegistryLocked(timer->id);
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
	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		return PulseTimerResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
	}

	PulseTimerRecord *rawTimer = new (std::nothrow) PulseTimerRecord();
	if (rawTimer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	std::shared_ptr<PulseTimerRecord> timer(rawTimer);
	timer->callback = std::move(callback);

	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	PulseTimerResult preflight =
	    impl->validateTimerCreateLocked(PulseTimerType::Interval, "interval limit reached");
	if (!preflight) {
		return preflight;
	}
	if (!impl->allocateTimerIdLocked(timer->id)) {
		return PulseTimerResult::failure(PulseStatus::InternalError, "timer id exhausted");
	}
	timer->type = PulseTimerType::Interval;
	timer->intervalMs = intervalMs;
	timer->nextDueMs = pulseNowMs() + intervalMs;
	if (!impl->addRegistryLocked(timer)) {
		return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
	}
	PulseResult queued = impl->enqueueCommandLocked(PulseCommandType::Add, timer->id);
	if (!queued) {
		(void)impl->removeRegistryLocked(timer->id);
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
	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		return PulseTimerResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
	}

	PulseTimerRecord *rawTimer = new (std::nothrow) PulseTimerRecord();
	if (rawTimer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	std::shared_ptr<PulseTimerRecord> timer(rawTimer);
	timer->countdownCallback = std::move(callback);

	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	PulseTimerResult preflight =
	    impl->validateTimerCreateLocked(PulseTimerType::Countdown, "countdown limit reached");
	if (!preflight) {
		return preflight;
	}
	if (!impl->allocateTimerIdLocked(timer->id)) {
		return PulseTimerResult::failure(PulseStatus::InternalError, "timer id exhausted");
	}
	timer->type = PulseTimerType::Countdown;
	timer->durationMs = config.durationMs;
	timer->tickMs = config.tickMs;
	timer->nextDueMs = pulseNowMs() + config.tickMs;
	if (!impl->addRegistryLocked(timer)) {
		return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
	}
	PulseResult queued = impl->enqueueCommandLocked(PulseCommandType::Add, timer->id);
	if (!queued) {
		(void)impl->removeRegistryLocked(timer->id);
		return PulseTimerResult::failure(queued.status, queued.message, timer->id);
	}
	return PulseTimerResult::success(timer->id, "countdown scheduled");
}

PulseResult Pulse::clear(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Clear,
	    false,
	    PulseTimerType::Timeout
	);
}

PulseResult Pulse::clearTimeout(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Clear,
	    true,
	    PulseTimerType::Timeout
	);
}

PulseResult Pulse::clearInterval(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Clear,
	    true,
	    PulseTimerType::Interval
	);
}

PulseResult Pulse::clearCountdown(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Clear,
	    true,
	    PulseTimerType::Countdown
	);
}

PulseResult Pulse::pause(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Pause,
	    false,
	    PulseTimerType::Timeout
	);
}

PulseResult Pulse::resume(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Resume,
	    false,
	    PulseTimerType::Timeout
	);
}

PulseResult Pulse::restart(PulseTimerId id) {
	return queueControl(
	    _impl.load(std::memory_order_acquire),
	    id,
	    PulseCommandType::Restart,
	    false,
	    PulseTimerType::Timeout
	);
}

PulseTimerState Pulse::getState(PulseTimerId id) {
	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		return PulseTimerState::NotFound;
	}
	PulseLock lock(impl->mutex);
	if (!lock || impl->lifecycle == PulseLifecycleState::Stopped ||
	    impl->lifecycle == PulseLifecycleState::Uninitialized) {
		return PulseTimerState::NotFound;
	}
	auto timer = impl->findTimerLocked(id);
	return timer ? timer->state : PulseTimerState::NotFound;
}

PulseDiag Pulse::getDiagnostics() {
	PulseDiag diag;
	PulseImpl *impl = _impl.load(std::memory_order_acquire);
	if (impl == nullptr) {
		return diag;
	}
	PulseLock lock(impl->mutex);
	if (!lock) {
		return diag;
	}

	diag.commandQueueSize = impl->config.commandQueueSize;
	diag.commandQueueUsed = impl->queueUsageLocked();
	diag.executedCallbackCount = impl->executedCallbackCount;
	diag.droppedCommandCount = impl->droppedCommandCount;
	diag.lateCallbackCount = impl->lateCallbackCount;
	diag.stackHighWaterMarkBytes = impl->finalStackHighWaterMarkBytes;
	diag.requestedStackType = impl->config.stackType;
	diag.actualStackType = impl->actualStackType;

	if (impl->taskHandle != nullptr) {
		diag.stackHighWaterMarkBytes =
		    pulse_task_support::stackHighWaterMarkBytes(impl->taskHandle);
	}
	for (uint32_t index = 0; index < impl->timerCount; index++) {
		const auto &timer = impl->timers[index];
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
