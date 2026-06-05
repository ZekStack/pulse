## Pulse

A millisecond-based timer library for ESP32.

Pulse handles timeouts, intervals, countdowns, and relative timing tasks. It is intended for short-duration runtime timing.

---

### Rules

*   No exceptions, no throw
*   Embedded friendly
*   Thread safe
*   Pulse timers are based on monotonic uptime milliseconds.  
    They are not affected by NTP sync, timezone changes, DST changes, or system date corrections.
*   one internal Pulse task
*   one control queue
*   one sorted timer list / priority queue

---

### Example usage

```src
#include <Pulse.h>

Pulse pulse;

void setup() {
    PulseConfig config;
    config.stackSizeBytes = 4096;
    config.coreId = tskNO_AFFINITY;
    config.priority = 1;
    config.stackType = PulseStackType::Auto;

    config.maxTimeouts = 16;
    config.maxIntervals = 16;
    config.maxCountdowns = 8;
    config.commandQueueSize = 20;

    PulseResult initResult = pulse.init(config);
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    pulse.setTimeout([]() {
        Serial.println("After 1 sec!");
    }, 1000);

    pulse.setInterval([]() {
        Serial.println("Every 1 sec!");
    }, 1000);

    PulseCountdownConfig countdown;
	countdown.durationMs = 10000;
	countdown.tickMs = 1000;

	pulse.setCountdown(countdown, [](const PulseCountdownTick& tick) {
    	Serial.printf("%d seconds left\n", tick.remainingSeconds);
	});

    PulseTimerResult timeoutResult = pulse.setTimeout([]() {
        Serial.println("Should never run!");
    }, 1000);

    if (timeoutResult) {
        pulse.clearTimeout(timeoutResult.id);
    }

    PulseTimerResult intervalResult = pulse.setInterval([]() {
        Serial.println("Should never run!");
    }, 1000);

    if (intervalResult) {
        pulse.clearInterval(intervalResult.id);
    }

    PulseTimerResult countdownResult = pulse.setCountdown(countdown, [](const PulseCountdownTick& tick) {
    	Serial.printf("Should never run!");
	});

    if (countdownResult) {
        pulse.clearCountdown(countdownResult.id);
    }
}
```

---

### Pulse task:  
\- receives add/clear/pause/resume commands  
\- tracks all timers  
\- executes due callbacks  
\- reschedules intervals/countdowns

### Other methods

```src
PulseResult clear(PulseTimerId id);
PulseResult pause(PulseTimerId id);
PulseResult resume(PulseTimerId id);
PulseResult restart(PulseTimerId id);
PulseTimerState getState(PulseTimerId id); // PulseTimerState::Running, PulseTimerState::Paused, PulseTimerState::NotFound

struct PulseCountdownTick {
    uint32_t elapsedMs;
    uint32_t remainingMs;
    uint32_t elapsedSeconds;
    uint32_t remainingSeconds;
    bool isFinished;
};

PulseDiag diag = pulse.getDiagnostics();

diag.timeoutCount;
diag.intervalCount;
diag.countdownCount;
diag.runningCount;
diag.pausedCount;
diag.commandQueueSize;
diag.commandQueueUsed;
diag.executedCallbackCount;
diag.droppedCommandCount;
diag.lateCallbackCount;
```

Pulse callbacks are executed from the internal Pulse task.  
Callbacks should be short.  
Long-running work should be offloaded to Worker.

setInterval() uses delay-after-callback timing by default.  
Pulse does not try to catch up missed interval ticks.

If the timer limit is reached, setTimeout/setInterval/setCountdown returns a failed PulseTimerResult.  
If the command queue is full, the operation returns a failed PulseResult.