#pragma once

#include <stddef.h>
#include <stdint.h>

void fakeAdvanceTimeMs(uint64_t milliseconds);
void fakeWakeAllTasks();
void fakeSetStackHighWaterMark(size_t bytes);
