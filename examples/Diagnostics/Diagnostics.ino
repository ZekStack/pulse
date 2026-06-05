#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;

void setup() {
	Serial.begin(115200);

	PulseResult initResult = pulse.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	pulse.setTimeout([]() {
		Serial.println("timeout");
	}, 3000);

	pulse.setInterval([]() {
		Serial.println("interval");
	}, 1000);
}

void loop() {
	PulseDiag diag = pulse.getDiagnostics();
	Serial.printf(
	    "running=%u paused=%u queue=%u/%u callbacks=%u late=%u dropped=%u\n",
	    static_cast<unsigned>(diag.runningCount),
	    static_cast<unsigned>(diag.pausedCount),
	    static_cast<unsigned>(diag.commandQueueUsed),
	    static_cast<unsigned>(diag.commandQueueSize),
	    static_cast<unsigned>(diag.executedCallbackCount),
	    static_cast<unsigned>(diag.lateCallbackCount),
	    static_cast<unsigned>(diag.droppedCommandCount)
	);
	delay(2000);
}
