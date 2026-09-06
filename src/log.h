// USB-serial logging with a millisecond timestamp.
#pragma once

#include <stdio.h>
#include "pico/time.h"

#define LOG_LINE(level, fmt, ...) \
    printf("[%6lu] " level " " fmt "\n", (unsigned long)to_ms_since_boot(get_absolute_time()), ##__VA_ARGS__)

#define LOGI(fmt, ...) LOG_LINE("I", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_LINE("W", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_LINE("E", fmt, ##__VA_ARGS__)
