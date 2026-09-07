#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct FakeSemaphore;
typedef struct FakeSemaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticksToWait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
	return xSemaphoreCreateRecursiveMutexStatic(buffer);
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticksToWait) {
	return xSemaphoreTakeRecursive(semaphore, ticksToWait);
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
	return xSemaphoreGiveRecursive(semaphore);
}

#ifdef __cplusplus
}
#endif
