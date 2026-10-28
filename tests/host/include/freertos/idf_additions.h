#pragma once

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t coreId);

#ifdef __cplusplus
}
#endif
