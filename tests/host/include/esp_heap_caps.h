#pragma once

#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_8BIT ((uint32_t)1U)
#define MALLOC_CAP_SPIRAM ((uint32_t)2U)

#ifdef __cplusplus
extern "C" {
#endif

size_t heap_caps_get_total_size(uint32_t capabilities);

#ifdef __cplusplus
}
#endif
