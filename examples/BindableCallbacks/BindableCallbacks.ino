#include <Arduino.h>
#include <Pulse.h>
#include <functional>

class Blinker {
  public:
	explicit Blinker(Pulse &pulse) : _pulse(pulse) {
	}

	void begin() {
		_pulse.setInterval(std::bind(&Blinker::tick, this), 1000);
	}

  private:
	void tick() {
		_count++;
		Serial.printf("bound callback count=%u\n", static_cast<unsigned>(_count));
	}

	Pulse &_pulse;
	uint32_t _count = 0;
};

Pulse pulse;
Blinker blinker(pulse);

void setup() {
	Serial.begin(115200);

	PulseResult initResult = pulse.init();
	if (!initResult) {
		Serial.println(initResult.message.c_str());
		return;
	}

	blinker.begin();
}

void loop() {
	delay(1000);
}
