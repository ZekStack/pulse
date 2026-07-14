#include <Pulse.h>
#include <host_freertos.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct TestFailure {
	std::string message;
};

#define REQUIRE(condition)                                                                    \
	do {                                                                                      \
		if (!(condition)) {                                                                  \
			throw TestFailure{std::string("requirement failed: ") + #condition + " at " + \
			                  __FILE__ + ":" + std::to_string(__LINE__)};                  \
		}                                                                                     \
	} while (false)

bool waitUntil(const std::function<bool()> &predicate, uint32_t timeoutMs = 1000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate()) {
			return true;
		}
		std::this_thread::yield();
	}
	return predicate();
}

void waitForQueueDrain(Pulse &pulse) {
	REQUIRE(waitUntil([&]() { return pulse.getDiagnostics().commandQueueUsed == 0; }));
}

void testStackHighWaterUsesBytes() {
	host_reset_time();
	host_set_stack_high_water_mark(777);
	Pulse pulse;
	REQUIRE(pulse.init());
	REQUIRE(waitUntil([&]() { return pulse.getDiagnostics().stackHighWaterMarkBytes == 777; }));
	REQUIRE(pulse.end());
	REQUIRE(pulse.getDiagnostics().stackHighWaterMarkBytes == 777);
}

void testTimeoutFiresOnce() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<uint32_t> calls{0};
	REQUIRE(pulse.setTimeout([&]() { calls.fetch_add(1); }, 100));
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return calls.load() == 1; }));
	host_advance_time_ms(1000);
	REQUIRE(calls.load() == 1);
	REQUIRE(pulse.end());
}

void testFinalCountdownRejectsRestart() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	PulseTimerId id = 0;
	std::atomic<bool> finished{false};
	std::atomic<PulseStatus> restartStatus{PulseStatus::InternalError};
	PulseCountdownConfig config{100, 100};
	auto result = pulse.setCountdown(config, [&](const PulseCountdownTick &tick) {
		if (tick.isFinished) {
			restartStatus.store(pulse.restart(id).status);
			finished.store(true);
		}
	});
	REQUIRE(result);
	id = result.id;
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return finished.load(); }));
	REQUIRE(restartStatus.load() == PulseStatus::TimerNotFound);
	REQUIRE(pulse.end());
}

void testCallbackClearsAnotherDueTimer() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	PulseTimerId secondId = 0;
	std::atomic<uint32_t> firstCalls{0};
	std::atomic<uint32_t> secondCalls{0};
	std::atomic<PulseStatus> clearStatus{PulseStatus::InternalError};

	auto first = pulse.setTimeout([&]() {
		firstCalls.fetch_add(1);
		clearStatus.store(pulse.clear(secondId).status);
	}, 100);
	REQUIRE(first);
	auto second = pulse.setTimeout([&]() { secondCalls.fetch_add(1); }, 100);
	REQUIRE(second);
	secondId = second.id;
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return firstCalls.load() == 1; }));
	REQUIRE(waitUntil([&]() { return pulse.getState(secondId) == PulseTimerState::NotFound; }));
	REQUIRE(clearStatus.load() == PulseStatus::Ok);
	REQUIRE(secondCalls.load() == 0);
	REQUIRE(pulse.end());
}

void testCallbackRestartIsAuthoritative() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	PulseTimerId id = 0;
	std::atomic<uint32_t> calls{0};
	std::atomic<PulseStatus> restartStatus{PulseStatus::InternalError};
	auto result = pulse.setInterval([&]() {
		const uint32_t count = calls.fetch_add(1) + 1;
		if (count == 1) {
			restartStatus.store(pulse.restart(id).status);
		}
	}, 100);
	REQUIRE(result);
	id = result.id;
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return calls.load() == 1; }));
	REQUIRE(waitUntil([&]() { return restartStatus.load() == PulseStatus::Ok; }));
	waitForQueueDrain(pulse);
	host_advance_time_ms(99);
	REQUIRE(calls.load() == 1);
	host_advance_time_ms(1);
	REQUIRE(waitUntil([&]() { return calls.load() == 2; }));
	REQUIRE(pulse.end());
}

