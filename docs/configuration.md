# Configuration

`PulseConfig` controls the internal task, command queue, and timer limits.

```cpp
PulseConfig config;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.stackType = PulseStackType::Auto;
config.maxTimeouts = 16;
config.maxIntervals = 16;
config.maxCountdowns = 8;
config.commandQueueSize = 20;
config.taskName = "pulse-task";

PulseResult result = pulse.init(config);
```

## Task settings

Stack sizes are ESP32 FreeRTOS byte sizes. Pulse rejects stack sizes below 1024 bytes or values that are not aligned to `sizeof(StackType_t)`.

`PulseStackType::Auto` uses PSRAM task stacks when ESP-IDF support is available and PSRAM is present. It falls back to internal RAM otherwise.

Stack high-water diagnostics use the byte value returned by ESP-IDF directly.

## Limits

The timer limits are per type:

| Field | Meaning |
| --- | --- |
| `maxTimeouts` | Maximum active or paused timeouts. |
| `maxIntervals` | Maximum active or paused intervals. |
| `maxCountdowns` | Maximum active or paused countdowns. |
| `commandQueueSize` | Number of pending timer-control commands. |

If a limit is reached, creation returns a failed `PulseTimerResult`.

During `init()`, Pulse validates that the configured timer limits have a non-zero total and do not overflow. It then allocates pre-sized internal pointer arrays for `maxTimeouts + maxIntervals + maxCountdowns` timer slots.

Timer records, `shared_ptr` control blocks, user callbacks, and `std::function` captures may still allocate. Pulse uses checked non-throwing allocation where practical, but standard-library callback storage follows the toolchain's allocation behavior.

Fully fixed-capacity timer record and callback storage may be introduced later.

## Command queue

The command queue stores timer-control commands by value.

Public timer controls enqueue with a zero wait time while lifecycle protection is held. If the queue is full, the operation fails immediately with `PulseStatus::QueueFull`.

Shutdown does not use the command queue. It has a dedicated task wakeup and therefore remains available even when the command queue is full.

## Lifecycle reuse

The implementation object is installed lazily on the first `init()` and remains stable until the public `Pulse` object is destroyed. Repeated `init()` and `end()` cycles reuse implementation-lifetime synchronization resources while creating fresh scheduler resources for each lifecycle generation.

`init()` is accepted only from the internally uninitialized state. It returns `Busy` while an earlier generation is stopping or waiting for public finalization.

## Timing rules

All timing uses ESP-IDF's 64-bit monotonic runtime timer. Pulse is not affected by NTP sync, timezone changes, DST, or system date corrections.

Zero-millisecond delay, interval, duration, and tick values are rejected.
