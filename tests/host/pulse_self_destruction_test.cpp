#include "Pulse.h"
#include "fake_freertos_test.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {
bool waitUntil(const std::function<bool()> &predicate, uint32_t timeoutMs = 1500) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

#define REQUIRE(condition)                                                                    \
	do {                                                                                      \
		if (!(condition)) {                                                                  \
			std::cerr << "FAILED: " #condition " at " << __FILE__ << ':' << __LINE__       \
			          << std::endl;                                                           \
			return 1;                                                                          \
		}                                                                                     \
	} while (false)
} // namespace

int main() {
	auto *pulse = new Pulse();
	REQUIRE(pulse->init());
	REQUIRE(waitUntil([&]() { return pulse->getDiagnostics().commandQueueUsed == 0; }));
	REQUIRE(fakeActiveTaskCount() == 1);

	std::atomic<bool> destructorReturned{false};
	Pulse *ownedPulse = pulse;
	REQUIRE(pulse->setTimeout(
	    [ownedPulse, &destructorReturned]() {
		    delete ownedPulse;
		    destructorReturned = true;
	    },
	    1000
	));
	REQUIRE(waitUntil([&]() { return pulse->getDiagnostics().commandQueueUsed == 0; }));

	fakeAdvanceTimeMs(1000);
	fakeWakeAllTasks();
	REQUIRE(waitUntil([&]() { return destructorReturned.load(); }));
	REQUIRE(waitUntil([]() { return fakeActiveTaskCount() == 0; }));

	std::cout << "Pulse callback self-destruction passed" << std::endl;
	return 0;
}
