# API Reference

This page summarizes the public API declared in `src/Pulse.h`.

## Results

Pulse does not throw exceptions. Operations return `PulseResult` or `PulseTimerResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `PulseStatus`. |
| `message` | Human-readable status as a stable `const char *`. |
| `id` | Returned by `PulseTimerResult` when a timer was created. |

`PulseStatus` values are `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `QueueCreateFailed`, `TimerNotFound`, `QueueFull`, `Busy`, `Timeout`, and `InternalError`.

## Pulse

| Method | Purpose |
| --- | --- |
| `init(config)` | Allocate timer pointer storage, create the command queue, and start the internal task. |
| `end(timeoutMs)` | Stop the task, delete the command queue, and clear timers. |
| `setTimeout(callback, delayMs)` | Run a callback once after a delay. |
| `setInterval(callback, intervalMs)` | Run a callback repeatedly with delay-after-callback timing. |
| `setCountdown(config, callback)` | Run countdown tick callbacks until completion. |
| `clear(id)` | Queue clearing any timer by id. |
| `clearTimeout(id)` | Queue clearing a timeout by id. |
| `clearInterval(id)` | Queue clearing an interval by id. |
| `clearCountdown(id)` | Queue clearing a countdown by id. |
| `pause(id)` | Queue pausing a timer while preserving time until its next callback. |
| `resume(id)` | Queue resuming a paused timer. |
| `restart(id)` | Queue restarting a timer from its original delay, interval, or countdown duration. |
| `getState(id)` | Return `Running`, `Paused`, or `NotFound`. |
| `getDiagnostics()` | Return aggregate counts and task diagnostics. |

`clear()`, `pause()`, `resume()`, and `restart()` are thread-safe queued commands. A successful result means the command was queued, not necessarily already applied. Calling `getState()` immediately after one of these methods may still show the previous state until the Pulse task processes the command.

Callbacks may call `clear()`, `pause()`, `resume()`, and `restart()`. Those operations are queued and take effect after the current callback returns.

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

`PulseDiag` reports timer counts, running and paused counts, command queue size and usage, callback counters, late callback count, stack high-water mark, and requested/actual stack type. While the Pulse task is running, the stack high-water mark is read from the task handle; after shutdown, the last stored task value is returned.
