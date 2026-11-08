#include "relay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "config.h"
#include "frame.h"
#include "http.h"
#include "log.h"
#include "ota_verify.h"     // ota_b64url_decode
#include "psram.h"
#include "relay_crypto.h"
#include "settings.h"
#include "vendor/cJSON.h"

#define SEALED_FRAME_BYTES (FRAME_BYTES + RELAY_SEAL_OVERHEAD)

static void b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (acc << 8) | in[i]; bits += 8;
        while (bits >= 6 && o + 1 < cap) { bits -= 6; out[o++] = A[(acc >> bits) & 63]; }
    }
    if (bits > 0 && o + 1 < cap) out[o++] = A[(acc << (6 - bits)) & 63];
    out[o] = 0;
}

static void url_for(char *out, size_t cap, const char *what)
{
    snprintf(out, cap, "%s/v1/i/%s/d/%s/%s", settings_relay_url(), settings_relay_install(), settings_relay_device(), what);
}

static void bearer(char *out, size_t cap)
{
    snprintf(out, cap, "Authorization: Bearer %s", settings_relay_token());
}

// ---- pairing ----

relay_pair_result_t relay_pair(uint32_t budget_ms)
{
    if (settings_relay_paired()) return RELAY_PAIRED;
    const char *code = settings_relay_code();
    if (!code[0]) return RELAY_CODE_DEAD;
    char url[256];

    if (!settings_relay_priv()) {
        uint8_t priv[32], pub[32];
        for (int i = 0; i < 32; i += 4) { uint32_t r = get_rand_32(); memcpy(priv + i, &r, 4); }
        relay_public_key(priv, pub);
        char pub64[48];
        b64url_encode(pub, 32, pub64, sizeof pub64);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "code", code);
        cJSON_AddStringToObject(o, "panel_pubkey", pub64);
        cJSON_AddNumberToObject(o, "panel_w", PANEL_W);
        cJSON_AddNumberToObject(o, "panel_h", PANEL_H);
        cJSON_AddStringToObject(o, "model", DEVICE_KIND);
        cJSON_AddStringToObject(o, "gamut", DEVICE_GAMUT);
        char *body = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        snprintf(url, sizeof url, "%s/v1/pair", settings_relay_url());
        http_resp_t r;
        int st = http_request("POST", url, NULL, 0, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
        http_resp_free(&r);
        free(body);
        if (st == 200 || st == 202) {
            // The scalar is the latch: from now on only poll.
            settings_relay_set_priv(priv);
            LOGI("relay: pairing submitted with code %s; approve it in Tesserae > Settings > Cloud relay", code);
        } else if (st == 404) {
            return RELAY_CODE_DEAD;
        } else if (st == 403) {
            return RELAY_REJECTED;
        } else {
            return RELAY_UNREACHABLE;
        }
        memset(priv, 0, sizeof priv);
    }

    snprintf(url, sizeof url, "%s/v1/pair/%s", settings_relay_url(), code);
    absolute_time_t deadline = make_timeout_time_ms(budget_ms);
    while (true) {
        http_resp_t r;
        int st = http_request("GET", url, NULL, 0, NULL, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
        if (st == 404) { http_resp_free(&r); return RELAY_CODE_DEAD; }
        if (st != 200 || !r.body) { http_resp_free(&r); return st <= 0 ? RELAY_UNREACHABLE : RELAY_WAITING; }
        cJSON *j = cJSON_ParseWithLength(r.body, r.body_len);
        http_resp_free(&r);
        const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "status"));
        if (status && strcmp(status, "ready") == 0) {
            const char *home = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "home_pubkey"));
            const char *inst = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "install_id"));
            const char *dev = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "device_id"));
            const char *tok = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "device_token"));
            uint8_t home_pub[32], key[32];
            bool ok = home && inst && dev && tok && ota_b64url_decode(home, home_pub, sizeof home_pub) == 32
                   && settings_relay_priv() && relay_derive_frame_key(settings_relay_priv(), home_pub, key);
            if (ok) {
                settings_relay_set_paired(inst, dev, tok, key);
                memset(key, 0, sizeof key);
                LOGI("relay: paired as %s (install %s); the frame key was derived here and never travelled", dev, inst);
            } else {
                LOGE("relay: pairing completed but the response was unusable");
            }
            cJSON_Delete(j);
            return ok ? RELAY_PAIRED : RELAY_CODE_DEAD;
        }
        cJSON_Delete(j);
        if (time_reached(deadline)) return RELAY_WAITING;
        sleep_ms(5000);
    }
}

// ---- frames ----

typedef struct { uint32_t got; bool length_ok; } dl_t;

static bool frame_length(size_t len, void *arg)
{
    dl_t *d = arg;
    d->length_ok = len == SEALED_FRAME_BYTES;
    if (!d->length_ok) LOGE("relay: sealed frame is %u bytes, want %u", (unsigned)len, (unsigned)SEALED_FRAME_BYTES);
    return d->length_ok;
}

static void frame_sink(const uint8_t *data, size_t len, void *arg)
{
    dl_t *d = arg;
    if (d->got + len > SEALED_FRAME_BYTES) len = SEALED_FRAME_BYTES - d->got;
    psram_write(RELAY_PSRAM_OFFSET + d->got, data, len);
    d->got += (uint32_t)len;
}

