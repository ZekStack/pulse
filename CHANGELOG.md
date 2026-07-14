# Changelog

## 0.1.0

### Features

- Monotonic ESP-IDF uptime-based timeouts, intervals, and countdowns.
- Pause, resume, restart, typed clear helpers, diagnostics, and optional PSRAM task stacks.
- Bounded per-type timer counts and a bounded command queue.

### Timer semantics

- Intervals use delay-after-callback timing and do not catch up missed ticks.
- Countdown callbacks begin after `tickMs` and end with an exact `remainingMs == 0` final tick.
- Timeouts and completed countdowns are terminal before their callbacks run.
- Callback-generated controls are processed before another already-due timer is dispatched.
- Callback restart and pause operations are authoritative and are not overwritten by default rescheduling.

### Threading and lifecycle

- Public controls are synchronized with the command queue and lifecycle state.
- Shutdown has a dedicated task-notification wake path and does not depend on command-queue capacity.
- `end()` timeouts leave Pulse safely in a retryable stopping state.
- External destruction waits for the Pulse task to become quiescent.
- Destruction from a Pulse callback uses safe asynchronous self-shutdown.

### Diagnostics

- Stack high-water values use ESP-IDF byte units without additional scaling.
- Diagnostics remain safe while shutdown is in progress and retain the final stack high-water value.

### Compatibility

- Arduino ESP32.
- C++20.
- Builds for classic ESP32, ESP32-S3, ESP32-C3, and ESP32-P4 in CI.
- Compiles with exceptions disabled.

### Known limitations

- Callbacks execute serially from one Pulse task and must eventually return.
- Pulse does not explicitly throw exceptions, but allocation behavior of user-provided `std::function` captures follows the standard library and toolchain.
- Pulse schedules runtime durations, not wall-clock dates or times.
