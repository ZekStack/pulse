#include <Arduino.h>
#include <Pulse.h>

#ifndef PULSE_SMOKE_USE_PSRAM
#define PULSE_SMOKE_USE_PSRAM 0
#endif

namespace {
Pulse pulse;
SemaphoreHandle_t callbackEntered = nullptr;
SemaphoreHandle_t releaseCallback = nullptr;
volatile bool smokeComplete = false;
volatile bool smokePassed = true;

void check(bool condition, const char *message) {
	Serial.print(condition ? "PASS: " : "FAIL: ");
	Serial.println(message);
	if (!condition) {
		smokePassed = false;
	}
}

PulseConfig smokeConfig() {
	PulseConfig config;
	config.stackSizeBytes = 4096;
	config.stackType = PULSE_SMOKE_USE_PSRAM ? PulseStackType::Psram : PulseStackType::Internal;
	config.commandQueueSize = 16;
	return config;
}

void shutdownVerifier(void *) {
	if (xSemaphoreTake(callbackEntered, pdMS_TO_TICKS(2000)) != pdTRUE) {
		check(false, "long callback entered");
		smokeComplete = true;
		vTaskDelete(nullptr);
		return;
	}

	PulseResult timed = pulse.end(20);
	check(!timed && timed.status == PulseStatus::Timeout, "end times out during long callback");
	check(
	    pulse.setTimeout([]() {}, 1).status == PulseStatus::Busy,
	    "timer creation is busy while stopping"
	);

	xSemaphoreGive(releaseCallback);
	check(static_cast<bool>(pulse.end(2000)), "end retry succeeds");
	check(static_cast<bool>(pulse.init(smokeConfig())), "reinitialize after shutdown");
	check(static_cast<bool>(pulse.end(2000)), "second lifecycle ends cleanly");

	smokeComplete = true;
	vTaskDelete(nullptr);
}
} // namespace

void setup() {
	Serial.begin(115200);
	delay(250);
	Serial.println("Pulse device smoke starting");

	callbackEntered = xSemaphoreCreateBinary();
	releaseCallback = xSemaphoreCreateBinary();
	check(callbackEntered != nullptr && releaseCallback != nullptr, "smoke semaphores allocated");
	if (callbackEntered == nullptr || releaseCallback == nullptr) {
		smokeComplete = true;
		return;
	}

	PulseResult initialized = pulse.init(smokeConfig());
	check(static_cast<bool>(initialized), "Pulse initialized");
	if (!initialized) {
		smokeComplete = true;
		return;
	}

	PulseDiag diagnostics = pulse.getDiagnostics();
	check(diagnostics.stackHighWaterMarkBytes > 0, "stack high-water mark is available");
	check(
	    diagnostics.actualStackType == smokeConfig().stackType,
	    "requested task stack type is active"
	);

	volatile PulseStatus callbackEndStatus = PulseStatus::InternalError;
	PulseTimerResult callbackEnd = pulse.setTimeout(
	    [&]() { callbackEndStatus = pulse.end(10).status; },
	    10
	);
	check(static_cast<bool>(callbackEnd), "callback-side end test scheduled");
	delay(30);
	check(callbackEndStatus == PulseStatus::Busy, "end from callback returns Busy");

	PulseTimerResult longCallback = pulse.setTimeout(
	    []() {
		    xSemaphoreGive(callbackEntered);
		    xSemaphoreTake(releaseCallback, portMAX_DELAY);
	    },
	    20
	);
	check(static_cast<bool>(longCallback), "long callback scheduled");

	BaseType_t created = xTaskCreate(
	    shutdownVerifier,
	    "pulse-smoke-stop",
	    4096,
	    nullptr,
	    1,
	    nullptr
	);
	check(created == pdPASS, "shutdown verifier task created");
}

void loop() {
	if (!smokeComplete) {
		delay(25);
		return;
	}

	Serial.println(smokePassed ? "PULSE_SMOKE_PASS" : "PULSE_SMOKE_FAIL");
	if (callbackEntered != nullptr) {
		vSemaphoreDelete(callbackEntered);
		callbackEntered = nullptr;
	}
	if (releaseCallback != nullptr) {
		vSemaphoreDelete(releaseCallback);
		releaseCallback = nullptr;
	}
	for (;;) {
		delay(1000);
	}
}
