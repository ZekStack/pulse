#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;

void setup() {
	Serial.begin(115200);

	PulseConfig config;
	config.stackSizeBytes = 4096;
	config.priority = 1;
	config.coreId = tskNO_AFFINITY;
	config.stackType = PulseStackType::Auto;
	config.maxTimeouts = 1;
	config.maxIntervals = 1;
	config.maxCountdowns = 1;
	config.commandQueueSize = 8;

	PulseResult initResult = pulse.init(config);
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	pulse.setTimeout([]() {
		Serial.println("first timeout");
	}, 5000);

	PulseTimerResult second = pulse.setTimeout([]() {
		Serial.println("should not be scheduled");
	}, 5000);

	if (!second) {
		Serial.printf("limit failure: %s\n", second.message.c_str());
	}
}

void loop() {
	delay(1000);
}
