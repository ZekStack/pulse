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
* **Lifecycle-safe shutdown** - shutdown is independent of command-queue capacity and may be retried after a timeout.
* **Production-minded** - result-based errors, synchronized lifecycle operations, diagnostics, and no explicit exceptions.

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
* `clear()`, `pause()`, `resume()`, and `restart()` enqueue nonblocking control commands.
* A control that returns success is accepted for the current running lifecycle generation. Shutdown supersedes pending timer controls.
* Controls queued by a callback are processed before another already-due timer is selected.
* A timeout and a final countdown are terminal before their callback runs. Controls for that timer return `TimerNotFound` from the terminal callback.
* Calling `end()` from a Pulse callback returns `PulseStatus::Busy` because the task cannot wait for itself.
* If `end(timeoutMs)` returns `Timeout`, shutdown remains requested. Call `end()` again to continue waiting.
* Destroying Pulse from another task waits until the scheduler is quiesced. Callbacks must eventually return.
* Zero-millisecond timer values are rejected.
* Stack sizes are FreeRTOS byte sizes on ESP32 and must be at least 1024 bytes.
* Stack high-water diagnostics use the ESP-IDF byte value directly.
* `PulseStackType::Auto` prefers PSRAM task stacks when supported and falls back to internal RAM.

## Timing guarantees

Pulse uses ESP-IDF's 64-bit monotonic runtime timer internally. It is intended for short runtime timers, not wall-clock scheduling.

## Threading and lifecycle model

All callbacks run from the internal Pulse task. Timer creation and control methods synchronize against initialization and shutdown.

Each successful `init()` begins a new internal lifecycle generation. `end()` targets the generation that was running when the call began. A delayed waiter from an older generation cannot stop a newer run.

The internal lifecycle is:

```txt
Uninitialized -> Running -> Stopping -> Stopped -> Uninitialized
```

`Stopping` means shutdown was requested but an active callback or scheduler cleanup may still be in progress. Timer creation and controls return `Busy` in this state. Diagnostics remain available.

`Stopped` means scheduler resources are quiesced and the scheduler task will no longer access them. Physical FreeRTOS task deletion follows using task-local values. A waiting `end()` then completes public lifecycle finalization.

Shutdown has a dedicated task wakeup and does not use the bounded command queue.

## Callback control semantics

Controls from interval and non-final countdown callbacks are applied after the callback returns and before another due timer is dispatched.

An interval that pauses itself resumes after a complete interval. A non-final countdown that pauses itself preserves the delay until its next countdown tick.

Timeouts and final countdowns are removed from the registry before their terminal callback. Their own `clear()`, `pause()`, `resume()`, and `restart()` calls therefore return `TimerNotFound`.

## Memory and exception model

Pulse does not explicitly throw exceptions. Internal Pulse allocations use checked non-throwing allocation where practical.

Timer records, `shared_ptr` control blocks, user callbacks, and `std::function` captures may allocate. Construction and storage of user-provided callbacks follow the standard-library and toolchain allocation behavior. A build with exceptions disabled proves compilation compatibility; it does not guarantee graceful failure for every standard-library allocation.

Fully fixed-capacity callback and timer-record storage may be introduced in a later release.

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
| [`docs/api.md`](docs/api.md) | Public classes, lifecycle, timer controls, and diagnostics. |
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
| Exceptions | No explicit throws; `std::function` follows toolchain behavior |
| Status | `0.1.0` release candidate |

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
