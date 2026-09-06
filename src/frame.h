// Frame download into the PSRAM frame store and hand-off to the panel.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Download `url` (must be exactly FRAME_BYTES long) into PSRAM. Returns the
// HTTP status (200 = complete frame in PSRAM), or a negative error.
int frame_download(const char *url);
size_t frame_received(void);
// Paint the frame in PSRAM. Call only after a complete download.
bool frame_paint(void);
