#include "Pulse.h"
#include "fake_freertos_test.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

bool waitForQueueEmpty(Pulse &pulse) {
	return waitUntil([&pulse]() { return pulse.getDiagnostics().commandQueueUsed == 0; });
}

void advanceAndWake(uint64_t milliseconds) {
	fakeAdvanceTimeMs(milliseconds);
	fakeWakeAllTasks();
}

#define REQUIRE(condition)                                                                    \
	do {                                                                                      \
		if (!(condition)) {                                                                  \
			std::cerr << "FAILED: " #condition " at " << __FILE__ << ':' << __LINE__       \
			          << std::endl;                                                           \
			return false;                                                                      \
		}                                                                                     \
	} while (false)

bool testEqualDeadlineOrdering() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::mutex mutex;
	std::vector<int> order;
	std::atomic<uint32_t> count{0};

	REQUIRE(pulse.setTimeout(
	    [&]() {
		    std::lock_guard<std::mutex> lock(mutex);
		    order.push_back(1);
		    count++;
	    },
	    1000
	));
	REQUIRE(pulse.setTimeout(
	    [&]() {
		    std::lock_guard<std::mutex> lock(mutex);
		    order.push_back(2);
		    count++;
	    },
	    1000
	));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return count.load() == 2; }));
	{
		std::lock_guard<std::mutex> lock(mutex);
		REQUIRE(order.size() == 2);
		REQUIRE(order[0] == 1);
		REQUIRE(order[1] == 2);
	}
	REQUIRE(pulse.end());
	return true;
}

bool testIntervalDelayAfterCallback() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<uint32_t> count{0};
	PulseTimerId id = 0;
	PulseTimerResult result = pulse.setInterval(
	    [&]() {
		    if (++count == 1) {
			    fakeAdvanceTimeMs(500);
		    }
	    },
	    1000
	);
	REQUIRE(result);
	id = result.id;
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return count.load() == 1; }));

	advanceAndWake(900);
	std::this_thread::sleep_for(20ms);
	REQUIRE(count.load() == 1);
	advanceAndWake(100);
	REQUIRE(waitUntil([&]() { return count.load() == 2; }));
	REQUIRE(pulse.clear(id));
	REQUIRE(pulse.end());
	return true;
}

bool testNonDivisibleCountdownAndTerminalControl() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::mutex mutex;
	std::vector<PulseCountdownTick> ticks;
	std::atomic<int> terminalStatus{-1};
	PulseTimerId id = 0;
	PulseCountdownConfig config;
	config.durationMs = 2500;
	config.tickMs = 1000;
	PulseTimerResult result = pulse.setCountdown(
	    config,
	    [&](const PulseCountdownTick &tick) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    ticks.push_back(tick);
		    }
		    if (tick.isFinished) {
			    terminalStatus.store(static_cast<int>(pulse.restart(id).status));
		    }
	    }
	);
	REQUIRE(result);
	id = result.id;
	REQUIRE(waitForQueueEmpty(pulse));

	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() {
		std::lock_guard<std::mutex> lock(mutex);
		return ticks.size() == 1;
	}));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() {
		std::lock_guard<std::mutex> lock(mutex);
		return ticks.size() == 2;
	}));
	advanceAndWake(500);
	REQUIRE(waitUntil([&]() {
		std::lock_guard<std::mutex> lock(mutex);
		return ticks.size() == 3;
	}));

	{
		std::lock_guard<std::mutex> lock(mutex);
		REQUIRE(ticks[0].elapsedMs == 1000);
		REQUIRE(ticks[1].elapsedMs == 2000);
		REQUIRE(ticks[2].elapsedMs == 2500);
		REQUIRE(ticks[2].remainingMs == 0);
		REQUIRE(ticks[2].isFinished);
	}
	REQUIRE(terminalStatus.load() == static_cast<int>(PulseStatus::TimerNotFound));
	REQUIRE(pulse.end());
	return true;
}

bool schedulerSemantics() {
	return testEqualDeadlineOrdering() && testIntervalDelayAfterCallback() &&
	       testNonDivisibleCountdownAndTerminalControl();
}

