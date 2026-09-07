# Changelog

## 0.2.0

### Strata integration

- Require Strata `v0.1.2` and adopt the shared `Strata::MemoryPolicy` configuration contract.
- Replace `PulseStackType` with `Strata::Placement`; the default `PreferExternal` scheduler stack preserves the previous `Auto` behavior.
- Route Pulse implementation storage, bounded timer registries, timer records, `shared_ptr` control blocks, and command-queue item storage through Strata-owned allocation paths.
- Replace Pulse-specific recursive mutex, queue, PSRAM task allocation, and capability-aware task teardown with Strata FreeRTOS owners.
- Remove direct ESP-IDF heap-capability ownership logic from Pulse production sources.

### Lifecycle

- Rework scheduler shutdown around Strata's externally-owned static task lifetime: the scheduler quiesces first and is then reaped from another FreeRTOS task context.
- Preserve generation-aware concurrent `end()` behavior so stale waiters cannot affect a newer lifecycle.
- Preserve Pulse destruction from inside a callback by deferring orphaned task reaping to the FreeRTOS timer-service task.
- Remove the dynamically allocated completion EventGroup; lifecycle state is now the shutdown completion source of truth.

### Diagnostics and validation

- Report requested scheduler stack placement separately from the observed Strata memory region.
- Report command-queue item-storage placement and observed region.
- Extend host fakes for Strata static tasks, queues, and recursive mutexes plus external task deletion.
- Add Strata memory-policy host coverage while retaining the existing scheduler, callback, queue-full, stale-waiter, repeated-lifecycle, and callback-destruction tests.
- Add CI source audits preventing direct allocation, heap-capability, and dynamic FreeRTOS ownership paths from returning.
- Validate Strata-backed builds for ESP32, ESP32-S3, ESP32-C3, and ESP32-P4 with PIOArduino and Arduino CLI.

### Migration from 0.1.0

- `PulseStackType::Auto` -> `Strata::Placement::PreferExternal`
- `PulseStackType::Internal` -> `Strata::Placement::Internal`
- `PulseStackType::Psram` -> `Strata::Placement::RequireExternal`
- `PulseConfig::stackType` -> `PulseConfig::memory.taskStack`
- `PulseConfig::memory.allocation` now controls movable Pulse-owned storage.

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
