#include "frame.h"

#include "config.h"
#include "epd.h"
#include "http.h"
#include "log.h"
#include "psram.h"

static volatile size_t s_received;
static bool s_complete;

static bool on_length(size_t len, void *arg)
{
    (void)arg;
    if (len != FRAME_BYTES) {
        LOGE("frame: unexpected length %u (want %u); refusing", (unsigned)len, (unsigned)FRAME_BYTES);
        return false;
    }
    s_received = 0;
    return true;
}

static void sink(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    if (s_received + len > FRAME_BYTES) len = FRAME_BYTES - s_received;
    psram_write((uint32_t)s_received, data, len);
    s_received += len;
}

int frame_download(const char *url)
{
    s_received = 0;
    s_complete = false;
    http_resp_t r;
    int code = http_request("GET", url, NULL, 0, NULL, &r, on_length, sink, NULL,
                            HTTP_TIMEOUT_DOWNLOAD_MS, DOWNLOAD_STALL_TIMEOUT_MS);
    http_resp_free(&r);
    s_complete = (code == 200 && s_received == FRAME_BYTES);
    if (code == 200 && !s_complete) {
        LOGE("frame: incomplete download (%u/%u bytes)", (unsigned)s_received, (unsigned)FRAME_BYTES);
        return HTTP_ERR_TRANSPORT;
    }
    return code;
}

size_t frame_received(void) { return s_received; }

void frame_set_complete(bool complete) { s_complete = complete; s_received = complete ? FRAME_BYTES : 0; }

static void psram_row(int y, uint8_t *out, void *arg)
{
    (void)arg;
    psram_read((uint32_t)y * ROW_BYTES, out, ROW_BYTES);
}

bool frame_paint(void)
{
    if (!s_complete) { LOGE("frame: paint without a complete frame"); return false; }
    return epd_paint(psram_row, NULL);
}
