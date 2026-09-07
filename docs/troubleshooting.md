# Troubleshooting

## `init()` fails

Check `PulseResult::status` and `PulseResult::message`.

Common causes include an invalid stack size, zero queue/timer capacity, invalid Strata placement, allocation failure, or scheduler task creation failure.

If `memory.taskStack` is `Strata::Placement::RequireExternal`, `TaskCreateFailed` is expected when external memory is unavailable. If `memory.allocation` is `RequireExternal`, Pulse may return `OutOfMemory` while allocating bounded timer storage before the scheduler task is created.

Use `PreferExternal` when fallback to internal memory is acceptable.

## Timer creation fails

Common causes:

- Pulse was not initialized.
- Pulse is stopping, quiesced, or being reaped and the operation returned `Busy`.
- A callback was missing.
- A delay, interval, duration, or tick value was zero.
- A per-type timer limit was reached.
- The command queue was full.
- Strata could not allocate the timer record/control block under the configured `memory.allocation` policy.

## Countdown does not start immediately

This is expected. Countdown callbacks first run after `tickMs`.

## Interval callbacks drift

Pulse intervals use delay-after-callback timing. The next interval is scheduled after the callback returns. Pulse does not catch up missed interval ticks.

## Command queue is full

Increase `PulseConfig::commandQueueSize` or reduce bursts of `clear()`, `pause()`, `resume()`, and `restart()` calls.

Timer-control sends are nonblocking. Queue-full operations return `QueueFull` immediately. Shutdown uses the task notification path and does not depend on queue capacity.

## State did not change immediately

This is expected after `clear()`, `pause()`, `resume()`, and `restart()`. Success means the command was queued for the current lifecycle generation, not necessarily already applied.

## A terminal callback cannot restart its timer

Timeouts are removed before their callback runs. Countdowns are removed before the final callback. Controls targeting those terminal timers return `TimerNotFound` by design.

Restart or pause a countdown from a non-final tick instead.

## `end()` returns `Busy` from a callback

All callbacks run on the Pulse scheduler task. That task cannot synchronously reap its own Strata-owned task storage, so callback-side `end()` returns `Busy`.

Destroying the Pulse object from its callback remains supported in `v0.2.0`: teardown is marked orphaned and final task reaping is deferred to the FreeRTOS timer-service task after the scheduler quiesces.

## `end()` returns `Timeout`

The timeout does not cancel shutdown or release scheduler resources prematurely. Pulse remains in the stopping lifecycle for that generation.

Allow the active callback to return, then call `end()` again. A stale waiter from an older generation cannot reap a newer scheduler task.

## Destruction blocks

Destroying Pulse from another task waits until the scheduler is quiesced and externally reaped. A callback that never returns can therefore block destruction indefinitely.

Callbacks must eventually return. Offload long-running or blocking work to Worker.

## Requested placement and region differ

This is normal with `Strata::Placement::PreferExternal`. `PulseDiag::requestedStackPlacement` and `commandQueueStoragePlacement` report policy, while `stackRegion` and `commandQueueStorageRegion` report the observed memory region. External-preferred allocation may fall back to internal memory.

## Callback blocks other timers

All Pulse callbacks run from one internal task. Keep callbacks short and offload blocking work to Worker.

Intervals and non-final countdowns may call Pulse controls. Those controls are queued and applied after the current callback returns. An interval that pauses itself resumes after a complete interval; a countdown preserves its next tick delay.
