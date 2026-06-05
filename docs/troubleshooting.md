# Troubleshooting

## Timer creation fails

Check `PulseTimerResult::message` and `PulseTimerResult::status`.

Common causes:

* Pulse was not initialized.
* A callback was missing.
* A delay, interval, duration, or tick value was zero.
* A per-type timer limit was reached.
* The command queue was full.

## Countdown does not start immediately

This is expected. Countdown callbacks first run after `tickMs`.

## Interval callbacks drift

Pulse intervals use delay-after-callback timing. The next interval is scheduled after the callback returns. Pulse does not catch up missed interval ticks.

## Command queue is full

Increase `PulseConfig::commandQueueSize` or reduce bursts of `clear()`, `pause()`, `resume()`, and `restart()` calls.

## Callback blocks other timers

All Pulse callbacks run from one internal task. Keep callbacks short and offload blocking work to Worker.
