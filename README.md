# Pulse

Pulse is a millisecond timer library for ESP32. It schedules short runtime timeouts, intervals, and countdowns from one internal FreeRTOS task using monotonic uptime, so NTP sync, timezone changes, DST, and wall-clock corrections do not affect timer deadlines.

Pulse `v0.2.0` uses [Strata](https://github.com/ZekStack/strata) for memory placement and owned FreeRTOS storage.

[![CI](https://github.com/ZekStack/pulse/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/pulse/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/pulse?sort=semver)](https://github.com/ZekStack/pulse/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Highlights

- **Monotonic uptime timers** — timeouts, intervals, and countdowns use ESP-IDF runtime time.
- **One scheduler task** — callbacks execute serially from one internal Pulse task.
- **Bounded scheduling storage** — timer counts and command-queue capacity are configured up front.
- **Shared memory policy** — `Strata::MemoryPolicy` controls general Pulse-owned allocation and task-stack placement.
- **Strata-owned FreeRTOS storage** — scheduler stack/TCB, recursive mutex storage, and command queue storage use static Strata ownership.
- **Lifecycle-safe shutdown** — scheduler task deletion happens from another FreeRTOS task context after quiescence.
- **Generation-safe reuse** — stale `end()` waiters cannot affect a newer Pulse lifecycle.
- **Runtime diagnostics** — timer/queue counters plus requested placement and observed memory region.

## Dependency

Pulse `v0.2.0` requires Strata `v0.1.2`.

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/pulse.git#v0.2.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

Pulse's `library.json` pins Strata, so PlatformIO resolves it transitively.

### Arduino IDE

Pulse and Strata are not published to Arduino Library Manager yet. Install both repositories into the Arduino libraries directory:

```text
Arduino/libraries/Strata
Arduino/libraries/Pulse
```

## Quick start

```cpp
#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;
PulseTimerId intervalId = 0;

void setup() {
	Serial.begin(115200);

	PulseResult initResult = pulse.init();
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	pulse.setTimeout([]() {
		Serial.println("after one second");
	}, 1000);

	PulseTimerResult interval = pulse.setInterval([]() {
		Serial.println("every second");
	}, 1000);

	if (interval) {
		intervalId = interval.id;
	}
}

void loop() {
	delay(1000);
}
```

## Memory policy

Pulse uses the same memory-policy shape as other Strata-backed ZekStack libraries:

```cpp
PulseConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;

PulseResult result = pulse.init(config);
```

`memory.allocation` controls movable Pulse-owned storage, including timer records, `shared_ptr` control blocks, bounded timer-registry backing, and command-queue item storage. `memory.taskStack` controls the scheduler task stack.

Defaults:

```text
allocation = Strata::Placement::Default
taskStack  = Strata::Placement::PreferExternal
```

`PreferExternal` falls back to internal memory. `RequireExternal` fails instead of consuming internal memory when external memory is unavailable. Strata keeps FreeRTOS control blocks internal.

## Diagnostics

Requested memory policy and observed memory location are intentionally separate:

```cpp
PulseDiag diag = pulse.getDiagnostics();

Serial.printf(
    "stack=%s/%s queue=%s/%s\n",
    Strata::toString(diag.requestedStackPlacement),
    Strata::toString(diag.stackRegion),
    Strata::toString(diag.commandQueueStoragePlacement),
    Strata::toString(diag.commandQueueStorageRegion)
);
```

`requestedStackPlacement` is what the application asked for. `stackRegion` is where Strata actually placed the scheduler stack. With `PreferExternal`, those can differ when Pulse falls back to internal memory.

## Important behavior

> [!IMPORTANT]
> Pulse callbacks run from the internal Pulse task. Keep callbacks short and offload long-running work to Worker.

- `setInterval()` uses delay-after-callback timing and does not catch up missed ticks.
- Countdown callbacks first run after `tickMs`; the final callback is guaranteed with `isFinished=true`.
- `clear()`, `pause()`, `resume()`, and `restart()` enqueue nonblocking controls.
- Callback-generated controls are processed before another already-due timer is selected.
- A timeout and final countdown are removed before their terminal callback; controls for that timer return `TimerNotFound`.
- Calling `end()` from a Pulse callback returns `PulseStatus::Busy` because the scheduler cannot wait for itself.
- If `end(timeoutMs)` returns `Timeout`, shutdown remains requested; call `end()` again to continue waiting.
- Destroying Pulse from another task waits until the scheduler quiesces and is externally reaped.
- Destroying Pulse from its own callback remains supported; final Strata task deletion is deferred to the FreeRTOS timer-service task.
- Zero-millisecond timer values are rejected.
- Stack sizes are FreeRTOS byte sizes on ESP32 and must be at least 1024 bytes and aligned to `sizeof(StackType_t)`.

## Lifecycle and task ownership

Each successful `init()` starts a new lifecycle generation. The scheduler runs until shutdown is requested, releases timer and queue resources, then reaches a quiescent handoff. `end()` claims that generation's reap, calls `Strata::FreeRTOS::Task::reset()` from an external task context, and only then makes Pulse reusable.

This external deletion is required because Strata owns the scheduler's static task stack and task control block. Concurrent and stale `end()` waiters are generation-checked before they can reap anything.

## Memory and exception boundary

Pulse routes storage it directly owns through Strata. That includes the implementation object, timer registry backing, timer records, shared ownership control blocks created by Pulse, scheduler task storage, recursive mutex control storage, and command queue storage.

`std::function` remains the public callback surface. Allocation performed by the standard library or caller while constructing/copying user callback captures is outside Pulse's direct ownership boundary. The CI no-exceptions build proves compilation compatibility; it does not claim every standard-library allocation failure is recoverable.

## Configuration

```cpp
PulseConfig config;
config.memory.allocation = Strata::Placement::Default;
config.memory.taskStack = Strata::Placement::PreferExternal;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.maxTimeouts = 16;
config.maxIntervals = 16;
config.maxCountdowns = 8;
config.commandQueueSize = 20;
config.taskName = "pulse-task";
```

See [`docs/configuration.md`](docs/configuration.md) for details.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal init, timeout, interval, and clear. |
| `Countdown` | Countdown ticks and final completion callback. |
| `PauseResumeRestart` | Timer pause, resume, restart, and state checks. |
| `ConfigAndLimits` | Strata memory policy, task settings, queue, and timer limits. |
| `Diagnostics` | Runtime counters plus Strata placement and region diagnostics. |
| `BindableCallbacks` | `std::bind` with private class methods. |

## Documentation

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Dependency setup, first timers, and memory policy. |
| [`docs/configuration.md`](docs/configuration.md) | Memory placement, task settings, limits, and queue sizing. |
| [`docs/api.md`](docs/api.md) | Public API, lifecycle, diagnostics, and v0.1.0 migration mapping. |
| [`docs/examples.md`](docs/examples.md) | Included examples. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Lifecycle, placement, and common runtime issues. |

## Migrating from v0.1.0

Pulse `v0.2.0` intentionally adopts the shared Strata vocabulary instead of retaining Pulse-specific aliases:

| v0.1.0 | v0.2.0 |
| --- | --- |
| `PulseStackType::Auto` | `Strata::Placement::PreferExternal` |
| `PulseStackType::Internal` | `Strata::Placement::Internal` |
| `PulseStackType::Psram` | `Strata::Placement::RequireExternal` |
| `config.stackType` | `config.memory.taskStack` |
| no general allocation policy | `config.memory.allocation` |

The default v0.2.0 task-stack policy preserves the old `Auto` behavior.

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Strata | `v0.1.2` |
| PSRAM | Optional through Strata placement policy |
| Exceptions | No explicit throws; callback/STL allocation follows toolchain behavior |
| CI targets | ESP32, ESP32-S3, ESP32-C3, ESP32-P4 |

## License

MIT — see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack. Pulse follows the shared Strata memory-policy contract used by higher-level ZekStack libraries.
