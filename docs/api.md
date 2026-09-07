# API Reference

This page summarizes the public API declared in `src/Pulse.h` for Pulse `v0.2.0`.

## Results

Pulse does not explicitly throw exceptions. Operations return `PulseResult` or `PulseTimerResult`.

Pulse-owned allocation routes through Strata where Pulse controls the storage. User-provided `std::function` callbacks and captures still follow standard-library and toolchain allocation behavior.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `PulseStatus`. |
| `message` | Human-readable status as a stable `const char *`. |
| `id` | Returned by `PulseTimerResult` when a timer was created. |

`PulseStatus` values are `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `QueueCreateFailed`, `TimerNotFound`, `QueueFull`, `Busy`, `Timeout`, and `InternalError`.

## PulseConfig

```cpp
PulseConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.maxTimeouts = 16;
config.maxIntervals = 16;
config.maxCountdowns = 8;
config.commandQueueSize = 20;
config.taskName = "pulse-task";
```

`memory.allocation` controls movable Pulse-owned storage such as timer records, bounded timer slot backing, and command-queue item storage. `memory.taskStack` controls the scheduler task stack. Strata keeps the relevant FreeRTOS control blocks internal.

The default task-stack policy is `Strata::Placement::PreferExternal`, which preserves Pulse `v0.1.0`'s old `PulseStackType::Auto` behavior.

## Pulse

| Method | Purpose |
| --- | --- |
| `init(config)` | Allocate Strata-backed scheduler resources and start a new lifecycle generation. |
| `end(timeoutMs)` | Request shutdown and externally reap the targeted scheduler task after it quiesces. |
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
| `getDiagnostics()` | Return aggregate timer, queue, task-stack, and Strata placement diagnostics. |

## Lifecycle

The public lifecycle remains equivalent to:

```text
Uninitialized -> Running -> Stopping -> Uninitialized
```

Internally, Pulse `v0.2.0` adds `Quiesced` and `Reaping` handoff states. The scheduler task first stops dispatching work and releases scheduler-owned timer/queue storage. Another FreeRTOS task context then resets the `Strata::FreeRTOS::Task`, which deletes the scheduler and releases its static stack and task control block.

Each successful `init()` begins a new lifecycle generation. An `end()` call captures the generation it intends to stop. If another caller completes shutdown and a newer generation starts before an older waiter resumes, that stale waiter returns success without touching the new scheduler.

`init()` is accepted only from the internally uninitialized state. During stopping, quiescing, or reaping, timer creation and controls return `Busy`, and a new `init()` also returns `Busy`.

## Shutdown

Shutdown does not depend on bounded command-queue capacity. It changes the lifecycle to stopping and wakes the Pulse task through its task notification.

If `end(timeoutMs)` returns `Timeout`, shutdown remains requested. A later `end()` continues waiting for the same lifecycle generation.

Calling `end()` from the Pulse task, including from a callback, returns `PulseStatus::Busy` because the scheduler cannot synchronously wait for itself.

Destroying Pulse from another task uses join-style behavior and waits until the scheduler can be reaped. Destroying Pulse from one of its own callbacks is also supported: Pulse marks the implementation orphaned, allows the scheduler to quiesce, and defers the Strata task reset to the FreeRTOS timer-service task.

## Queued controls

`clear()`, `pause()`, `resume()`, and `restart()` use nonblocking sends to a `Strata::FreeRTOS::Queue<PulseCommand>`. Queue item storage follows `PulseConfig::memory.allocation`; the FreeRTOS queue control block is internal.

A successful result means the control was accepted for the current running lifecycle generation. Controls already queued when a callback completes are processed before another due timer is selected.

Shutdown supersedes pending timer operations. Calling `getState()` immediately after a queued control may still show the previous state until the scheduler processes it.

## Callback controls

Intervals and non-final countdowns remain registered while their callbacks execute.

Controls queued during one of those callbacks are applied before default rescheduling. Pulse uses a timer mutation generation so restart, pause, resume, or clear cannot be overwritten by unconditional post-callback scheduling.

An interval that pauses itself preserves a full interval before the next callback after resume. A non-final countdown that pauses itself preserves `min(tickMs, remainingMs)` before the next countdown tick after resume.

A timeout is removed before its callback runs. A countdown is removed before its final callback. Controls targeting those terminal timers return `TimerNotFound`.

A callback may clear another timer that is already due; that clear is processed before Pulse selects the next due timer.

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

`PulseDiag` reports:

- timer counts and running/paused counts;
- command queue size/usage;
- callback, late-callback, and dropped-command counters;
- `stackHighWaterMarkBytes`;
- `requestedStackPlacement` and observed `stackRegion`;
- `commandQueueStoragePlacement` and observed `commandQueueStorageRegion`.

The requested placement is policy; the region is the actual storage location reported by Strata. With `PreferExternal`, the requested placement can remain external-preferred even when the observed region is internal after fallback.

While the scheduler is active, stack high-water data comes from `Strata::FreeRTOS::Task`. After quiescence, Pulse retains the final byte-normalized value.

## v0.1.0 to v0.2.0 migration

| Pulse v0.1.0 | Pulse v0.2.0 |
| --- | --- |
| `PulseStackType::Auto` | `Strata::Placement::PreferExternal` |
| `PulseStackType::Internal` | `Strata::Placement::Internal` |
| `PulseStackType::Psram` | `Strata::Placement::RequireExternal` |
| `config.stackType` | `config.memory.taskStack` |
| no general allocation policy | `config.memory.allocation` |
