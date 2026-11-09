// Host stub for the Pico SDK's pico/time.h: just enough for src/log.c.
#pragma once
#include <stdint.h>
typedef uint64_t absolute_time_t;
static inline absolute_time_t get_absolute_time(void) { return 0; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