bool testTimeoutRestart() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<uint32_t> count{0};
	PulseTimerResult timer = pulse.setTimeout([&]() { count++; }, 1000);
	REQUIRE(timer);
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(400);
	REQUIRE(pulse.restart(timer.id));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(900);
	std::this_thread::sleep_for(20ms);
	REQUIRE(count.load() == 0);
	advanceAndWake(100);
	REQUIRE(waitUntil([&]() { return count.load() == 1; }));
	REQUIRE(pulse.end());
	return true;
}

bool testIntervalRestartAndExternalPause() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::atomic<uint32_t> count{0};
	PulseTimerResult timer = pulse.setInterval([&]() { count++; }, 2000);
	REQUIRE(timer);
	REQUIRE(waitForQueueEmpty(pulse));

	advanceAndWake(400);
	REQUIRE(pulse.restart(timer.id));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1800);
	std::this_thread::sleep_for(20ms);
	REQUIRE(count.load() == 0);
	advanceAndWake(200);
	REQUIRE(waitUntil([&]() { return count.load() == 1; }));

	advanceAndWake(500);
	REQUIRE(pulse.pause(timer.id));
	REQUIRE(waitUntil([&]() { return pulse.getState(timer.id) == PulseTimerState::Paused; }));
	REQUIRE(pulse.resume(timer.id));
	REQUIRE(waitUntil([&]() { return pulse.getState(timer.id) == PulseTimerState::Running; }));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1300);
	std::this_thread::sleep_for(20ms);
	REQUIRE(count.load() == 1);
	advanceAndWake(300);
	REQUIRE(waitUntil([&]() { return count.load() == 2; }));

	REQUIRE(pulse.clear(timer.id));
	REQUIRE(pulse.end());
	return true;
}

bool testCountdownRestartAndSelfPause() {
	Pulse pulse;
	REQUIRE(pulse.init());
	std::mutex mutex;
	std::vector<uint32_t> elapsed;
	std::atomic<bool> pauseAccepted{false};
	PulseTimerId id = 0;
	PulseCountdownConfig config;
	config.durationMs = 5000;
	config.tickMs = 1000;
	PulseTimerResult timer = pulse.setCountdown(
	    config,
	    [&](const PulseCountdownTick &tick) {
		    {
			    std::lock_guard<std::mutex> lock(mutex);
			    elapsed.push_back(tick.elapsedMs);
		    }
		    if (tick.elapsedMs == 1000 && elapsed.size() == 1) {
			    pauseAccepted.store(static_cast<bool>(pulse.pause(id)));
		    }
	    }
	);
	REQUIRE(timer);
	id = timer.id;
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return pulse.getState(id) == PulseTimerState::Paused; }));
	REQUIRE(pauseAccepted.load());

	REQUIRE(pulse.resume(id));
	REQUIRE(waitUntil([&]() { return pulse.getState(id) == PulseTimerState::Running; }));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(900);
	std::this_thread::sleep_for(20ms);
	{
		std::lock_guard<std::mutex> lock(mutex);
		REQUIRE(elapsed.size() == 1);
	}
	advanceAndWake(100);
	REQUIRE(waitUntil([&]() {
		std::lock_guard<std::mutex> lock(mutex);
		return elapsed.size() == 2;
	}));

	REQUIRE(pulse.restart(id));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() {
		std::lock_guard<std::mutex> lock(mutex);
		return elapsed.size() == 3;
	}));
	{
		std::lock_guard<std::mutex> lock(mutex);
		REQUIRE(elapsed.back() == 1000);
	}
	REQUIRE(pulse.clear(id));
	REQUIRE(pulse.end());
	return true;
}

bool restartAndPauseSemantics() {
	return testTimeoutRestart() && testIntervalRestartAndExternalPause() &&
	       testCountdownRestartAndSelfPause();
}

struct NotificationResumeGuard {
	~NotificationResumeGuard() {
		fakeResumeTaskNotifications();
	}
};

