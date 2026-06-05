# API Reference

This page summarizes the public API declared in `src/Pulse.h`.

## Results

Pulse does not throw exceptions. Operations return `PulseResult` or `PulseTimerResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `PulseStatus`. |
| `message` | Human-readable status. |
| `id` | Returned by `PulseTimerResult` when a timer was created. |

`PulseStatus` values are `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `QueueCreateFailed`, `TimerNotFound`, `QueueFull`, `Busy`, `Timeout`, and `InternalError`.

## Pulse

| Method | Purpose |
| --- | --- |
| `init(config)` | Create the command queue and start the internal task. |
| `end(timeoutMs)` | Stop the task, drain commands, and clear timers. |
| `setTimeout(callback, delayMs)` | Run a callback once after a delay. |
| `setInterval(callback, intervalMs)` | Run a callback repeatedly with delay-after-callback timing. |
| `setCountdown(config, callback)` | Run countdown tick callbacks until completion. |
| `clear(id)` | Clear any timer by id. |
| `clearTimeout(id)` | Clear a timeout by id. |
| `clearInterval(id)` | Clear an interval by id. |
| `clearCountdown(id)` | Clear a countdown by id. |
| `pause(id)` | Pause a timer and preserve time until its next callback. |
| `resume(id)` | Resume a paused timer. |
| `restart(id)` | Restart a timer from its original delay, interval, or countdown duration. |
| `getState(id)` | Return `Running`, `Paused`, or `NotFound`. |
| `getDiagnostics()` | Return aggregate counts and task diagnostics. |

## Countdown ticks

Countdown callbacks receive `PulseCountdownTick`.

| Field | Meaning |
| --- | --- |
| `elapsedMs` | Countdown time elapsed. |
| `remainingMs` | Countdown time remaining. |
| `elapsedSeconds` | `elapsedMs / 1000`. |
| `remainingSeconds` | `remainingMs / 1000`. |
| `isFinished` | `true` on the guaranteed final tick. |

The first callback runs after `tickMs`. The final callback is guaranteed with `remainingMs=0` and `isFinished=true`.

## Diagnostics

`PulseDiag` reports timer counts, running and paused counts, command queue size and usage, callback counters, late callback count, stack high-water mark, and requested/actual stack type.
