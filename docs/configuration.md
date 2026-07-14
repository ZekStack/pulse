# Configuration

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
```

## Task settings

ESP32 FreeRTOS task stack sizes are byte values. Pulse requires at least 1024 bytes and alignment to `sizeof(StackType_t)`.

`PulseStackType::Internal` uses internal memory. `Psram` requires ESP-IDF external-stack support and available PSRAM. `Auto` prefers PSRAM and falls back to internal memory.

The command queue stores commands by value. Queue sends are nonblocking; a full queue returns `QueueFull`.

The task is awakened through direct task notifications. Shutdown therefore does not need command-queue space and cannot be ordered behind ordinary controls.

## Limits

| Field | Meaning |
| --- | --- |
| `maxTimeouts` | Active or paused timeouts. |
| `maxIntervals` | Active or paused intervals. |
| `maxCountdowns` | Active or paused countdowns. |
| `commandQueueSize` | Pending timer-control commands. |

The total timer capacity must be non-zero and must not overflow `uint32_t`.

Pulse preallocates bounded pointer registries. Timer records are allocated individually with checked non-throwing allocation and use intrusive reference counting so task callbacks can safely outlive registry removal.

User-provided `std::function` callbacks and their captures retain standard-library allocation behavior.

## Timing

All deadlines use ESP-IDF's 64-bit monotonic runtime timer. Zero delay, interval, duration, and tick values are rejected.

Intervals and countdowns use delay-after-callback scheduling. They do not catch up elapsed ticks after a long callback.
