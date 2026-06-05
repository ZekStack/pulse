#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;
PulseTimerId timerId = 0;

void printState(const char *label) {
	Serial.printf("%s: %s\n", label, pulse.timerStateToString(pulse.getState(timerId)));
}

void setup() {
	Serial.begin(115200);

	PulseResult initResult = pulse.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	PulseTimerResult timer = pulse.setTimeout([]() {
		Serial.println("timeout fired");
	}, 5000);

	if (!timer) {
		Serial.println(timer.message.c_str());
		return;
	}
	timerId = timer.id;

	pulse.setTimeout([]() {
		pulse.pause(timerId);
		printState("pause queued");
	}, 1000);

	pulse.setTimeout([]() {
		pulse.resume(timerId);
		printState("resume queued");
	}, 3000);

	pulse.setTimeout([]() {
		pulse.restart(timerId);
		printState("restart queued");
	}, 4000);
}

void loop() {
	delay(1000);
}
