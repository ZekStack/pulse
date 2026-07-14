#pragma once

#include "FreeRTOS.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FakeQueue;
typedef struct FakeQueue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticksToWait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticksToWait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);

#ifdef __cplusplus
}
#endif
