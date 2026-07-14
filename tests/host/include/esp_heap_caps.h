#pragma once
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_8BIT 0x1
#define MALLOC_CAP_SPIRAM 0x2
inline std::size_t heap_caps_get_total_size(uint32_t) { return 0; }
