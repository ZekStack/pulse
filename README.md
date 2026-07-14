# Pulse

Pulse is a millisecond timer library for Arduino ESP32 projects.

It schedules uptime-based timeouts, intervals, and countdowns without being affected by NTP synchronization, timezone changes, daylight-saving changes, or wall-clock corrections.

[![CI](https://github.com/ZekStack/pulse/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/pulse/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/pulse?sort=semver)](https://github.com/ZekStack/pulse/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Highlights

- **Monotonic timing** using ESP-IDF's 64-bit runtime timer.
- **One internal task** for all timers and callbacks.
- **Bounded counts** for timeouts, intervals, countdowns, and queued controls.
- **Lifecycle-safe shutdown** with timeout/retry behavior and safe destruction.
- **Callback-safe controls** with deterministic ordering between already-due timers.
- **Diagnostics** for timer counts, queue use, callback execution, lateness, and task stack.
- **No explicit exceptions** in Pulse production sources.

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

Pulse is not published to Arduino Library Manager yet. Download the repository ZIP or clone it into:

```text
Arduino/libraries/Pulse
```

Compile the sketch with C++20 support.

## Quick start

```cpp
#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;
PulseTimerId intervalId = 0;

void setup() {
	Serial.begin(115200);

	PulseResult initialized = pulse.init();
	if (!initialized) {
		Serial.println(initialized.message);
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

## Countdown

```cpp
PulseCountdownConfig countdown;
countdown.durationMs = 10000;
countdown.tickMs = 1000;

pulse.setCountdown(countdown, [](const PulseCountdownTick &tick) {
	Serial.printf(
	    "remaining=%u finished=%s\n",
	    static_cast<unsigned>(tick.remainingMs),
	    tick.isFinished ? "true" : "false"
	);
});
```

The first callback runs after `tickMs`. The final callback has `remainingMs == 0` and `isFinished == true`, including when `durationMs` is not divisible by `tickMs`.

## Callback context and ordering

All callbacks run serially from the internal Pulse task. Keep them short and offload blocking work.

`clear()`, `pause()`, `resume()`, and `restart()` are queued controls. While Pulse remains in the same running lifecycle generation, controls accepted during a callback are processed before another already-due timer is dispatched.

Timeouts and final countdown ticks are terminal before their callback begins. Controls targeting that same terminal timer return `PulseStatus::TimerNotFound`.

Intervals and non-final countdowns remain controllable during callbacks. A callback-side pause preserves the next full interval or countdown delay. A callback-side restart is not overwritten by normal rescheduling.

## Shutdown behavior

```cpp
PulseResult result = pulse.end(5000);
```

- `end()` requests shutdown through a dedicated task wake path, independent of command-queue capacity.
- Calling `end()` from a Pulse callback returns `PulseStatus::Busy` because the task cannot wait for itself.
- If `end(timeoutMs)` times out, Pulse remains safely in the stopping state. Call `end()` again to continue waiting.
- Timer creation and controls return `PulseStatus::Busy` while stopping.
- External destruction waits until the Pulse task is quiescent and no longer accesses the implementation.
- Callbacks must eventually return; otherwise external destruction can wait indefinitely.

A successfully queued timer control may be superseded by shutdown because all timers are being destroyed.

## Memory and allocation model

Timer counts and pointer registries are bounded by `PulseConfig`. Timer records use checked non-throwing allocation and intrusive lifetime ownership.

Pulse does not explicitly throw exceptions. Construction and storage of user-provided `std::function` callbacks and their captures follow standard-library and toolchain allocation behavior. A no-exceptions build proves compile compatibility, not graceful handling of every standard-library allocation failure.

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
config.taskName = "pulse-task";

PulseResult result = pulse.init(config);
```

`PulseStackType::Auto` prefers a PSRAM task stack when supported and otherwise uses internal RAM.

## Diagnostics

```cpp
PulseDiag diag = pulse.getDiagnostics();
```

Diagnostics include timer counts, running and paused counts, command queue capacity and use, callback counters, late callbacks, requested and actual stack type, and stack high-water bytes.

ESP-IDF reports stack high-water values in bytes; Pulse returns those values directly. During shutdown, diagnostics remain available. After the task becomes quiescent, the last stored high-water value is returned.

## Compatibility

| Item | Support |
| --- | --- |
| Version | `0.1.0` |
| Framework | Arduino ESP32 |
| Language | C++20 |
| Targets compiled in CI | ESP32, S3, C3, P4 |
| PSRAM | Optional task stack |
| Filesystem | None |
| External dependencies | None |
| Wall-clock scheduling | Not supported |

## Documentation

- [`docs/getting-started.md`](docs/getting-started.md)
- [`docs/configuration.md`](docs/configuration.md)
- [`docs/api.md`](docs/api.md)
- [`docs/examples.md`](docs/examples.md)
- [`docs/troubleshooting.md`](docs/troubleshooting.md)
- [`CHANGELOG.md`](CHANGELOG.md)

## License

MIT — see [`LICENSE.md`](LICENSE.md).

Part of the ZekStack ESP32 library stack.
