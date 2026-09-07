# Getting Started

Pulse schedules short runtime timers from one internal FreeRTOS task. Pulse `v0.2.0` uses Strata for memory placement and owned FreeRTOS storage.

## Install

Pulse `v0.2.0` requires Strata `v0.1.2` and C++20.

### PlatformIO

```ini
lib_deps =
  https://github.com/ZekStack/pulse.git#v0.2.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

Pulse's `library.json` pins Strata `v0.1.2`, so PlatformIO resolves it transitively.

### Arduino IDE

Install both repositories into the Arduino libraries directory:

```text
Arduino/libraries/Strata
Arduino/libraries/Pulse
```

Use Strata `v0.1.2` or a compatible later release.

## First timer

```cpp
#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;

void setup() {
	Serial.begin(115200);

	PulseResult result = pulse.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	pulse.setTimeout([]() {
		Serial.println("timeout fired");
	}, 1000);
}

void loop() {
	delay(1000);
}
```

## Memory placement

The default scheduler stack prefers external memory and falls back to internal memory. General Pulse-owned allocations use Strata's default placement.

```cpp
PulseConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;

PulseResult result = pulse.init(config);
```

Use `Strata::Placement::Internal` for internal-only placement or `Strata::Placement::RequireExternal` when initialization must fail rather than fall back.

## Timer controls

`setTimeout()` fires once, `setInterval()` repeats, and `setCountdown()` reports elapsed and remaining time. Store the returned `PulseTimerId` when you need to clear, pause, resume, or restart a timer.

```cpp
PulseTimerResult result = pulse.setInterval([]() {
	Serial.println("tick");
}, 1000);

if (result) {
	pulse.pause(result.id);
	pulse.resume(result.id);
	pulse.clear(result.id);
}
```

Control methods are queued. A successful result means the command was accepted by the Pulse command queue, not necessarily already applied.

## Callback context

Callbacks run from the internal Pulse task. Keep them short. Use Worker for long-running or blocking work. Callbacks may call `clear()`, `pause()`, `resume()`, and `restart()`; those operations are queued until the current callback returns.

Calling `end()` from a callback returns `PulseStatus::Busy` because the scheduler cannot wait for itself. Destroying a Pulse object from its own callback remains supported; final task deletion is deferred to an external FreeRTOS task context.
