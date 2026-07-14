# Troubleshooting

## Timer creation fails

Inspect `PulseTimerResult::status` and `message`.

Common causes:

- Pulse is not initialized.
- Pulse is stopping.
- The callback is empty.
- A timing value is zero.
- Countdown `tickMs` exceeds `durationMs`.
- A per-type timer limit is reached.
- The command queue is full.
- Timer or implementation allocation failed.

## State did not change immediately

Controls are queued. Success means the command was accepted, not necessarily already visible through `getState()`.

During callbacks, commands present at callback completion are processed before another already-due timer is dispatched.

## A terminal callback cannot restart itself

This is intentional. A timeout and a final countdown are removed before their callback starts. Self-controls return `TimerNotFound`.

Restart an interval or a non-final countdown instead, or schedule a new timer.

## Pause from an interval callback resumes too soon

Pulse `0.1.0` preserves the complete next interval when pause is requested during the callback. Confirm that the pause command returned success and that the queue was not full.

## `end()` returns `Busy`

`end()` returns `Busy` when called from the Pulse task, including from a callback. Call it from another task.

## `end()` returns `Timeout`

Shutdown was requested but an active callback did not return before the deadline. Pulse remains valid in the stopping state. Timer creation and controls return `Busy`. Call `end()` again after the callback finishes.

## Destruction blocks

External destruction waits until the Pulse task is quiescent. A callback that never returns can therefore block destruction indefinitely.

## Command queue is full

Increase `commandQueueSize` or reduce bursts of controls. Shutdown remains available because it uses a separate task-notification wake path.

## Stack diagnostics look smaller than earlier versions

ESP-IDF already reports stack high-water values in bytes. Pulse `0.1.0` no longer multiplies that value by `sizeof(StackType_t)`, so the corrected number may be one quarter of an older incorrect report on 32-bit targets.