void testCallbackPausePreservesFullInterval() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	PulseTimerId id = 0;
	std::atomic<uint32_t> calls{0};
	std::atomic<PulseStatus> pauseStatus{PulseStatus::InternalError};
	auto result = pulse.setInterval([&]() {
		calls.fetch_add(1);
		pauseStatus.store(pulse.pause(id).status);
	}, 100);
	REQUIRE(result);
	id = result.id;
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return calls.load() == 1; }));
	REQUIRE(waitUntil([&]() { return pulse.getState(id) == PulseTimerState::Paused; }));
	REQUIRE(pauseStatus.load() == PulseStatus::Ok);
	REQUIRE(pulse.resume(id));
	REQUIRE(waitUntil([&]() { return pulse.getState(id) == PulseTimerState::Running; }));
	waitForQueueDrain(pulse);
	host_advance_time_ms(99);
	REQUIRE(calls.load() == 1);
	host_advance_time_ms(1);
	REQUIRE(waitUntil([&]() { return calls.load() == 2; }));
	REQUIRE(pulse.clear(id));
	REQUIRE(pulse.end());
}

void testEndFromCallbackReturnsBusy() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<PulseStatus> status{PulseStatus::InternalError};
	std::atomic<bool> done{false};
	REQUIRE(pulse.setTimeout([&]() {
		status.store(pulse.end().status);
		done.store(true);
	}, 100));
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return done.load(); }));
	REQUIRE(status.load() == PulseStatus::Busy);
	REQUIRE(pulse.end());
}

void testEndTimeoutCanBeRetried() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool release = false;

	auto blockedTimer = pulse.setTimeout([&]() {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return release; });
	}, 100);
	REQUIRE(blockedTimer);
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	{
		std::unique_lock<std::mutex> lock(mutex);
		REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return entered; }));
	}

	PulseResult firstEnd;
	std::atomic<bool> firstEndFinished{false};
	std::thread endThread([&]() {
		firstEnd = pulse.end(20);
		firstEndFinished.store(true);
	});
	const bool stoppingObserved = waitUntil([&]() {
		auto result = pulse.setTimeout([]() {}, 10);
		return !result && result.status == PulseStatus::Busy;
	});
	for (uint32_t index = 0; index < 1000 && !firstEndFinished.load(); index++) {
		host_advance_time_ms(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const bool firstEndCompleted = firstEndFinished.load();
	if (firstEndCompleted) {
		endThread.join();
	}

	const PulseResult initDuringStopping = pulse.init();
	const PulseResult controlDuringStopping = pulse.clear(blockedTimer.id);

	std::atomic<bool> diagnosticsFinished{false};
	PulseDiag stoppingDiagnostics;
	std::thread diagnosticsThread([&]() {
		stoppingDiagnostics = pulse.getDiagnostics();
		diagnosticsFinished.store(true);
	});
	const bool diagnosticsWereSafe = waitUntil([&]() { return diagnosticsFinished.load(); }, 250);

	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	cv.notify_all();

	if (!firstEndCompleted) {
		for (uint32_t index = 0; index < 1000 && !firstEndFinished.load(); index++) {
			host_advance_time_ms(1);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		endThread.join();
	}
	diagnosticsThread.join();
	const PulseResult secondEnd = pulse.end();
	const PulseResult reinit = pulse.init();
	const PulseResult finalEnd = pulse.end();

	REQUIRE(stoppingObserved);
	REQUIRE(firstEndFinished.load());
	REQUIRE(!firstEnd);
	REQUIRE(firstEnd.status == PulseStatus::Timeout);
	REQUIRE(!initDuringStopping);
	REQUIRE(initDuringStopping.status == PulseStatus::Busy);
	REQUIRE(!controlDuringStopping);
	REQUIRE(controlDuringStopping.status == PulseStatus::Busy);
	REQUIRE(diagnosticsWereSafe);
	(void)stoppingDiagnostics;
	REQUIRE(secondEnd);
	REQUIRE(reinit);
	REQUIRE(finalEnd);
}


void testConcurrentEndWaiters() {
	host_reset_time();
	Pulse pulse;
	REQUIRE(pulse.init());
	PulseResult first;
	PulseResult second;
	std::thread firstThread([&]() { first = pulse.end(); });
	std::thread secondThread([&]() { second = pulse.end(); });
	firstThread.join();
	secondThread.join();
	REQUIRE(first);
	REQUIRE(second);
	REQUIRE(pulse.end());
	REQUIRE(pulse.init());
	REQUIRE(pulse.end());
}

void testDestructorWaitsForCallback() {
	host_reset_time();
	auto *pulse = new Pulse();
	REQUIRE(pulse->init());
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool release = false;
	std::atomic<bool> destructorFinished{false};

	REQUIRE(pulse->setTimeout([&]() {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return release; });
	}, 100));
	waitForQueueDrain(*pulse);
	host_advance_time_ms(100);
	{
		std::unique_lock<std::mutex> lock(mutex);
		REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return entered; }));
	}

	std::thread destructorThread([&]() {
		delete pulse;
		destructorFinished.store(true);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool waited = !destructorFinished.load();
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	cv.notify_all();
	destructorThread.join();
	REQUIRE(waited);
	REQUIRE(destructorFinished.load());
}

void testSelfDestructionIsSafe() {
	host_reset_time();
	auto *pulse = new Pulse();
	REQUIRE(pulse->init());
	std::atomic<bool> destructorReturned{false};
	REQUIRE(pulse->setTimeout([&]() {
		delete pulse;
		destructorReturned.store(true);
	}, 100));
	waitForQueueDrain(*pulse);
	host_advance_time_ms(100);
	REQUIRE(waitUntil([&]() { return destructorReturned.load(); }));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void testShutdownIgnoresFullCommandQueue() {
	host_reset_time();
	Pulse pulse;
	PulseConfig config;
	config.commandQueueSize = 1;
	REQUIRE(pulse.init(config));
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool release = false;
	PulseTimerId id = 0;

	auto timer = pulse.setInterval([&]() {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [&]() { return release; });
	}, 100);
	REQUIRE(timer);
	id = timer.id;
	waitForQueueDrain(pulse);
	host_advance_time_ms(100);
	{
		std::unique_lock<std::mutex> lock(mutex);
		REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return entered; }));
	}

	const PulseResult queued = pulse.pause(id);
	const PulseResult queueFull = pulse.restart(id);
	PulseResult firstEnd;
	std::atomic<bool> endFinished{false};
	std::thread endThread([&]() {
		firstEnd = pulse.end(20);
		endFinished.store(true);
	});
	for (uint32_t index = 0; index < 1000 && !endFinished.load(); index++) {
		host_advance_time_ms(1);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	cv.notify_all();
	endThread.join();
	const PulseResult secondEnd = pulse.end();

	REQUIRE(queued);
	REQUIRE(!queueFull);
	REQUIRE(queueFull.status == PulseStatus::QueueFull);
	REQUIRE(!firstEnd);
	REQUIRE(firstEnd.status == PulseStatus::Timeout);
	REQUIRE(secondEnd);
}

