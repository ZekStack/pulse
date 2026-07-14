#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY ((BaseType_t)-1)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define INCLUDE_uxTaskGetStackHighWaterMark 1

#ifdef __cplusplus
}
#endif
