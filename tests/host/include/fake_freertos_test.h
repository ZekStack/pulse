#pragma once

#include <stddef.h>
#include <stdint.h>

void fakeAdvanceTimeMs(uint64_t milliseconds);
void fakeWakeAllTasks();
void fakeSetStackHighWaterMark(size_t bytes);
size_t fakeActiveTaskCount();

void fakeSuspendTaskNotifications();
void fakeResumeTaskNotifications();

void fakeHoldCurrentEndWaiter();
bool fakeWaitForHeldEndWaiter(uint32_t timeoutMs);
void fakeReleaseHeldEndWaiters();

inline void fakeHoldCurrentEventWaiter() {
	fakeHoldCurrentEndWaiter();
}

inline bool fakeWaitForHeldEventWaiter(uint32_t timeoutMs) {
	return fakeWaitForHeldEndWaiter(timeoutMs);
}

inline void fakeReleaseHeldEventWaiters() {
	fakeReleaseHeldEndWaiters();
}
