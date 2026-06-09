# Pulse

Pulse is a millisecond timer library for ESP32.

Pulse helps you schedule short runtime timeouts, intervals, and countdowns in Arduino ESP32 projects. It is designed for uptime-based timing that should not be affected by NTP sync, timezone changes, DST changes, or system date corrections.

[![CI](https://github.com/ZekStack/pulse/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/pulse/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/pulse?sort=semver)](https://github.com/ZekStack/pulse/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Pulse?

* **Uptime timers** - all timing is based on ESP-IDF's monotonic runtime timer.
* **One task** - timeouts, intervals, and countdowns are coordinated by one internal Pulse task.
* **Bounded counts** - configured limits cap each timer type and the command queue.
* **Task-side callbacks** - callbacks run from the internal Pulse task.
* **Production-minded** - result-based errors, diagnostics, thread-safe internals, and no exceptions.

## Install

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/pulse.git

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

### Arduino IDE

Pulse is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into your Arduino libraries folder.

```txt
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

## Important notes

> [!IMPORTANT]
> Pulse callbacks run from the internal Pulse task. Keep callbacks short and offload long-running work to Worker.

* `setInterval()` uses delay-after-callback timing and does not catch up missed ticks.
* Countdown callbacks first run after `tickMs`; the final callback is guaranteed with `isFinished=true`.
* `clear()`, `pause()`, `resume()`, and `restart()` are queued control commands.
* Zero-millisecond timer values are rejected.
* Stack sizes are FreeRTOS byte sizes on ESP32 and must be at least 1024 bytes.
* `PulseStackType::Auto` prefers PSRAM task stacks when supported and falls back to internal RAM.

## Timing guarantees

Pulse uses ESP-IDF's 64-bit monotonic runtime timer internally. It is intended for short runtime timers, not wall-clock scheduling.

## Threading model

All callbacks run from the internal Pulse task. Control methods are thread-safe and queued, so a successful `clear()`, `pause()`, `resume()`, or `restart()` means the command was accepted, not necessarily already applied. Pulse callbacks may call these control methods; the queued operation takes effect after the current callback returns.

## Memory model

Pulse has bounded timer counts and preallocated internal timer pointer storage, but timer records, user callbacks, and `std::function` captures may allocate.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal init, timeout, interval, and clear. |
| `Countdown` | Countdown ticks and final completion callback. |
| `PauseResumeRestart` | Timer pause, resume, restart, and state checks. |
| `ConfigAndLimits` | Stack, queue, and timer limit configuration. |
| `Diagnostics` | Runtime counters and queue diagnostics. |
| `BindableCallbacks` | `std::bind` with private class methods. |

Start with:

```txt
examples/Basic
```

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Step-by-step setup and first timer flow. |
| [`docs/configuration.md`](docs/configuration.md) | Config options, limits, stack behavior, and queue sizing. |
| [`docs/api.md`](docs/api.md) | Public classes, result types, timer controls, and diagnostics. |
| [`docs/examples.md`](docs/examples.md) | Explanation of all included examples. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common issues and solutions. |

## API overview

```cpp
Pulse pulse;
pulse.init();

PulseTimerResult timeout = pulse.setTimeout([]() {}, 1000);
PulseTimerResult interval = pulse.setInterval([]() {}, 1000);

PulseCountdownConfig countdown;
countdown.durationMs = 10000;
countdown.tickMs = 1000;
pulse.setCountdown(countdown, [](const PulseCountdownTick &tick) {});

pulse.pause(interval.id);
pulse.resume(interval.id);
pulse.clear(interval.id);

PulseDiag diag = pulse.getDiagnostics();
```

For the full API, see [`docs/api.md`](docs/api.md).

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Filesystem | none |
| PSRAM | Optional for task stacks when ESP-IDF support is available |
| Dependencies | none |
| Exceptions | Not used |
| Status | Early-stage `0.0.1` |

## Configuration

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

PulseResult result = pulse.init(config);
```

For all options, see [`docs/configuration.md`](docs/configuration.md).

## Error handling

Pulse reports operation status through `PulseResult` and `PulseTimerResult`.

```cpp
PulseTimerResult result = pulse.setTimeout([]() {}, 1000);

if (!result) {
	Serial.println(result.message);
	return;
}
```

For result fields and status codes, see [`docs/api.md`](docs/api.md).

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
