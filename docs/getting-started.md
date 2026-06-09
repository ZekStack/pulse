# Getting Started

Pulse schedules short runtime timers from one internal FreeRTOS task.

## Install

Use Pulse as an Arduino ESP32 or PlatformIO library and compile with C++20.

```ini
lib_deps =
  https://github.com/ZekStack/pulse.git

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

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
