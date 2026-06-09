#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;

void setup() {
	Serial.begin(115200);

	PulseResult initResult = pulse.init();
	if (!initResult) {
		Serial.println(initResult.message);
		return;
	}

	PulseCountdownConfig countdown;
	countdown.durationMs = 10000;
	countdown.tickMs = 1000;

	pulse.setCountdown(countdown, [](const PulseCountdownTick &tick) {
		Serial.printf(
		    "elapsed=%u remaining=%u finished=%s\n",
		    static_cast<unsigned>(tick.elapsedSeconds),
		    static_cast<unsigned>(tick.remainingSeconds),
		    tick.isFinished ? "yes" : "no"
		);
	});
}

void loop() {
	delay(1000);
}
