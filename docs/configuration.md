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

`PulseStackType::Auto` uses PSRAM task stacks when the ESP-IDF support is available and PSRAM is present. It falls back to internal RAM otherwise.

## Limits

The timer limits are per type:

| Field | Meaning |
| --- | --- |
| `maxTimeouts` | Maximum active or paused timeouts. |
| `maxIntervals` | Maximum active or paused intervals. |
| `maxCountdowns` | Maximum active or paused countdowns. |
| `commandQueueSize` | Number of pending control commands. |

If a limit is reached, creation returns a failed `PulseTimerResult`.

During `init()`, Pulse validates that the configured timer limits have a non-zero total and do not overflow. It then allocates pre-sized internal pointer arrays for `maxTimeouts + maxIntervals + maxCountdowns` timer slots. Timer records, user callbacks, and `std::function` captures may still allocate.

The command queue stores control commands by value. If the queue is full, queued operations return `PulseStatus::QueueFull`.

## Timing rules

All timing uses ESP-IDF's 64-bit monotonic runtime timer. Pulse is not affected by NTP sync, timezone changes, DST, or system date corrections.

Zero-millisecond delay, interval, duration, and tick values are rejected.
