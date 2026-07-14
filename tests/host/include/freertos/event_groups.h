#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t EventBits_t;
#define BIT0 ((EventBits_t)1U)

struct FakeEventGroup;
typedef struct FakeEventGroup *EventGroupHandle_t;

EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t eventGroup, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t eventGroup, EventBits_t bits);
EventBits_t xEventGroupWaitBits(
    EventGroupHandle_t eventGroup,
    EventBits_t bitsToWaitFor,
    BaseType_t clearOnExit,
    BaseType_t waitForAllBits,
    TickType_t ticksToWait
);
void vEventGroupDelete(EventGroupHandle_t eventGroup);

#ifdef __cplusplus
}
#endif