bool queueFullAndShutdownPriority() {
	Pulse pulse;
	PulseConfig config;
	config.commandQueueSize = 1;
	config.maxTimeouts = 4;
	REQUIRE(pulse.init(config));
	REQUIRE(waitForQueueEmpty(pulse));

	fakeSuspendTaskNotifications();
	NotificationResumeGuard guard;
	PulseTimerResult first = pulse.setTimeout([]() {}, 10000);
	REQUIRE(first);
	PulseTimerResult second = pulse.setTimeout([]() {}, 10000);
	REQUIRE(!second);
	REQUIRE(second.status == PulseStatus::QueueFull);
	PulseDiag diag = pulse.getDiagnostics();
	REQUIRE(diag.commandQueueUsed == 1);
	REQUIRE(diag.timeoutCount == 1);
	REQUIRE(diag.droppedCommandCount == 1);

	PulseResult timed = pulse.end(5);
	REQUIRE(!timed);
	REQUIRE(timed.status == PulseStatus::Timeout);
	fakeResumeTaskNotifications();
	REQUIRE(pulse.end(1000));
	return true;
}

bool testConcurrentFirstInit() {
	Pulse pulse;
	std::barrier start(3);
	PulseResult first;
	PulseResult second;
	std::thread one([&]() {
		start.arrive_and_wait();
		first = pulse.init();
	});
	std::thread two([&]() {
		start.arrive_and_wait();
		second = pulse.init();
	});
	start.arrive_and_wait();
	one.join();
	two.join();

	REQUIRE(static_cast<bool>(first) != static_cast<bool>(second));
	const PulseStatus rejectedStatus = first ? second.status : first.status;
	REQUIRE(
	    rejectedStatus == PulseStatus::AlreadyInitialized || rejectedStatus == PulseStatus::Busy
	);
	std::atomic<bool> fired{false};
	REQUIRE(pulse.setTimeout([&]() { fired = true; }, 1000));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return fired.load(); }));
	REQUIRE(pulse.end());
	return true;
}

bool testStaleEndWaiterCannotStopNewGeneration() {
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
	    1000
	));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	{
		std::unique_lock<std::mutex> lock(mutex);
		REQUIRE(condition.wait_for(lock, 1s, [&]() { return entered; }));
	}

	PulseResult staleResult;
	std::thread staleWaiter([&]() {
		fakeHoldCurrentEventWaiter();
		staleResult = pulse.end(5000);
	});
	REQUIRE(waitUntil([&]() {
		return pulse.setTimeout([]() {}, 1).status == PulseStatus::Busy;
	}));
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	condition.notify_all();
	REQUIRE(fakeWaitForHeldEventWaiter(1000));
	REQUIRE(pulse.end(1000));
	REQUIRE(pulse.init());
	fakeReleaseHeldEventWaiters();
	staleWaiter.join();
	REQUIRE(staleResult);

	std::atomic<bool> fired{false};
	REQUIRE(pulse.setTimeout([&]() { fired = true; }, 1000));
	REQUIRE(waitForQueueEmpty(pulse));
	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return fired.load(); }));
	REQUIRE(pulse.end());
	return true;
}

bool testDestructorWaitsForCallback() {
	auto *pulse = new Pulse();
	REQUIRE(pulse->init());
	std::mutex mutex;
	std::condition_variable condition;
	bool entered = false;
	bool release = false;
	REQUIRE(pulse->setTimeout(
	    [&]() {
		    std::unique_lock<std::mutex> lock(mutex);
		    entered = true;
		    condition.notify_all();
		    condition.wait(lock, [&]() { return release; });
	    },
	    1000
	));
	REQUIRE(waitForQueueEmpty(*pulse));
	advanceAndWake(1000);
	{
		std::unique_lock<std::mutex> lock(mutex);
		REQUIRE(condition.wait_for(lock, 1s, [&]() { return entered; }));
	}

	std::atomic<bool> deleted{false};
	std::thread destroyer([&]() {
		delete pulse;
		deleted = true;
	});
	std::this_thread::sleep_for(30ms);
	REQUIRE(!deleted.load());
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = true;
	}
	condition.notify_all();
	destroyer.join();
	REQUIRE(deleted.load());
	return true;
}

bool lifecycleConcurrency() {
	return testConcurrentFirstInit() && testStaleEndWaiterCannotStopNewGeneration() &&
	       testDestructorWaitsForCallback();
}

struct ReentrantCapture {
	Pulse *pulse = nullptr;
	std::atomic<bool> *destroyed = nullptr;