// Open the sealed frame staged in PSRAM into the frame store at 0. The
// plaintext is only declared complete once the tag has authenticated it.
static bool open_frame(void)
{
    const uint8_t *key = settings_relay_key();
    if (!key) return false;
    uint8_t nonce[RELAY_NONCE_BYTES], tag[RELAY_TAG_BYTES];
    psram_read(RELAY_PSRAM_OFFSET, nonce, sizeof nonce);
    psram_read(RELAY_PSRAM_OFFSET + RELAY_NONCE_BYTES + FRAME_BYTES, tag, sizeof tag);
    relay_unseal_t u;
    if (!relay_unseal_begin(&u, key, nonce)) return false;
    static uint8_t in[4096], out[4096 + 16];
    for (uint32_t off = 0; off < FRAME_BYTES; off += sizeof in) {
        uint32_t n = FRAME_BYTES - off < sizeof in ? FRAME_BYTES - off : sizeof in;
        psram_read(RELAY_PSRAM_OFFSET + RELAY_NONCE_BYTES + off, in, n);
        if (!relay_unseal_update(&u, in, n, out)) { relay_unseal_finish(&u, tag); return false; }
        psram_write(off, out, n);
    }
    return relay_unseal_finish(&u, tag);
}

int relay_fetch_frame(const char *etag, bool *opened, char *etag_out, size_t etag_cap)
{
    *opened = false;
    frame_set_complete(false);
    char url[320], auth[180], inm[128];
    url_for(url, sizeof url, "frame");
    bearer(auth, sizeof auth);
    const char *hdrs[2] = { auth, NULL };
    int nh = 1;
    if (etag && etag[0]) { snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", etag); hdrs[1] = inm; nh = 2; }
    dl_t d = {0};
    http_resp_t r;
    int code = http_request("GET", url, hdrs, nh, NULL, &r, frame_length, frame_sink, &d, HTTP_TIMEOUT_DOWNLOAD_MS, DOWNLOAD_STALL_TIMEOUT_MS);
    if (code == 200 && d.got == SEALED_FRAME_BYTES) {
        if (open_frame()) {
            *opened = true;
            frame_set_complete(true);
            snprintf(etag_out, etag_cap, "%s", r.etag);
        } else {
            LOGE("relay: the frame did not authenticate; ignoring it");
        }
    } else if (code == 200) {
        LOGE("relay: incomplete frame (%lu bytes)", (unsigned long)d.got);
    }
    http_resp_free(&r);
    return code;
}

// ---- status and config ----

static void apply_config(const cJSON *cfg, relay_status_t *out)
{
    if (!cJSON_IsObject(cfg)) return;
    const cJSON *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(cfg, "sleep_interval_s"))) out->sleep_interval_s = (int32_t)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(cfg, "button_wake_s"))) out->button_wake_s = (int32_t)v->valuedouble;
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(cfg, "awake_poll_s"))) out->awake_poll_s = (int32_t)v->valuedouble;
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(cfg, "always_on"))) out->always_on = cJSON_IsTrue(v) ? 1 : 0;
}

static void pull_config(const char *remote_etag, relay_status_t *out)
{
    char url[320], auth[180], inm[96];
    url_for(url, sizeof url, "config");
    bearer(auth, sizeof auth);
    const char *hdrs[2] = { auth, NULL };
    int nh = 1;
    if (settings_relay_config_etag()[0]) {
        snprintf(inm, sizeof inm, "If-None-Match: \"%s\"", settings_relay_config_etag());
        hdrs[1] = inm; nh = 2;
    }
    http_resp_t r;
    int code = http_request("GET", url, hdrs, nh, NULL, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    if (code == 200 && r.body && settings_relay_key()) {
        static uint8_t plain[2048 + 16];
        int n = relay_unseal(settings_relay_key(), (const uint8_t *)r.body, r.body_len, plain, sizeof plain - 1);
        if (n >= 0) {
            plain[n] = 0;
            cJSON *cfg = cJSON_ParseWithLength((const char *)plain, (size_t)n);
            apply_config(cfg, out);
            cJSON_Delete(cfg);
            settings_relay_set_config_etag(r.etag[0] ? r.etag : remote_etag);
            LOGI("relay: config applied (etag %s)", settings_relay_config_etag());
        } else {
            LOGE("relay: the config did not authenticate; ignoring it");
        }
    }
    http_resp_free(&r);
}

void relay_post_status(const tess_telemetry_t *t, relay_status_t *out)
{
    memset(out, 0, sizeof *out);
    out->sleep_interval_s = out->button_wake_s = out->awake_poll_s = -1;
    out->always_on = -1;
    char *body = tess_status_body(t, &out->diag_sent);
    char url[320], auth[180];
    url_for(url, sizeof url, "status");
    bearer(auth, sizeof auth);
    const char *hdrs[] = { auth };
    http_resp_t r;
    out->code = http_request("POST", url, hdrs, 1, body, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    free(body);
    char remote[48] = "";
    if (out->code == 200 && r.body) {
        cJSON *j = cJSON_ParseWithLength(r.body, r.body_len);
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "config_etag"));
        if (e) snprintf(remote, sizeof remote, "%s", e);
        cJSON_Delete(j);
    }
    http_resp_free(&r);
    if (remote[0] && strcmp(remote, settings_relay_config_etag()) != 0) pull_config(remote, out);
}
