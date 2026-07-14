#include "Pulse.h"
#include "fake_freertos_test.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace {
bool waitUntil(const std::function<bool()> &predicate, uint32_t timeoutMs = 1000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) {
			return true;
		}
		std::this_thread::sleep_for(1ms);
	}
	return predicate();
}

bool waitForQueueEmpty(Pulse &pulse) {
	return waitUntil([&pulse]() { return pulse.getDiagnostics().commandQueueUsed == 0; });
}

#define REQUIRE(condition)                                                                    \
	do {                                                                                      \
		if (!(condition)) {                                                                  \
			std::cerr << "FAILED: " #condition " at " << __FILE__ << ':' << __LINE__       \
			          << std::endl;                                                           \
			return false;                                                                      \
		}                                                                                     \
	} while (false)

bool testStackHighWaterUsesBytes() {
	fakeSetStackHighWaterMark(777);
	Pulse pulse;
	REQUIRE(pulse.init());
	REQUIRE(pulse.getDiagnostics().stackHighWaterMarkBytes == 777);
	REQUIRE(pulse.end());
	return true;
}

bool testDueTimerCanBeClearedFromCallback() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<uint32_t> firstCount{0};
	std::atomic<uint32_t> secondCount{0};
	std::atomic<bool> clearAccepted{false};
	PulseTimerId secondId = 0;

	REQUIRE(pulse.setTimeout(
	    [&]() {
		    firstCount++;
		    clearAccepted.store(static_cast<bool>(pulse.clear(secondId)));
	    },
	    100
	));
	PulseTimerResult second = pulse.setTimeout([&]() { secondCount++; }, 100);
	REQUIRE(second);
	secondId = second.id;
	REQUIRE(waitForQueueEmpty(pulse));

	fakeAdvanceTimeMs(100);
	fakeWakeAllTasks();
	REQUIRE(waitUntil([&]() { return firstCount.load() == 1; }));
	REQUIRE(clearAccepted.load());
	std::this_thread::sleep_for(20ms);
	REQUIRE(secondCount.load() == 0);
	REQUIRE(pulse.end());
	return true;
}

bool testEndFromCallbackReturnsBusy() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<int> callbackStatus{-1};
	REQUIRE(pulse.setTimeout(
	    [&]() { callbackStatus.store(static_cast<int>(pulse.end(10).status)); },
	    50
	));
	REQUIRE(waitForQueueEmpty(pulse));
	fakeAdvanceTimeMs(50);
	fakeWakeAllTasks();
	REQUIRE(waitUntil([&]() { return callbackStatus.load() >= 0; }));
	REQUIRE(callbackStatus.load() == static_cast<int>(PulseStatus::Busy));
	REQUIRE(pulse.end());
	return true;
}

bool testSelfPausePreservesFullInterval() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<uint32_t> callbackCount{0};
	std::atomic<bool> pauseAccepted{false};
	PulseTimerId intervalId = 0;
	PulseTimerResult interval = pulse.setInterval(
	    [&]() {
		    const uint32_t count = ++callbackCount;
		    if (count == 1) {
			    pauseAccepted.store(static_cast<bool>(pulse.pause(intervalId)));
		    }
	    },
	    1000
	);
	REQUIRE(interval);
	intervalId = interval.id;
	REQUIRE(waitForQueueEmpty(pulse));

	fakeAdvanceTimeMs(1000);
	fakeWakeAllTasks();
	REQUIRE(waitUntil([&]() { return callbackCount.load() == 1; }));
	REQUIRE(pauseAccepted.load());
	REQUIRE(waitUntil([&]() { return pulse.getState(intervalId) == PulseTimerState::Paused; }));

	REQUIRE(pulse.resume(intervalId));
	REQUIRE(waitUntil([&]() { return pulse.getState(intervalId) == PulseTimerState::Running; }));
	REQUIRE(waitForQueueEmpty(pulse));

	fakeAdvanceTimeMs(900);
	fakeWakeAllTasks();
	std::this_thread::sleep_for(20ms);
	REQUIRE(callbackCount.load() == 1);

	fakeAdvanceTimeMs(100);
	fakeWakeAllTasks();
	REQUIRE(waitUntil([&]() { return callbackCount.load() == 2; }));
	REQUIRE(pulse.clear(intervalId));
	REQUIRE(pulse.end());
	return true;
}

bool testLongCallbackEndTimeoutAndRetry() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool release = false;

	REQUIRE(pulse.setTimeout(
	    [&]() {
		    std::unique_lock<std::mutex> lock(mutex);
		    entered = true;
		    condition.notify_all();
		    condition.wait(lock, [&]() { return release; });
	    },
	    25
	));
	REQUIRE(waitForQueueEmpty(pulse));
	fakeAdvanceTimeMs(25);
	fakeWakeAllTasks();
	{
		std::unique_lock<std::mutex> lock(mutex);
		REQUIRE(condition.wait_for(lock, 1s, [&]() { return entered; }));
	}

	PulseResult timedOut = pulse.end(5);
	REQUIRE(!timedOut);
	REQUIRE(timedOut.status == PulseStatus::Timeout);
	PulseTimerResult rejected = pulse.setTimeout([]() {}, 1);
	REQUIRE(!rejected);
	REQUIRE(rejected.status == PulseStatus::Busy);

	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	condition.notify_all();
	REQUIRE(pulse.end(1000));
	REQUIRE(pulse.init());
	REQUIRE(pulse.end());
	return true;
}

bool testRepeatedLifecycleCycles() {
	Pulse pulse;
	for (uint32_t index = 0; index < 20; index++) {
		REQUIRE(pulse.init());
		REQUIRE(pulse.end());
	}

	auto *selfOwned = new Pulse();
	REQUIRE(selfOwned->init());
	REQUIRE(waitForQueueEmpty(*selfOwned));
	REQUIRE(fakeActiveTaskCount() == 1);
	std::atomic<bool> destructorReturned{false};
	Pulse *callbackOwned = selfOwned;
	REQUIRE(selfOwned->setTimeout(
	    [callbackOwned, &destructorReturned]() {
		    delete callbackOwned;
		    destructorReturned = true;
	    },
	    1000
	));
	REQUIRE(waitForQueueEmpty(*selfOwned));
	fakeAdvanceTimeMs(1000);
	fakeWakeAllTasks();
	REQUIRE(waitUntil([&]() { return destructorReturned.load(); }));
	REQUIRE(waitUntil([]() { return fakeActiveTaskCount() == 0; }));
	return true;
}

struct TestCase {
	const char *name;
	bool (*run)();
};

constexpr TestCase tests[] = {
    {"stack-high-water-bytes", &testStackHighWaterUsesBytes},
    {"clear-already-due-timer", &testDueTimerCanBeClearedFromCallback},
    {"callback-end-busy", &testEndFromCallbackReturnsBusy},
    {"self-pause-interval", &testSelfPausePreservesFullInterval},
    {"long-callback-shutdown", &testLongCallbackEndTimeoutAndRetry},
    {"repeated-lifecycle", &testRepeatedLifecycleCycles},
};
} // namespace

int main(int argc, char **argv) {
	bool matched = argc == 1;
	for (const auto &test : tests) {
		if (argc > 1 && std::strcmp(argv[1], test.name) != 0) {
			continue;
		}
		matched = true;
		std::cout << "RUN: " << test.name << std::endl;
		if (!test.run()) {
			return 1;
		}
	}
	if (!matched) {
		std::cerr << "Unknown test: " << argv[1] << std::endl;
		return 2;
	}
	std::cout << "Selected Pulse host tests passed" << std::endl;
	return 0;
}