	~ReentrantCapture() {
		std::atomic<bool> diagnosticReturned{false};
		std::thread reader([&]() {
			(void)pulse->getDiagnostics();
			diagnosticReturned = true;
		});
		reader.join();
		if (diagnosticReturned.load()) {
			*destroyed = true;
		}
	}
};

bool cleanupAndDiagnostics() {
	auto *pulse = new Pulse();
	REQUIRE(pulse->init());
	std::atomic<bool> captureDestroyed{false};
	auto capture = std::make_shared<ReentrantCapture>();
	capture->pulse = pulse;
	capture->destroyed = &captureDestroyed;
	REQUIRE(pulse->setInterval([capture]() {}, 10000));
	capture.reset();
	REQUIRE(waitForQueueEmpty(*pulse));

	PulseTimerResult timeout = pulse->setTimeout([]() {}, 10000);
	PulseCountdownConfig countdownConfig;
	countdownConfig.durationMs = 10000;
	countdownConfig.tickMs = 1000;
	PulseTimerResult countdown = pulse->setCountdown(countdownConfig, [](const auto &) {});
	REQUIRE(timeout && countdown);
	REQUIRE(waitForQueueEmpty(*pulse));
	PulseDiag before = pulse->getDiagnostics();
	REQUIRE(before.timeoutCount == 1);
	REQUIRE(before.intervalCount == 1);
	REQUIRE(before.countdownCount == 1);
	REQUIRE(before.runningCount == 3);

	REQUIRE(pulse->pause(timeout.id));
	REQUIRE(waitUntil([&]() { return pulse->getState(timeout.id) == PulseTimerState::Paused; }));
	PulseDiag paused = pulse->getDiagnostics();
	REQUIRE(paused.pausedCount == 1);
	REQUIRE(paused.runningCount == 2);

	REQUIRE(pulse->end(1000));
	REQUIRE(captureDestroyed.load());
	delete pulse;
	return true;
}

bool callbackCommandBatch() {
	Pulse pulse;
	PulseConfig config;
	config.commandQueueSize = 12;
	REQUIRE(pulse.init(config));
	std::atomic<uint32_t> firstCount{0};
	std::atomic<uint32_t> secondCount{0};
	std::atomic<uint32_t> thirdCount{0};
	std::atomic<bool> commandsAccepted{true};
	PulseTimerId secondId = 0;
	PulseTimerId thirdId = 0;

	REQUIRE(pulse.setTimeout(
	    [&]() {
		    firstCount++;
		    for (uint32_t index = 0; index < 5; index++) {
			    if (!pulse.restart(thirdId)) {
				    commandsAccepted = false;
			    }
		    }
		    if (!pulse.clear(secondId)) {
			    commandsAccepted = false;
		    }
	    },
	    1000
	));
	PulseTimerResult second = pulse.setTimeout([&]() { secondCount++; }, 1000);
	PulseTimerResult third = pulse.setTimeout([&]() { thirdCount++; }, 1000);
	REQUIRE(second && third);
	secondId = second.id;
	thirdId = third.id;
	REQUIRE(waitForQueueEmpty(pulse));

	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return firstCount.load() == 1; }));
	REQUIRE(commandsAccepted.load());
	std::this_thread::sleep_for(30ms);
	REQUIRE(secondCount.load() == 0);
	REQUIRE(thirdCount.load() == 0);

	advanceAndWake(1000);
	REQUIRE(waitUntil([&]() { return thirdCount.load() == 1; }));
	REQUIRE(secondCount.load() == 0);
	REQUIRE(pulse.end());
	return true;
}

struct TestCase {
	const char *name;
	bool (*run)();
};

constexpr TestCase tests[] = {
    {"scheduler-semantics", &schedulerSemantics},
    {"restart-and-pause", &restartAndPauseSemantics},
    {"queue-and-shutdown", &queueFullAndShutdownPriority},
    {"lifecycle-concurrency", &lifecycleConcurrency},
    {"cleanup-and-diagnostics", &cleanupAndDiagnostics},
    {"callback-command-batch", &callbackCommandBatch},
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
	std::cout << "Selected extended Pulse host tests passed" << std::endl;
	return 0;
}
