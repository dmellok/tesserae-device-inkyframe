// 8 MB SPI PSRAM (APS6404-class) on the display SPI bus, CS GP3. Used as the
// frame store: the 192,000-byte panel frame does not fit beside the Wi-Fi
// stack in the RP2040's SRAM. Sequences follow Pimoroni's psram_display driver.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool psram_init(void);
void psram_write(uint32_t addr, const uint8_t *data, size_t len);
void psram_fill(uint32_t addr, uint8_t byte, size_t len);
void psram_read(uint32_t addr, uint8_t *data, size_t len);
// Write/read self-test over a small region; returns true on success.
bool psram_test(void);
