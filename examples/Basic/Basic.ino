#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;
PulseTimerId intervalId = 0;
uint32_t intervalRuns = 0;

void setup() {
	Serial.begin(115200);

	PulseResult initResult = pulse.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	pulse.setTimeout([]() {
		Serial.println("timeout fired after 1 second");
	}, 1000);

	PulseTimerResult intervalResult = pulse.setInterval([]() {
		intervalRuns++;
		Serial.printf("interval run=%u\n", static_cast<unsigned>(intervalRuns));
		if (intervalRuns >= 5 && intervalId != 0) {
			pulse.clearInterval(intervalId);
		}
	}, 1000);

	if (intervalResult) {
		intervalId = intervalResult.id;
	}
}

void loop() {
	delay(1000);
}
