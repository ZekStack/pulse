#pragma once

#include <stdint.h>

// Pulse targets ESP-IDF's FreeRTOS port. Keep the host task fake on the same
// stack-depth and high-water-mark byte semantics used by ESP32 while the
// Strata allocator itself continues to use its generic host backend.
#define ESP32 1

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;

typedef struct StaticTask_t {
	uintptr_t opaque[8];
} StaticTask_t;

typedef struct StaticQueue_t {
	uintptr_t opaque[8];
} StaticQueue_t;

typedef struct StaticSemaphore_t {
	uintptr_t opaque[8];
} StaticSemaphore_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY ((BaseType_t)-1)
#define tskIDLE_PRIORITY ((UBaseType_t)0)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define configSUPPORT_STATIC_ALLOCATION 1
#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 1
#define configUSE_TIMERS 1
#define INCLUDE_vTaskDelete 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTimerPendFunctionCall 1

#ifdef __cplusplus
}
#endif