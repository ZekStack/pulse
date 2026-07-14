#include <Arduino.h>
#include <Pulse.h>

Pulse pulse;
volatile uint32_t timeoutCalls = 0;
volatile uint32_t intervalCalls = 0;
volatile bool countdownFinished = false;
PulseTimerId intervalId = 0;

void fail(const char *message) {
	Serial.printf("PULSE_SMOKE_FAIL: %s\n", message);
	while (true) {
		delay(1000);
	}
}

void setup() {
	Serial.begin(115200);
	delay(500);

	PulseConfig config;
	config.stackType = PulseStackType::Auto;
	config.commandQueueSize = 8;

	PulseResult initialized = pulse.init(config);
	if (!initialized) {
		fail(initialized.message);
	}

	if (!pulse.setTimeout([]() { timeoutCalls++; }, 50)) {
		fail("timeout creation failed");
	}

	PulseTimerResult interval = pulse.setInterval([]() { intervalCalls++; }, 40);
	if (!interval) {
		fail("interval creation failed");
	}
	intervalId = interval.id;

	PulseCountdownConfig countdown{125, 50};
	if (!pulse.setCountdown(countdown, [](const PulseCountdownTick &tick) {
		if (tick.isFinished && tick.remainingMs == 0) {
			countdownFinished = true;
		}
	})) {
		fail("countdown creation failed");
	}
}

void loop() {
	static bool completed = false;
	if (completed) {
		delay(1000);
		return;
	}

	if (millis() < 500) {
		delay(10);
		return;
	}

	if (timeoutCalls != 1) {
		fail("timeout count mismatch");
	}
	if (intervalCalls < 2) {
		fail("interval did not repeat");
	}
	if (!countdownFinished) {
		fail("countdown did not finish");
	}
	if (!pulse.clearInterval(intervalId)) {
		fail("interval clear failed");
	}

	PulseDiag diagnostics = pulse.getDiagnostics();
	Serial.printf(
	    "PULSE_SMOKE_DIAG: stack=%u actualStack=%s callbacks=%u\n",
	    static_cast<unsigned>(diagnostics.stackHighWaterMarkBytes),
	    diagnostics.actualStackType == PulseStackType::Psram ? "psram" : "internal",
	    static_cast<unsigned>(diagnostics.executedCallbackCount)
	);

	PulseResult ended = pulse.end();
	if (!ended) {
		fail(ended.message);
	}

	completed = true;
	Serial.println("PULSE_SMOKE_PASS");
}
