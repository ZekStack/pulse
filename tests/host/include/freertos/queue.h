#pragma once

#include "FreeRTOS.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FakeQueue;
typedef struct FakeQueue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
QueueHandle_t xQueueCreateStatic(
    UBaseType_t length,
    UBaseType_t itemSize,
    uint8_t *storage,
    StaticQueue_t *queueBuffer
);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticksToWait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticksToWait);
BaseType_t xQueueSendFromISR(
    QueueHandle_t queue,
    const void *item,
    BaseType_t *higherPriorityTaskWoken
);
BaseType_t xQueueReceiveFromISR(
    QueueHandle_t queue,
    void *item,
    BaseType_t *higherPriorityTaskWoken
);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);

#ifdef __cplusplus
}
#endif
