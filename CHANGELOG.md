# Changelog

## 0.1.0

### Features

- Millisecond timeouts, intervals, and countdowns based on ESP-IDF monotonic uptime.
- One bounded scheduler task and command queue for timer coordination.
- Internal or PSRAM-backed task stacks with requested and actual stack diagnostics.
- Pause, resume, restart, clear, timer-state queries, and aggregate diagnostics.

### Threading and lifecycle

- Four-state lifecycle with generation-aware shutdown and reinitialization.
- Dedicated shutdown wakeup independent of command-queue capacity.
- Retryable `end()` timeouts that preserve implementation and scheduler safety.
- Join-style external destruction and safe internal self-destruction ownership.
- Nonblocking, lifecycle-synchronized timer-control queue operations.

### Timer semantics

- Callback-generated controls are processed before another due timer is selected.
- Timeout and final-countdown callbacks are explicitly terminal.
- Interval self-pause preserves a complete interval before resume.
- Countdown self-pause preserves the correct next tick delay.
- Mutation generations prevent callback-side controls from being overwritten by default rescheduling.

### Diagnostics

- ESP-IDF stack high-water values are reported directly in bytes.
- Queue usage, timer counts, callback counts, dropped commands, and late callbacks are exposed through `PulseDiag`.

### Compatibility

- Arduino ESP32 with C++20.
- Example compilation is validated for classic ESP32, ESP32-S3, ESP32-C3, and ESP32-P4 through PIOArduino and Arduino CLI.
- Production sources compile with exceptions disabled.

### Known limitations

- Callbacks run serially on the Pulse task and must eventually return.
- `end()` called from a callback returns `Busy`.
- Shutdown supersedes pending timer controls.
- Timer records, `shared_ptr` control blocks, `std::function`, and user callback captures may allocate.
- Pulse is intended for short uptime timers, not wall-clock scheduling.
