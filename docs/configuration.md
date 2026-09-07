# Configuration

`PulseConfig` controls Pulse's Strata memory policy, scheduler task, command queue, and timer limits.

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

PulseResult result = pulse.init(config);
```

## Memory policy

Pulse `v0.2.0` uses the ZekStack-standard `Strata::MemoryPolicy` contract.

| Field | Controls |
| --- | --- |
| `memory.allocation` | Timer records and `shared_ptr` control blocks, timer registry backing, active-timer registry backing, and command-queue item storage. |
| `memory.taskStack` | Scheduler task stack placement. |

The default policy is:

```cpp
allocation = Strata::Placement::Default;
taskStack = Strata::Placement::PreferExternal;
```

`PreferExternal` prefers external memory and falls back to internal memory. `RequireExternal` fails rather than consuming internal memory when external memory is unavailable. FreeRTOS control blocks remain internal as required by Strata.

Pulse keeps its small lifecycle implementation object in internal memory because it coordinates synchronization and task ownership.

## Task settings

Stack sizes are ESP32 FreeRTOS byte sizes. Pulse rejects stack sizes below 1024 bytes or values that are not aligned to `sizeof(StackType_t)`.

`memory.taskStack = Strata::Placement::PreferExternal` preserves the old `PulseStackType::Auto` behavior from Pulse `v0.1.0`.

Stack high-water diagnostics are normalized to bytes by Strata.

## Limits

The timer limits are per type:

| Field | Meaning |
| --- | --- |
| `maxTimeouts` | Maximum active or paused timeouts. |
| `maxIntervals` | Maximum active or paused intervals. |
| `maxCountdowns` | Maximum active or paused countdowns. |
| `commandQueueSize` | Number of pending timer-control commands. |

If a limit is reached, creation returns a failed `PulseTimerResult`.

During `init()`, Pulse validates that the configured timer limits have a non-zero total and do not overflow. It then creates bounded Strata-backed slot storage for `maxTimeouts + maxIntervals + maxCountdowns` timer records.

Timer records and their `shared_ptr` control blocks use `memory.allocation`. User callback captures are still created by the caller/standard library; Pulse does not claim ownership of allocations performed while constructing a `std::function` capture.

## Command queue

The command queue is `Strata::FreeRTOS::Queue<PulseCommand>`. Commands remain trivially-copyable values. Queue item storage follows `memory.allocation`, while Strata keeps the FreeRTOS queue control block internal.

Public timer controls enqueue with zero wait time while lifecycle protection is held. If the queue is full, the operation fails immediately with `PulseStatus::QueueFull`.

Shutdown does not use queue capacity. It wakes the scheduler task directly and remains available even when the command queue is full.

## Lifecycle reuse

The implementation object is installed lazily on the first `init()` and remains stable until the public `Pulse` object is destroyed. Repeated `init()` and `end()` cycles reuse implementation-lifetime synchronization while creating fresh Strata-owned scheduler resources for each lifecycle generation.

The scheduler task quiesces first; `end()` then deletes the Strata task from an external task context. This is required because Strata owns the static task stack and control block. Pulse preserves lifecycle-generation protection so a stale waiter from an older generation cannot reap a newer scheduler task.

If a `Pulse` object is destroyed from one of its own callbacks, teardown is deferred to the FreeRTOS timer-service task so the scheduler is still reaped externally.

## Timing rules

All timing uses ESP-IDF's 64-bit monotonic runtime timer. Pulse is not affected by NTP sync, timezone changes, DST, or system date corrections.

Zero-millisecond delay, interval, duration, and tick values are rejected.
