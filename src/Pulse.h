#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using PulseTimerId = uint32_t;

struct PulseCountdownTick;
struct PulseImpl;

using PulseCallback = std::function<void()>;
using PulseCountdownCallback = std::function<void(const PulseCountdownTick &)>;

enum class PulseStatus : uint8_t {
	Ok,
	NotInitialized,
	AlreadyInitialized,
	InvalidArgument,
	OutOfMemory,
	TaskCreateFailed,
	QueueCreateFailed,
	TimerNotFound,
	QueueFull,
	Busy,
	Timeout,
	InternalError,
};

enum class PulseStackType : uint8_t {
	Auto,
	Internal,
	Psram,
};

enum class PulseTimerType : uint8_t {
	Timeout,
	Interval,
	Countdown,
};

enum class PulseTimerState : uint8_t {
	Running,
	Paused,
	NotFound,
};

struct PulseConfig {
	uint32_t stackSizeBytes = 4096;
	UBaseType_t priority = 1;
	BaseType_t coreId = tskNO_AFFINITY;
	PulseStackType stackType = PulseStackType::Auto;
	uint32_t maxTimeouts = 16;
	uint32_t maxIntervals = 16;
	uint32_t maxCountdowns = 8;
	uint32_t commandQueueSize = 20;
	const char *taskName = "pulse-task";
};

struct PulseResult {
	bool result = false;
	PulseStatus status = PulseStatus::InternalError;
	const char *message = "internal error";

	explicit operator bool() const {
		return result;
	}

	static PulseResult success(const char *message = "ok");
	static PulseResult failure(PulseStatus status, const char *message);
};

struct PulseTimerResult : PulseResult {
	PulseTimerId id = 0;

	static PulseTimerResult success(PulseTimerId id, const char *message = "ok");
	static PulseTimerResult failure(
	    PulseStatus status,
	    const char *message,
	    PulseTimerId id = 0
	);
};

struct PulseCountdownConfig {
	uint32_t durationMs = 0;
	uint32_t tickMs = 0;
};

struct PulseCountdownTick {
	uint32_t elapsedMs = 0;
	uint32_t remainingMs = 0;
	uint32_t elapsedSeconds = 0;
	uint32_t remainingSeconds = 0;
	bool isFinished = false;
};

struct PulseDiag {
	uint32_t timeoutCount = 0;
	uint32_t intervalCount = 0;
	uint32_t countdownCount = 0;
	uint32_t runningCount = 0;
	uint32_t pausedCount = 0;
	uint32_t commandQueueSize = 0;
	uint32_t commandQueueUsed = 0;
	uint32_t executedCallbackCount = 0;
	uint32_t droppedCommandCount = 0;
	uint32_t lateCallbackCount = 0;
	size_t stackHighWaterMarkBytes = 0;
	PulseStackType requestedStackType = PulseStackType::Auto;
	PulseStackType actualStackType = PulseStackType::Internal;
};

class Pulse {
  public:
	Pulse() noexcept = default;
	~Pulse();

	Pulse(const Pulse &) = delete;
	Pulse &operator=(const Pulse &) = delete;

	PulseResult init(const PulseConfig &config = PulseConfig());
	PulseResult end(uint32_t timeoutMs = 5000);

	PulseTimerResult setTimeout(PulseCallback callback, uint32_t delayMs);
	PulseTimerResult setInterval(PulseCallback callback, uint32_t intervalMs);
	PulseTimerResult setCountdown(
	    const PulseCountdownConfig &config,
	    PulseCountdownCallback callback
	);

	PulseResult clear(PulseTimerId id);
	PulseResult clearTimeout(PulseTimerId id);
	PulseResult clearInterval(PulseTimerId id);
	PulseResult clearCountdown(PulseTimerId id);

	PulseResult pause(PulseTimerId id);
	PulseResult resume(PulseTimerId id);
	PulseResult restart(PulseTimerId id);

	PulseTimerState getState(PulseTimerId id);
	PulseDiag getDiagnostics();

	const char *statusToString(PulseStatus status) const;
	const char *timerStateToString(PulseTimerState state) const;
	const char *timerTypeToString(PulseTimerType type) const;

  private:
	std::atomic<PulseImpl *> _impl{nullptr};
};
