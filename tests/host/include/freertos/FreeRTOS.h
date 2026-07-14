#pragma once
#include <cstdint>
#include <climits>
using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;
using StackType_t = uint32_t;
using configSTACK_DEPTH_TYPE = uint32_t;
using EventBits_t = uint32_t;
struct HostTask;
struct HostQueue;
struct HostRecursiveMutex;
struct HostEventGroup;
using TaskHandle_t = HostTask *;
using QueueHandle_t = HostQueue *;
using SemaphoreHandle_t = HostRecursiveMutex *;
using EventGroupHandle_t = HostEventGroup *;
using TaskFunction_t = void (*)(void *);
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;
constexpr BaseType_t pdFAIL = 0;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr BaseType_t tskNO_AFFINITY = -1;
constexpr EventBits_t BIT0 = 1U << 0;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define configSUPPORT_STATIC_ALLOCATION 0
