# Troubleshooting

## Timer creation fails

Check `PulseTimerResult::message` and `PulseTimerResult::status`.

Common causes:

* Pulse was not initialized.
* Pulse is stopping or stopped and the operation returned `Busy`.
* A callback was missing.
* A delay, interval, duration, or tick value was zero.
* A per-type timer limit was reached.
* The total configured timer capacity was zero or overflowed.
* The command queue was full.

## Countdown does not start immediately

This is expected. Countdown callbacks first run after `tickMs`.

## Interval callbacks drift

Pulse intervals use delay-after-callback timing. The next interval is scheduled after the callback returns. Pulse does not catch up missed interval ticks.

## Command queue is full

Increase `PulseConfig::commandQueueSize` or reduce bursts of `clear()`, `pause()`, `resume()`, and `restart()` calls.

Timer-control queue sends are nonblocking. Queue-full operations return `QueueFull` immediately.

Shutdown has a separate wake mechanism and does not depend on command-queue capacity.

## State did not change immediately

This is expected after `clear()`, `pause()`, `resume()`, and `restart()`. A successful result means the command was queued for the current running lifecycle generation, not necessarily already applied.

While Pulse remains in that generation, callback-generated controls are processed before another due timer is selected. Shutdown supersedes pending timer controls.

## A terminal callback cannot restart its timer

Timeouts are removed before their callback runs. Countdowns are removed before the final callback. Controls targeting those terminal timers return `TimerNotFound` by design.

Restart or pause a countdown from a non-final tick instead.

## `end()` returns `Busy` from a callback

All callbacks run on the Pulse task. That task cannot synchronously wait for itself, so callback-side `end()` returns `Busy`.

Request shutdown from another task after the callback returns. Pulse does not expose a public asynchronous stop API in v0.1.0.

## `end()` returns `Timeout`

The timeout does not cancel shutdown and does not release scheduler resources prematurely. Pulse remains in the stopping state.

Allow the active callback to return, then call `end()` again. Diagnostics remain available while shutdown is pending.

## Destruction blocks

Destroying Pulse from another task waits until the scheduler is quiesced. A callback that never returns can therefore block destruction indefinitely.

Callbacks must eventually return. Offload long-running or blocking work to Worker.

## Callback blocks other timers

All Pulse callbacks run from one internal task. Keep callbacks short and offload blocking work to Worker.

Intervals and non-final countdowns may call Pulse controls. Those controls are queued and applied after the current callback returns. An interval that pauses itself resumes after a complete interval; a countdown preserves its next tick delay.
