#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct FakeTaskControl;
typedef struct FakeTaskControl *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(
    TaskFunction_t entry,
    const char *name,
    uint32_t stackDepth,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle
);

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry,
    const char *name,
    uint32_t stackDepth,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t coreId
);

void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t handle);
uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticksToWait);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle);

#ifdef __cplusplus
}
#endif
