#pragma once
#include <cstdint>
void host_reset_time();
void host_advance_time_ms(uint64_t milliseconds);
void host_set_stack_high_water_mark(uint32_t bytes);
