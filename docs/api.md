# API Reference

This page summarizes the public API declared in `src/Pulse.h`.

## Results

Pulse does not explicitly throw exceptions. Operations return `PulseResult` or `PulseTimerResult`.

Internal Pulse allocations use checked non-throwing allocation where practical. User-provided `std::function` callbacks and captures follow the standard-library and toolchain allocation behavior.

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
| `init(config)` | Allocate scheduler resources and start a new lifecycle generation. |
| `end(timeoutMs)` | Request shutdown and wait for the targeted lifecycle generation to quiesce. |
| `setTimeout(callback, delayMs)` | Run a callback once after a delay. |
| `setInterval(callback, intervalMs)` | Run a callback repeatedly with delay-after-callback timing. |
| `setCountdown(config, callback)` | Run countdown tick callbacks until completion. |
| `clear(id)` | Queue clearing any timer by id. |
| `clearTimeout(id)` | Queue clearing a timeout by id. |
| `clearInterval(id)` | Queue clearing an interval by id. |
| `clearCountdown(id)` | Queue clearing a countdown by id. |
| `pause(id)` | Queue pausing a timer while preserving its next delay. |
| `resume(id)` | Queue resuming a paused timer. |
| `restart(id)` | Queue restarting a timer from its original delay, interval, or countdown duration. |
| `getState(id)` | Return `Running`, `Paused`, or `NotFound`. |
| `getDiagnostics()` | Return aggregate counts and task diagnostics. |

## Lifecycle

Pulse internally uses these states:

```txt
Uninitialized -> Running -> Stopping -> Stopped -> Uninitialized
```

Each successful `init()` begins a new lifecycle generation. An `end()` call captures the generation it intends to stop. If another caller completes shutdown and a new generation starts before an older waiter wakes, that older waiter returns success without affecting the new run.

`init()` is allowed only from `Uninitialized`.

During `Stopping` and `Stopped`:

- timer creation and controls return `Busy`;
- `init()` returns `Busy`;
- `getDiagnostics()` remains available;
- `getState()` may report timer state until scheduler storage is detached, then returns `NotFound`.

`Stopped` means the scheduler is quiesced, callback storage has been detached or destroyed, and the scheduler task will no longer access scheduler resources. Physical FreeRTOS task deletion follows immediately using task-local values.

## Shutdown

Shutdown does not use the bounded command queue. It sets the lifecycle to `Stopping` and wakes the Pulse task through its dedicated task notification.

If `end(timeoutMs)` returns `Timeout`, shutdown remains requested. The object remains valid in `Stopping`, diagnostics remain available, and a later `end()` continues waiting.

Calling `end()` from the Pulse task, including from a callback, returns `Busy`. A task cannot synchronously wait for itself.

Destroying Pulse from another task uses join-style behavior: it requests shutdown and waits without a timeout until the scheduler is quiesced. Callbacks must eventually return.

## Queued controls

`clear()`, `pause()`, `resume()`, and `restart()` use nonblocking queue sends while holding lifecycle protection. Queue-full operations fail immediately with `QueueFull`.

A successful result means the control was accepted for the current running lifecycle generation. While Pulse remains in that generation, controls already queued when a callback completes are processed before another due timer is selected.

Shutdown supersedes pending timer operations. A control queued immediately before shutdown may be discarded because scheduler cleanup removes all timers.

Calling `getState()` immediately after a queued control may still show the previous state until the Pulse task processes it.

## Callback controls

Intervals and non-final countdowns remain registered while their callbacks execute.

Controls queued during one of those callbacks are applied before default rescheduling. Pulse uses a timer mutation generation so restart, pause, resume, or clear cannot be overwritten by unconditional post-callback scheduling.

An interval that pauses itself preserves a full interval before the next callback after resume.

A non-final countdown that pauses itself preserves `min(tickMs, remainingMs)` before the next countdown tick after resume.

A timeout is removed before its callback runs. A countdown is removed before its final callback. Controls targeting those terminal timers return `TimerNotFound`.

A callback may clear another timer that is already due. The queued clear is processed before Pulse selects the next due timer.

## Countdown ticks

Countdown callbacks receive `PulseCountdownTick`.

| Field | Meaning |
| --- | --- |
| `elapsedMs` | Countdown time elapsed. |
| `remainingMs` | Countdown time remaining. |
| `elapsedSeconds` | `elapsedMs / 1000`. |
| `remainingSeconds` | `remainingMs / 1000`. |
| `isFinished` | `true` on the guaranteed final tick. |

The first callback runs after `tickMs`. The final callback is guaranteed with `remainingMs=0` and `isFinished=true`, including when the duration is not divisible by the tick size.

## Diagnostics

`PulseDiag` reports timer counts, running and paused counts, command queue size and usage, callback counters, late callback count, stack high-water mark, and requested/actual stack type.

While the scheduler task is active, the stack high-water mark is queried while the lifecycle mutex protects the task handle. After scheduler cleanup, the last stored value is returned.

ESP-IDF reports `uxTaskGetStackHighWaterMark()` in bytes. Pulse returns that value directly without multiplying it by `sizeof(StackType_t)`.
