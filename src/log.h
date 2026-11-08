// Logging: every line goes to USB serial with a millisecond timestamp, and
// into a RAM ring the server can ask for (Tesserae's device log protocol v1,
// see diag.h).
#pragma once

#include <stddef.h>
#include <stdio.h>
#include "pico/time.h"

void log_emit(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOGI(fmt, ...) log_emit('I', fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) log_emit('W', fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) log_emit('E', fmt, ##__VA_ARGS__)

// The ring's capacity, advertised to the server.
#ifdef INKY_LOW_RAM
#define LOG_RING_BYTES (8 * 1024)
#else
#define LOG_RING_BYTES (24 * 1024)
#endif

// Copy the ring (oldest line first) into out; returns the length.
size_t log_ring_copy(char *out, size_t cap);
