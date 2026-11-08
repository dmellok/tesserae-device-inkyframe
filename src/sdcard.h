// The Inky Frame's microSD slot: an SD card in SPI mode on SPI0 (shared with
// the panel and the PSRAM; chip-select GPIO 22), with FatFs on top. The cache
// lives in /tesserae on the card, so anything else on it is left alone.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Detect, initialise and mount the card. False when there is no card, or it
// cannot be read (the cache then stays off; nothing else depends on it).
bool sdcard_mount(void);
bool sdcard_mounted(void);
// Free space on the card, in bytes (0 when not mounted).
uint64_t sdcard_free_bytes(void);
