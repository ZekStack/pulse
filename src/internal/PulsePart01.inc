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

namespace pulse_internal {
constexpr PulseTimerId kInvalidTimerId = 0;
constexpr EventBits_t kTaskStoppedBit = BIT0;

enum class PulseLifecycleState : uint8_t {
	Uninitialized,
	Running,
	Stopping,
	Stopped,
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

TickType_t timeoutTicks(uint64_t remainingMs) {
	if (remainingMs > UINT32_MAX) {
		return portMAX_DELAY;
	}
	const TickType_t ticks = pdMS_TO_TICKS(static_cast<uint32_t>(remainingMs));
	return ticks > 0 ? ticks : 1;
}
} // namespace pulse_internal

using namespace pulse_internal;

struct PulseTimerRecord {
	std::atomic<uint32_t> referenceCount{1};
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
	uint32_t callbackDefaultDelayMs = 0;
	uint32_t mutationGeneration = 0;
	uint64_t nextDueMs = 0;
	bool executingCallback = false;

	void retain() noexcept {
		referenceCount.fetch_add(1, std::memory_order_relaxed);
	}

	void release() noexcept {
		if (referenceCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			delete this;
		}
	}
};

struct PulseImpl {
	std::atomic<uint32_t> referenceCount{1};
	PulseConfig config{};
	PulseMutex mutex;
	std::unique_ptr<PulseTimerRecord *[]> timers;
	std::unique_ptr<PulseTimerRecord *[]> activeTimers;
	uint32_t timerCapacity = 0;
	uint32_t timerCount = 0;
	uint32_t activeTimerCount = 0;
	QueueHandle_t commandQueue = nullptr;
	TaskHandle_t taskHandle = nullptr;
	EventGroupHandle_t completionEvent = nullptr;
	PulseLifecycleState lifecycle = PulseLifecycleState::Uninitialized;
	uint64_t lifecycleGeneration = 0;
	uint32_t endWaiterCount = 0;
	bool createdWithCaps = false;
	PulseStackType actualStackType = PulseStackType::Internal;
	PulseTimerId nextTimerId = 1;
	uint32_t executedCallbackCount = 0;
	uint32_t droppedCommandCount = 0;
	uint32_t lateCallbackCount = 0;
	size_t finalStackHighWaterMarkBytes = 0;

	PulseImpl() {
		completionEvent = xEventGroupCreate();
	}

	~PulseImpl() {
		if (commandQueue != nullptr) {
			vQueueDelete(commandQueue);
			commandQueue = nullptr;
		}
		for (uint32_t index = 0; index < timerCount; index++) {
			if (timers && timers[index] != nullptr) {
				timers[index]->release();
				timers[index] = nullptr;
			}
		}
		timers.reset();
		activeTimers.reset();
		if (completionEvent != nullptr) {
			vEventGroupDelete(completionEvent);
			completionEvent = nullptr;
		}
	}

	bool isValid() const {
		return mutex.isValid() && completionEvent != nullptr;
	}

	void retain() noexcept {
		referenceCount.fetch_add(1, std::memory_order_relaxed);
	}

	void release() noexcept {
		if (referenceCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			delete this;
		}
	}

	PulseTimerRecord *findTimerLocked(PulseTimerId id) const {
		for (uint32_t index = 0; index < timerCount; index++) {
			PulseTimerRecord *timer = timers[index];
			if (timer != nullptr && timer->id == id) {
				return timer;
			}
		}
		return nullptr;
	}

	bool hasTimerLocked(PulseTimerId id) const {
		return findTimerLocked(id) != nullptr;
	}

	uint32_t countTypeLocked(PulseTimerType type) const {
		uint32_t count = 0;
		for (uint32_t index = 0; index < timerCount; index++) {
			PulseTimerRecord *timer = timers[index];
			if (timer != nullptr && timer->type == type && isActiveState(timer->state)) {
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
		if (lifecycle == PulseLifecycleState::Uninitialized) {
			return PulseTimerResult::failure(
			    PulseStatus::NotInitialized,
			    "pulse is not initialized"
			);
		}
		if (lifecycle != PulseLifecycleState::Running) {
			return PulseTimerResult::failure(PulseStatus::Busy, "pulse is stopping");
		}
		if (limitReachedLocked(type)) {
			return PulseTimerResult::failure(PulseStatus::Busy, limitMessage);
		}
		if (timerCount >= timerCapacity) {
			return PulseTimerResult::failure(PulseStatus::Busy, "timer storage is full");
		}
		return PulseTimerResult::success(kInvalidTimerId);
	}

	PulseResult validateControlLocked() const {
		if (lifecycle == PulseLifecycleState::Uninitialized) {
			return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
		}
