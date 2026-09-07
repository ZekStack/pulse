#pragma once

#include "FreeRTOS.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PendedFunction_t)(void *, uint32_t);

BaseType_t xTimerPendFunctionCall(
    PendedFunction_t function,
    void *parameter1,
    uint32_t parameter2,
    TickType_t ticksToWait
);

#ifdef __cplusplus
}
#endif