void testRepeatedLifecycle() {
	host_reset_time();
	Pulse pulse;
	for (int index = 0; index < 10; index++) {
		REQUIRE(pulse.init());
		REQUIRE(pulse.end());
	}
}

using Test = std::pair<const char *, void (*)()>;
} // namespace

int main() {
	const std::vector<Test> tests = {
	    {"stack high-water bytes", testStackHighWaterUsesBytes},
	    {"timeout fires once", testTimeoutFiresOnce},
	    {"final countdown rejects restart", testFinalCountdownRejectsRestart},
	    {"callback clears another due timer", testCallbackClearsAnotherDueTimer},
	    {"callback restart is authoritative", testCallbackRestartIsAuthoritative},
	    {"callback pause preserves interval", testCallbackPausePreservesFullInterval},
	    {"end from callback is busy", testEndFromCallbackReturnsBusy},
	    {"end timeout can be retried", testEndTimeoutCanBeRetried},
	    {"concurrent end waiters", testConcurrentEndWaiters},
	    {"destructor waits for callback", testDestructorWaitsForCallback},
	    {"self destruction is safe", testSelfDestructionIsSafe},
	    {"shutdown ignores full queue", testShutdownIgnoresFullCommandQueue},
	    {"repeated lifecycle", testRepeatedLifecycle},
	};

	for (const auto &[name, test] : tests) {
		try {
			test();
			std::cout << "PASS: " << name << '\n';
		} catch (const TestFailure &failure) {
			std::cerr << "FAIL: " << name << " - " << failure.message << '\n';
			return EXIT_FAILURE;
		} catch (const std::exception &error) {
			std::cerr << "FAIL: " << name << " - " << error.what() << '\n';
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}
