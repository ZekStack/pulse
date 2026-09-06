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
	    "running=%u paused=%u queue=%u/%u callbacks=%u late=%u dropped=%u stack=%s/%s queue-memory=%s/%s\n",
	    static_cast<unsigned>(diag.runningCount),
	    static_cast<unsigned>(diag.pausedCount),
	    static_cast<unsigned>(diag.commandQueueUsed),
	    static_cast<unsigned>(diag.commandQueueSize),
	    static_cast<unsigned>(diag.executedCallbackCount),
	    static_cast<unsigned>(diag.lateCallbackCount),
	    static_cast<unsigned>(diag.droppedCommandCount),
	    Strata::toString(diag.requestedStackPlacement),
	    Strata::toString(diag.stackRegion),
	    Strata::toString(diag.commandQueueStoragePlacement),
	    Strata::toString(diag.commandQueueStorageRegion)
	);
	delay(2000);
}
