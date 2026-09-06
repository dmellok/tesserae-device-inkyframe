#include "tesserae.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "vendor/cJSON.h"
#include "wifi.h"

static void json_str(const cJSON *o, const char *k, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
    else if (cap) out[0] = 0;
}

static int32_t json_int(const cJSON *o, const char *k, int32_t dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : dflt;
}

static uint32_t json_u32(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
}

static char *identity_json(const tess_identity_t *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_id", id->device_id);
    cJSON_AddStringToObject(o, "kind", id->kind);
    cJSON_AddNumberToObject(o, "panel_w", PANEL_W);
    cJSON_AddNumberToObject(o, "panel_h", PANEL_H);
    cJSON_AddNumberToObject(o, "rotation", PANEL_ROTATION);
    cJSON_AddStringToObject(o, "gamut", id->gamut);
    cJSON_AddStringToObject(o, "name", DEVICE_NAME);
    cJSON_AddStringToObject(o, "fw_version", FW_VERSION);
    cJSON_AddStringToObject(o, "mac", id->mac);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void parse_pair(http_resp_t *r, tess_pair_t *out)
{
    memset(out, 0, sizeof *out);
    out->code = r->status;
    out->sleep_interval_s = -1;
    out->retry_after_s = r->retry_after;
    if (r->status <= 0 || !r->body) return;
    cJSON *o = cJSON_ParseWithLength(r->body, r->body_len);
    if (!o) { LOGW("pair: bad JSON"); return; }
    json_str(o, "device_token", out->token, sizeof out->token);
    out->registered = out->token[0] != 0;
    json_str(o, "device_id", out->device_id, sizeof out->device_id);
    json_str(o, "error", out->error, sizeof out->error);
    // JSON first, then the header, so an explicit Retry-After wins (matching
    // the reference client's precedence).
    int ra = (int)json_int(o, "retry_after_s", 0);
    if (ra > 0) out->retry_after_s = ra;
    if (r->retry_after > 0) out->retry_after_s = r->retry_after;
    out->server_time = json_u32(o, "server_time");
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(o, "config");
    if (cfg) out->sleep_interval_s = json_int(cfg, "sleep_interval_s", -1);
    cJSON_Delete(o);
}

static void bearer(char *out, size_t cap, const char *token)
{
    snprintf(out, cap, "Authorization: Bearer %s", token);
}

tess_kind_probe_t tess_probe_kind(const char *base, const tess_identity_t *id)
{
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/register", base);
    char *body = identity_json(id);
    const char *hdrs[] = { "X-Pairing-Code: 000000" };
    http_resp_t r;
    http_request("POST", url, hdrs, 1, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_DISCOVER_MS, 0);
    free(body);
    tess_kind_probe_t res = TESS_KIND_UNKNOWN;
    if (r.status == 400 && r.body && strstr(r.body, "unknown device kind")) {
        LOGI("kind probe: '%s' is not installed on the server", id->kind);
        res = TESS_KIND_MISSING;
    } else if (r.status == 403 || r.status == 401) {
        LOGI("kind probe: '%s' exists on the server", id->kind);
        res = TESS_KIND_EXISTS;
    } else if (r.status > 0 && r.status != 429) {
        res = TESS_KIND_EXISTS;
    }
    http_resp_free(&r);
    return res;
}

void tess_discover(const char *base, const tess_identity_t *id, tess_pair_t *out)
{
    char url[256];
    snprintf(url, sizeof url, "%s/api/v1/device/discover", base);
    char *body = identity_json(id);
    http_resp_t r;
    http_request("POST", url, NULL, 0, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_DISCOVER_MS, 0);
    free(body);
    parse_pair(&r, out);
    http_resp_free(&r);
}

void tess_register(const char *base, const tess_identity_t *id, const char *code, tess_pair_t *out)
{
    char url[256], hdr[48];
    snprintf(url, sizeof url, "%s/api/v1/device/register", base);
    snprintf(hdr, sizeof hdr, "X-Pairing-Code: %s", code);
    const char *hdrs[] = { hdr };
    char *body = identity_json(id);
    http_resp_t r;
    http_request("POST", url, hdrs, 1, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_DISCOVER_MS, 0);
    free(body);
    parse_pair(&r, out);
    http_resp_free(&r);
}

void tess_get_frame(const char *base, const char *device_id, const char *token, const char *etag,
                    const char *button, uint32_t event_id, tess_frame_t *out)
{
    memset(out, 0, sizeof *out);
    out->button_wake_s = -1;
    char url[320];
    int n = snprintf(url, sizeof url, "%s/api/v1/device/%s/frame", base, device_id);
    if (button && button[0]) snprintf(url + n, sizeof url - n, "?button=%s&button_event_id=%lu", button, (unsigned long)event_id);

    char auth[300], inm[128];
    bearer(auth, sizeof auth, token);
    const char *hdrs[2] = { auth, NULL };
    int nh = 1;
    if (etag && etag[0]) { snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", etag); hdrs[1] = inm; nh = 2; }

    http_resp_t r;
    http_request("GET", url, hdrs, nh, NULL, &r, NULL, NULL, NULL, HTTP_TIMEOUT_FRAME_MS, 0);
    out->code = r.status;
    snprintf(out->etag, sizeof out->etag, "%s", r.etag);
    if (r.status == 200 && r.body) {
        cJSON *o = cJSON_ParseWithLength(r.body, r.body_len);
        if (!o) { LOGW("frame: bad JSON"); out->code = -10; }
        else {
            char raw[256];
            json_str(o, "url", raw, sizeof raw);
            json_str(o, "format", out->format, sizeof out->format);
            char render_id[80];
            json_str(o, "render_id", render_id, sizeof render_id);
            if (!out->etag[0]) snprintf(out->etag, sizeof out->etag, "%s", render_id);
            out->panel_w = (int)json_int(o, "native_w", json_int(o, "panel_w", 0));
            out->panel_h = (int)json_int(o, "native_h", json_int(o, "panel_h", 0));
            out->button_wake_s = json_int(o, "button_wake_s", -1);
            cJSON_Delete(o);
            if (raw[0] == '/') { char full[256]; snprintf(full, sizeof full, "%s%s", base, raw); wifi_rewrite_host(full, out->url, sizeof out->url); }
            else wifi_rewrite_host(raw, out->url, sizeof out->url);
        }
    }
    http_resp_free(&r);
}

void tess_post_status(const char *base, const char *device_id, const char *token,
                      const tess_telemetry_t *t, tess_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->next_poll_s = -1; out->sleep_interval_s = -1; out->button_wake_s = -1; out->always_on = -1; out->awake_poll_s = -1;

    cJSON *o = cJSON_CreateObject();
    if (t->battery_mv) cJSON_AddNumberToObject(o, "battery_mv", t->battery_mv);
    cJSON_AddNumberToObject(o, "rssi", t->rssi);
    cJSON_AddStringToObject(o, "ip", t->ip);
    cJSON_AddStringToObject(o, "mac", t->mac);
    cJSON_AddStringToObject(o, "fw_version", FW_VERSION);
    cJSON_AddStringToObject(o, "model", DEVICE_NAME);
    cJSON_AddNumberToObject(o, "panel_w", PANEL_W);
    cJSON_AddNumberToObject(o, "panel_h", PANEL_H);
    if (t->can_stay_awake) cJSON_AddBoolToObject(o, "can_stay_awake", true);
    if (t->button && t->button[0]) {
        cJSON_AddStringToObject(o, "button", t->button);
        cJSON_AddNumberToObject(o, "button_event_id", t->button_event_id);
    }
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/status", base, device_id);
    bearer(auth, sizeof auth, token);
    const char *hdrs[] = { auth };
    http_resp_t r;
    http_request("POST", url, hdrs, 1, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    free(body);
    out->code = r.status;
    if (r.status == 200 && r.body) {
        cJSON *j = cJSON_ParseWithLength(r.body, r.body_len);
        if (!j) LOGW("status: bad JSON%s", r.truncated ? " (truncated)" : "");
        else {
            out->next_poll_s = json_int(j, "next_poll_s", -1);
            out->wake_at = json_u32(j, "wake_at");
            out->server_time = json_u32(j, "server_time");
            const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(j, "config");
            if (cfg) {
                out->sleep_interval_s = json_int(cfg, "sleep_interval_s", -1);
                out->button_wake_s = json_int(cfg, "button_wake_s", -1);
                out->awake_poll_s = json_int(cfg, "awake_poll_s", -1);
                const cJSON *ao = cJSON_GetObjectItemCaseSensitive(cfg, "always_on");
                if (cJSON_IsBool(ao)) out->always_on = cJSON_IsTrue(ao) ? 1 : 0;
            }
            cJSON_Delete(j);
        }
    }
    if (out->server_time == 0 && r.date_epoch) out->server_time = r.date_epoch;
    http_resp_free(&r);
}

void tess_post_log(const char *base, const char *device_id, const char *token, const char *level, const char *msg)
{
    if (!token || !token[0]) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "level", level);
    cJSON_AddStringToObject(o, "msg", msg);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char url[256], auth[300];
    snprintf(url, sizeof url, "%s/api/v1/device/%s/log", base, device_id);
    bearer(auth, sizeof auth, token);
    const char *hdrs[] = { auth };
    http_resp_t r;
    http_request("POST", url, hdrs, 1, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    free(body);
    http_resp_free(&r);
}
