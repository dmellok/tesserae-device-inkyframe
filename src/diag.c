#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/rand.h"
#include "hardware/watchdog.h"

#include "config.h"
#include "http.h"
#include "json_num.h"
#include "log.h"
#include "rtc.h"
#include "settings.h"
#include "storage.h"

static const char *s_wake = "unknown";
static uint32_t s_boot_epoch;
static bool s_watchdog_reset;

static const char *paint_error(uint8_t code)
{
    switch (code) {
    case DIAG_PAINT_TIMEOUT: return "panel refresh timed out";
    case DIAG_PAINT_INCOMPLETE: return "no complete frame to paint";
    default: return "paint failed";
    }
}

void diag_begin(const char *wake_name)
{
    s_wake = wake_name ? wake_name : "unknown";
    s_watchdog_reset = watchdog_caused_reboot();
    s_boot_epoch = pcf_time_valid() ? pcf_get_epoch() : 0;
}

uint32_t diag_add_status(cJSON *body)
{
    cJSON *logs = cJSON_AddObjectToObject(body, "logs");
    cJSON_AddNumberToObject(logs, "schema", 1);
    cJSON_AddNumberToObject(logs, "ring_bytes", LOG_RING_BYTES);
    uint32_t id, at; uint8_t code;
    storage_get_diag(&id, &at, &code);
    if (!id) return 0;
    cJSON *d = cJSON_AddObjectToObject(body, "diag");
    json_add_u64(d, "id", id);
    cJSON_AddStringToObject(d, "paint_error", paint_error(code));
    if (at) json_add_u64(d, "at", at);
    return id;
}

void diag_acknowledge(uint32_t sent_id)
{
    uint32_t id, at; uint8_t code;
    storage_get_diag(&id, &at, &code);
    if (sent_id && id == sent_id) storage_set_diag(0, 0, DIAG_NONE);
}

void diag_paint_failed(uint8_t code)
{
    uint32_t id = get_rand_32();
    if (!id) id = 1;
    storage_set_diag(id, pcf_time_valid() ? pcf_get_epoch() : 0, code);
    LOGW("diag: paint failure latched (%s)", paint_error(code));
}

// Replace every occurrence of `secret` in buf[0..n) with "[redacted]"-length
// padding, in place (the text keeps its length, so nothing shifts).
static void blank(char *buf, size_t n, const char *secret)
{
    size_t sl = secret ? strlen(secret) : 0;
    if (sl < 4) return;
    for (size_t i = 0; i + sl <= n; i++)
        if (memcmp(buf + i, secret, sl) == 0) { memset(buf + i, '*', sl); i += sl - 1; }
}

void diag_upload_log(const char *base, const char *device_id, const char *token)
{
    if (!token || !token[0]) return;
    // header + ring, under the server's 64 KB cap
    size_t cap = LOG_RING_BYTES + 256;
    char *body = malloc(cap + 1);
    if (!body) { LOGW("diag: no memory for the log upload"); return; }
    int h = snprintf(body, cap, "# tesserae-log v1 fw=%s boot=%lu reset=%s wake=%s epoch=%lu\n", FW_VERSION,
                     (unsigned long)s_boot_epoch, s_watchdog_reset ? "watchdog" : "power", s_wake,
                     (unsigned long)(pcf_time_valid() ? pcf_get_epoch() : 0));
    size_t n = (size_t)h + log_ring_copy(body + h, cap - (size_t)h);
    body[n] = 0;
    blank(body, n, token);
    blank(body, n, settings_pass());

    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/log", base, device_id);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    const char *hdrs[] = { auth, "Content-Type: text/plain; charset=utf-8" };
    http_resp_t r;
    int code = http_request("POST", url, hdrs, 2, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    http_resp_free(&r);
    free(body);
    LOGI("diag: uploaded %u bytes of log (http %d)", (unsigned)n, code);
}
