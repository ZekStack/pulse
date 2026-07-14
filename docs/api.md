# API Reference

## Result types

Pulse operations return `PulseResult` or `PulseTimerResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success. |
| `status` | Machine-readable `PulseStatus`. |
| `message` | Stable human-readable `const char *`. |
| `id` | Timer identifier returned by `PulseTimerResult`. |

Pulse does not explicitly throw exceptions. Standard-library allocation behavior for user callbacks still follows the selected toolchain.

## Lifecycle

| Method | Behavior |
| --- | --- |
| `init(config)` | Allocates scheduler resources and starts the internal task. |
| `end(timeoutMs)` | Requests shutdown and waits for the task to become quiescent. |

Internal lifecycle states are `Uninitialized`, `Running`, `Stopping`, and `Stopped`.

- Timer creation and controls are accepted only while running.
- During stopping or stopped-before-finalization, those operations return `Busy`.
- `end()` from the Pulse task returns `Busy`.
- A timed-out `end()` leaves Pulse in `Stopping`; a later `end()` may continue waiting.
- Multiple concurrent external `end()` callers are supported.
- The implementation uses a lifecycle generation and waiter count so an old waiter cannot stop a later initialization cycle.

`Stopped` means the Pulse task has completed scheduler cleanup and will never access the implementation again. Physical FreeRTOS task deletion follows immediately using task-local values.

## Timer creation

| Method | Behavior |
| --- | --- |
| `setTimeout(callback, delayMs)` | Executes once after the delay. |
| `setInterval(callback, intervalMs)` | Repeats with delay-after-callback timing. |
| `setCountdown(config, callback)` | Emits countdown ticks through completion. |

Creation is transactional: registration and the `Add` command are committed under lifecycle protection. Queue failure rolls registration back before returning.

## Controls

| Method | Behavior |
| --- | --- |
| `clear(id)` | Queues removal of any timer. |
| `clearTimeout(id)` | Queues removal after validating timeout type. |
| `clearInterval(id)` | Queues removal after validating interval type. |
| `clearCountdown(id)` | Queues removal after validating countdown type. |
| `pause(id)` | Queues pause and preserves the next delay. |
| `resume(id)` | Queues resume using the preserved delay. |
| `restart(id)` | Queues restart from the timer's original duration. |

Queue sends are nonblocking. A full command queue returns `QueueFull` immediately.

While Pulse remains in the same running lifecycle generation, accepted callback-generated controls are processed before another already-due timer. Shutdown supersedes pending controls.

## Terminal callbacks

Timeouts and final countdowns are removed before their callback runs. Controls targeting that timer from its terminal callback return `TimerNotFound`.

Intervals and non-final countdowns remain registered. Callback controls increment an internal mutation generation; default rescheduling occurs only when no accepted control changed the timer.

A callback-side interval pause preserves the full next interval. A callback-side non-final countdown pause preserves the next countdown delay.

## Countdown tick

| Field | Meaning |
| --- | --- |
| `elapsedMs` | Countdown runtime elapsed. |
| `remainingMs` | Countdown runtime remaining. |
| `elapsedSeconds` | `elapsedMs / 1000`. |
| `remainingSeconds` | `remainingMs / 1000`. |
| `isFinished` | `true` for the final tick. |

The first callback runs after `tickMs`. The final tick is exact even when the duration is not divisible by the tick size.

## Read-only operations

`getState(id)` returns `Running`, `Paused`, or `NotFound`. It remains available during stopping while timer storage still exists, then returns `NotFound` after scheduler cleanup.

`getDiagnostics()` remains safe during running, stopping, and stopped states. It reads task and queue data under lifecycle protection and returns the final stored stack high-water value after the task is quiescent.
