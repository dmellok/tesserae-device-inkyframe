// Tesserae client for the Pimoroni Inky Frame 7.3".
//
// One wake cycle:
//   wake reason -> Wi-Fi -> (pair if no token) -> GET /frame (ETag) ->
//   download into PSRAM -> POST /status -> radio off -> paint -> persist ->
//   sleep for next_poll_s (RTC alarm + power off on battery; resident on USB).
//
// Setup: a frame with no Wi-Fi or server opens the setup portal (portal.c).
// So does holding Refresh (button C) for 5 s as it wakes; 20 s is a factory
// reset. Until the saved settings have reached the server once, a Wi-Fi or
// server failure reopens the portal with the reason; after that an outage only
// retries, offering the portal once per outage.
//
// The panel is a thin client: every frame arrives pre-dithered and packed in
// the panel's own nibble order, so the bytes stream straight to the glass.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "board.h"
#include "cache.h"
#include "config.h"
#include "diag.h"
#include "epd.h"
#include "frame.h"
#include "http.h"
#include "log.h"
#include "ota.h"
#include "portal.h"
#include "power.h"
#include "psram.h"
#include "relay.h"
#include "rtc.h"
#include "screen.h"
#include "settings.h"
#include "storage.h"
#include "tesserae.h"
#include "wifi.h"

// What is on the glass, kept in the RTC's RAM byte across power-off so status
// pages draw once and a frame is re-fetched without ETag after one was shown.
enum { SCREEN_UNKNOWN = 0, SCREEN_FRAME, SCREEN_PAIRING, SCREEN_NO_DASHBOARD, SCREEN_ERROR };

static const int32_t BACKOFF_S[] = BACKOFF_STEPS_S;
#define BACKOFF_N ((int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]))
static const char *const BUTTON_NAMES[5] = { BUTTON_A_NAME, BUTTON_B_NAME, BUTTON_C_NAME, BUTTON_D_NAME, BUTTON_E_NAME };
static const char *const BUTTON_KEYS[5] = { "a", "b", "c", "d", "e" };

static bool s_on_usb;
static uint8_t s_screen;
static uint32_t s_boot_count;
// Set during a cycle, written to flash after it with the radio down.
static bool s_reached_server;
static bool s_used_pairing_code;

static void set_screen(uint8_t s) { s_screen = s; pcf_ram_set(s); }

static const char *current_kind(void)  { return storage_fallback_kind() ? DEVICE_KIND_FALLBACK : DEVICE_KIND; }

static tess_identity_t identity(void)
{
    return (tess_identity_t){ storage_device_id(), current_kind(), DEVICE_GAMUT, wifi_mac() };
}

// The action a button sends; a button with none goes by its letter.
static const char *button_name(int index)
{
    if (index < 0 || index > 4) return NULL;
    return BUTTON_NAMES[index][0] ? BUTTON_NAMES[index] : BUTTON_KEYS[index];
}

static const char *button_key(int index)
{
    return (index >= 0 && index <= 4) ? BUTTON_KEYS[index] : NULL;
}

// Never returns when the form is saved (the portal reboots). Returns when it
// closed unused, so the caller carries on with its retry.
static void open_portal(const char *reason)
{
    wifi_off();
    LOGI("setup: opening the portal (%s)", reason[0] ? reason : "not configured");
    // The setup page is about to replace whatever is on the glass. Say so
    // before it is painted: saving the form reboots from inside the portal,
    // and the next boot must fetch a full frame rather than trust its ETag.
    set_screen(SCREEN_UNKNOWN);
    portal_run(reason);
}

// Unreachable Wi-Fi / server: escalating backoff, drawing an error page only
// when the outage looks persistent (so a blip never flashes the panel).
static int32_t fail_backoff(const char *what, const char *detail)
{
    uint8_t stage = storage_backoff();
    int32_t sleep_s = BACKOFF_S[stage < BACKOFF_N ? stage : BACKOFF_N - 1];
    if (stage + 1 < BACKOFF_N) storage_set_backoff(stage + 1);
    LOGW("%s: %s (backoff stage %u, sleeping %ld s)", what, detail, stage, (long)sleep_s);
    bool persistent = stage >= 2 || s_boot_count <= 1 || s_screen == SCREEN_UNKNOWN;
    wifi_off();
    if (persistent && s_screen != SCREEN_ERROR) {
        static char server[200];
        snprintf(server, sizeof server, "Server: %s", settings_server_url());
        screen_message(what, detail, "Retrying. Hold Refresh 5 s to change setup.", server);
        set_screen(SCREEN_ERROR);
    }
    return sleep_s;
}

// A failure before these settings have ever worked is a setup mistake: show
// the portal with the reason. A failure after is an outage: retry, offering
// the portal once when the outage has lasted through the backoff ladder.
static int32_t setup_or_backoff(const char *what, const char *detail, const char *portal_reason)
{
    if (!settings_verified()) {
        open_portal(portal_reason);
    } else if (storage_backoff() + 1 >= BACKOFF_N && !storage_portal_offered()) {
        storage_set_portal_offered(true);
        storage_commit();
        open_portal(portal_reason);
    }
    return fail_backoff(what, detail);
}

static void set_clock(uint32_t epoch)
{
    if (epoch < 1600000000u) return;
    uint32_t now = pcf_get_epoch();
    if (!pcf_time_valid() || (now > epoch ? now - epoch : epoch - now) > 5) {
        pcf_set_epoch(epoch);
        LOGI("rtc: clock set to %lu", (unsigned long)epoch);
    }
}

static void apply_pairing(const tess_pair_t *r)
{
    storage_set_token(r->token);
    if (r->device_id[0] && strcmp(r->device_id, storage_device_id()) != 0) {
        LOGI("pair: server assigned device id '%s'", r->device_id);
        storage_set_device_id(r->device_id);
    }
    if (r->sleep_interval_s > 0) storage_set_sleep_s(power_clamp_sleep_s(r->sleep_interval_s));
    storage_set_etag("");
    set_clock(r->server_time);
    s_reached_server = true;
    LOGI("pair: registered as '%s'", storage_device_id());
}

// Announce the catalog kind if the server knows it, else the stock kind.
static void choose_kind(const char *base)
{
    tess_identity_t id = { storage_device_id(), DEVICE_KIND, DEVICE_GAMUT, wifi_mac() };
    tess_kind_probe_t p = tess_probe_kind(base, &id);
    if (p == TESS_KIND_MISSING) {
        if (!storage_fallback_kind())
            LOGW("kind: server does not know '%s'; using '%s' (install server/hardware/pimoroni/*.json)", DEVICE_KIND, DEVICE_KIND_FALLBACK);
        storage_set_fallback_kind(true);
    } else if (p == TESS_KIND_EXISTS) {
        if (storage_fallback_kind()) LOGI("kind: server now knows '%s'", DEVICE_KIND);
        storage_set_fallback_kind(false);
    }
}

static int32_t server_unreachable(void)
{
    static char reason[200];
    snprintf(reason, sizeof reason, "Can't reach the Tesserae server at %s.", settings_server_url());
    return setup_or_backoff("Server unreachable", "Could not reach the Tesserae server.", reason);
}

// No token: announce on /discover (or /register with a pairing code) until
// the admin has clicked Register. Returns 0 when paired, else seconds to
// sleep before trying again.
static int32_t pair(const char *base)
{
    choose_kind(base);
    absolute_time_t budget = make_timeout_time_ms(PAIRING_AWAKE_BUDGET_S * 1000);
    const char *code = settings_pairing_code();
    bool use_code = code[0] != 0;
    tess_identity_t id = identity();

    while (true) {
        tess_pair_t r;
        if (use_code) tess_register(base, &id, code, &r);
        else tess_discover(base, &id, &r);

        if (r.registered) {
            apply_pairing(&r);
            if (use_code) s_used_pairing_code = true;
            storage_set_backoff(0);
            return 0;
        }
        if (r.code <= 0) return server_unreachable();
        // The server answered: the settings work, whatever it said.
        s_reached_server = true;
        if (r.code == 400 || r.code == 401 || r.code == 403) {
            LOGE("pair: rejected (%d): %s", r.code, r.error);
            char l1[120];
            snprintf(l1, sizeof l1, "HTTP %d: %s", r.code, r.error);
            if (s_screen != SCREEN_ERROR) {
                screen_message("Pairing rejected", l1,
                               use_code ? "Check the pairing code: hold Refresh 5 s to change it." : "Check the device id / kind.", "");
                set_screen(SCREEN_ERROR);
            }
            wifi_off();
            return 3600;
        }
        // 200 registered:false (waiting on the admin) or 429.
        if (s_screen != SCREEN_PAIRING) {
            screen_pairing(storage_device_id(), wifi_mac(), settings_server_url(), current_kind(),
                           storage_fallback_kind() ? "Note: catalog kind not installed on the server; using the stock kind." : "");
            set_screen(SCREEN_PAIRING);
        }
        // Clamp: this value comes from the server, and an absurd one would
        // overflow the resident-sleep deadline or alias the RTC alarm's
        // day-of-month field into a date the device never wakes on.
        int wait_s = r.retry_after_s > 0 ? r.retry_after_s : 30;
        if (wait_s < 5) wait_s = 5;
        if (wait_s > 900) wait_s = 900;
        LOGI("pair: waiting for Register in the UI, retry in %d s", wait_s);
        if (time_reached(budget)) {
            wifi_off();
            return wait_s > 60 ? wait_s : 60;
        }
        sleep_ms((uint32_t)wait_s * 1000);
    }
}

// GET /frame and download. Returns the HTTP status; `*downloaded` is set when
// a complete frame is in PSRAM (painted later, with the radio down). `*button`
// (a button index, -1 for none) is cleared once the server has acknowledged it.
static int fetch_frame(const char *base, int *button, uint32_t event_id, bool *downloaded, char *etag_out, size_t etag_cap)
{
    *downloaded = false;
    const char *etag = storage_etag();
    const char *name = button_name(*button);
    // A status page is showing, or a refresh press: always take a full frame.
    if (s_screen != SCREEN_FRAME || (name && strcmp(name, "refresh") == 0)) etag = "";

    tess_frame_t fr;
    tess_get_frame(base, storage_device_id(), storage_token(), etag, name, button_key(*button), event_id, &fr);
    if (fr.code == 200 || fr.code == 304 || fr.code == 204) { *button = -1; s_reached_server = true; }   // acknowledged

    if (fr.code == 200) {
        if ((fr.panel_w && fr.panel_w != PANEL_W) || (fr.panel_h && fr.panel_h != PANEL_H))
            LOGW("frame: server thinks the panel is %dx%d; check the device kind / panel size", fr.panel_w, fr.panel_h);
        int dl = frame_download(fr.url);
        if (dl == 200) {
            *downloaded = true;
            snprintf(etag_out, etag_cap, "%s", fr.etag);
        } else {
            char msg[96];
            snprintf(msg, sizeof msg, "frame download failed: %d, %u bytes", dl, (unsigned)frame_received());
            LOGE("frame: %s; keeping last image", msg);
            tess_post_log(base, storage_device_id(), storage_token(), "error", msg);
        }
    } else if (fr.code == 304) {
        LOGI("frame: unchanged");
    } else if (fr.code == 204) {
        LOGI("frame: nothing rendered for this device yet");
    } else if (fr.code == 401 || fr.code == 403) {
        LOGW("frame: auth failed (%d); dropping token to re-pair", fr.code);
        storage_clear_pairing();
    } else {
        LOGW("frame: request failed (%d); keeping last image", fr.code);
    }
    return fr.code;
}

static void paint_downloaded(const char *etag)
{
    board_led(LED_ACTIVITY, 60);
    if (frame_paint()) {
        storage_set_etag(etag);
        set_screen(SCREEN_FRAME);
    } else {
        LOGE("frame: paint failed; ETag not committed");
        diag_paint_failed(DIAG_PAINT_TIMEOUT);
    }
    board_led(LED_ACTIVITY, 20);
}

static void no_dashboard_page(void)
{
    if (s_screen == SCREEN_NO_DASHBOARD) return;
    char l1[80];
    snprintf(l1, sizeof l1, "Registered as '%s'.", storage_device_id());
    screen_message("Waiting for a dashboard", l1, "Bind a dashboard to this device in Tesserae", "and press Send.");
    set_screen(SCREEN_NO_DASHBOARD);
}

static void telemetry(tess_telemetry_t *t, int button, uint32_t event_id)
{
    memset(t, 0, sizeof *t);
    t->battery_mv = s_on_usb ? 0 : power_vsys_mv();
    t->rssi = wifi_rssi();
    t->ip = wifi_ip();
    t->mac = wifi_mac();
    t->button = button_name(button);
    t->button_key = button_key(button);
    t->button_event_id = event_id;
    t->can_stay_awake = s_on_usb;
}

// Wait for `b` to be released (at most 2 s).
static void await_release(int b)
{
    absolute_time_t t = make_timeout_time_ms(2000);
    while (board_button_down(b) && !time_reached(t)) sleep_ms(20);
}

// Stay awake briefly after a button wake to catch follow-up presses.
static void button_linger(const char *base, int32_t window_s)
{
    absolute_time_t deadline = make_timeout_time_ms((uint32_t)window_s * 1000);
    LOGI("button: lingering %ld s for more presses", (long)window_s);
    while (!time_reached(deadline)) {
        int b = board_any_button_down();
        if (b < 0) { sleep_ms(30); continue; }
        await_release(b);
        board_led_button(b, 60);
        bool downloaded = false; char etag[80] = "";
        int button = b;
        int code = fetch_frame(base, &button, storage_next_event_id(), &downloaded, etag, sizeof etag);
        if (downloaded) paint_downloaded(etag);
        else if (code == 204) no_dashboard_page();
        board_led_button(b, 0);
        if (code == 401 || code == 403) return;   // token gone; re-pair next wake
        deadline = make_timeout_time_ms((uint32_t)window_s * 1000);
    }
}

// Mains-powered panels: keep Wi-Fi up and poll instead of sleeping. Returns
// when the server turns always_on off or the token dies.
static void always_on_loop(const char *base)
{
    LOGI("always-on: polling every %ld s", (long)storage_awake_poll_s());
    int32_t next_poll = storage_awake_poll_s();
    uint32_t since_status = 0;
    while (true) {
        absolute_time_t deadline = make_timeout_time_ms((uint32_t)next_poll * 1000);
        int pressed = -1;
        while (!time_reached(deadline)) {
            int b = board_any_button_down();
            if (b >= 0) { pressed = b; while (board_button_down(b)) sleep_ms(20); break; }
            sleep_ms(50);
        }
        if (!wifi_connected() && !wifi_connect()) continue;
        int button = pressed;
        uint32_t event_id = button >= 0 ? storage_next_event_id() : 0;
        bool downloaded = false; char etag[80] = "";
        int code = fetch_frame(base, &button, event_id, &downloaded, etag, sizeof etag);
        if (downloaded) paint_downloaded(etag);
        else if (code == 204) no_dashboard_page();
        if (code == 401 || code == 403) return;
        since_status += (uint32_t)next_poll;
        if (since_status >= 60 || pressed >= 0) {
            tess_telemetry_t t; telemetry(&t, button, event_id);
            tess_status_t s;
            tess_post_status(base, storage_device_id(), storage_token(), &t, &s);
            if (s.code == 200) {
                set_clock(s.server_time);
                diag_acknowledge(s.diag_sent);
                if (s.logs_upload) diag_upload_log(base, storage_device_id(), storage_token());
                if (s.always_on == 0) { LOGI("always-on: turned off by config"); storage_set_always_on(false); return; }
                if (s.awake_poll_s > 0) storage_set_awake_poll_s(s.awake_poll_s);
                next_poll = (s.next_poll_s > 0 && s.next_poll_s < 300) ? s.next_poll_s : storage_awake_poll_s();
                if (next_poll < 5) next_poll = 5;
            }
            since_status = 0;
            storage_commit();   // always-on can run for weeks; do not lose the ETag
        }
    }
}

#ifdef OTA_BENCH_DESCRIPTOR_URL
// Bench builds only: also take a descriptor from a test web server, so the
// install, trial boot, confirm and rollback can be exercised without staging
// on a Tesserae server. Pair with OTA_TRUST_TEST_KEY and the test signing key.
#include "http.h"
#include "vendor/cJSON.h"
static void bench_offer(uint32_t vsys)
{
    http_resp_t r;
    int code = http_request("GET", OTA_BENCH_DESCRIPTOR_URL, NULL, 0, NULL, &r, NULL, NULL, NULL, HTTP_TIMEOUT_STATUS_MS, 0);
    if (code == 200 && r.body) {
        cJSON *d = cJSON_ParseWithLength(r.body, r.body_len);
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(d, "payload");
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(d, "signature");
        if (cJSON_IsString(p) && cJSON_IsString(g)) {
            LOGI("bench: descriptor from " OTA_BENCH_DESCRIPTOR_URL);
            ota_offer(p->valuestring, g->valuestring, current_kind(), s_on_usb, vsys);
        }
        cJSON_Delete(d);
    }
    http_resp_free(&r);
}
#endif

#ifdef SELFTEST_BASE_URL
// Bench self-test: no Tesserae involved. Download and paint the synthetic
// frame from tools/make_test_frames.py.
static int32_t selftest(void)
{
    if (!wifi_connect()) return fail_backoff("Wi-Fi unavailable", "self-test");
    s_on_usb = wifi_on_usb();
    int dl = frame_download(SELFTEST_BASE_URL "/test.bin");
    LOGI("selftest: test.bin -> http %d, %u bytes", dl, (unsigned)frame_received());
    wifi_off();
    if (dl == 200) frame_paint();
    return 3600;
}
#endif

#ifdef TLS_SELFTEST
#include "sha256.h"
static sha256_ctx_t s_st_sha;
static uint32_t s_st_got;
static void selftest_sink(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    sha256_update(&s_st_sha, data, len);
    s_st_got += (uint32_t)len;
}
#endif

// ---- the cloud relay (a frame that cannot reach the server) ----

static void relay_apply(const relay_status_t *rs)
{
    if (rs->sleep_interval_s > 0) storage_set_sleep_s(power_clamp_sleep_s(rs->sleep_interval_s));
    if (rs->button_wake_s >= 0) storage_set_button_wake_s(rs->button_wake_s);
}

// The relay revoked this pairing (401): forget it and ask for a new code.
static int32_t relay_revoked(void)
{
    LOGW("relay: 401, the pairing was revoked");
    settings_relay_drop(false);
    settings_commit();
    open_portal("This frame's cloud relay pairing was removed. Enter a new pairing code from Tesserae > Settings > Cloud relay.");
    return 60;
}

static int32_t cycle_relay(int wake_button)
{
    if (!settings_relay_paired()) {
        if (s_screen != SCREEN_PAIRING) {
            static char l2[64];
            snprintf(l2, sizeof l2, "Pairing code %s", settings_relay_code());
            screen_message("Pairing with the cloud relay", "Approve this frame in Tesserae:", "Settings > Cloud relay", l2);
            set_screen(SCREEN_PAIRING);
        }
        relay_pair_result_t pr = relay_pair(PAIRING_AWAKE_BUDGET_S * 1000u);
        settings_commit();   // the pairing latch must survive a power-off
        if (pr == RELAY_CODE_DEAD) {
            settings_relay_drop(false);
            settings_commit();
            open_portal("The cloud relay pairing code expired or is unknown. Make a new one in Tesserae > Settings > Cloud relay.");
            return 60;
        }
        if (pr == RELAY_REJECTED) return fail_backoff("Relay refused pairing", "The cloud relay rejected this frame.");
        if (pr == RELAY_UNREACHABLE) return setup_or_backoff("Relay unreachable", "Could not reach the cloud relay.",
                                                              "Can't reach the cloud relay. Check the relay URL.");
        if (pr == RELAY_WAITING) { wifi_off(); return 60; }
        s_reached_server = true;
    }

    // 1. Frame.
    bool opened = false; char etag[80] = "";
    const char *have = s_screen == SCREEN_FRAME ? storage_etag() : "";
    int code = relay_fetch_frame(have, &opened, etag, sizeof etag);
    if (code == 401) return relay_revoked();
    if (code <= 0) return setup_or_backoff("Relay unreachable", "Could not reach the cloud relay.",
                                          "Can't reach the cloud relay. Check the relay URL.");
    s_reached_server = true;

    // 2. Status, carrying any button press: the relay forwards it home. The
    //    press is repeated on every post of this wake (the slot is latest-only).
    int button = wake_button;
    uint32_t event_id = button >= 0 ? storage_next_event_id() : 0;
    tess_telemetry_t t; telemetry(&t, button, event_id);
    relay_status_t rs;
    relay_post_status(&t, &rs);
    if (rs.code == 401) return relay_revoked();
    if (rs.code == 200) { relay_apply(&rs); diag_acknowledge(rs.diag_sent); }
    storage_set_backoff(0);

    // 3. Paint. After a press, keep polling the mailbox through the awake
    //    window for the frame home renders in response.
    bool linger = button >= 0 && storage_button_wake_s() > 0;
    if (!linger) wifi_off();
    if (opened) paint_downloaded(etag);
    else if (code == 204) no_dashboard_page();
    if (linger) {
        absolute_time_t deadline = make_timeout_time_ms((uint32_t)storage_button_wake_s() * 1000u);
        while (!time_reached(deadline)) {
            sleep_ms(5000);
            relay_post_status(&t, &rs);
            code = relay_fetch_frame(storage_etag(), &opened, etag, sizeof etag);
            if (code == 401) return relay_revoked();
            if (opened) { paint_downloaded(etag); break; }
        }
        wifi_off();
    }
    return power_clamp_sleep_s(storage_sleep_s());
}

// One wake cycle. Returns the number of seconds to sleep.
static int32_t cycle(int wake_button)
{
#ifdef SELFTEST_BASE_URL
    return selftest();
#endif
    // The card first: a deck page for this press, or a due album frame,
    // paints with no network at all. A press served here is not reported
    // as a press (the server never saw it); the page goes up on the check-in.
    if (wake_button >= 0 && cache_local_nav(button_name(wake_button))) {
        set_screen(SCREEN_FRAME);
        wake_button = -1;
    } else if (wake_button < 0 && pcf_time_valid() && cache_album_due(pcf_get_epoch())) {
        set_screen(SCREEN_FRAME);
    }
    if (!wifi_connect()) {
        // An update on trial that cannot join Wi-Fi goes back to the previous
        // image rather than risk a frame that can never be updated again.
        if (ota_on_trial()) ota_abandon_trial("Wi-Fi did not connect");
        static char reason[120], detail[96];
        snprintf(detail, sizeof detail, "Could not join '%s'.", settings_ssid());
        snprintf(reason, sizeof reason, "Wi-Fi didn't connect to '%s'. Check the network name and password.", settings_ssid());
        return setup_or_backoff("Wi-Fi unavailable", detail, reason);
    }
    storage_set_portal_offered(false);   // the network is back
#ifdef TLS_SELFTEST
    {
        // Bench: prove the TLS client against both CA families once per boot.
        {
            static const char *const urls[] = {
                "https://tesserae-ota-signer.dmellok.workers.dev/",   // Google Trust Services
                "https://api.tesserae.ink/version/latest",            // Let's Encrypt
                "https://self-signed.badssl.com/",                     // must fail: self-signed
                "https://wrong.host.badssl.com/",                      // must fail: name mismatch
            };
            for (int i = 0; i < 4; i++) {
                http_resp_t r;
                int code = http_request("GET", urls[i], NULL, 0, NULL, &r, NULL, NULL, NULL, 20000, 0);
                LOGI("tls selftest: %s -> %d (%u bytes)", urls[i], code, (unsigned)r.body_len);
                http_resp_free(&r);
            }
            // A large HTTPS download (the 0.3.1 OTA image), streamed and hashed.
            sha256_init(&s_st_sha); s_st_got = 0;
            http_resp_t r;
            int code = http_request("GET", "https://tesserae-ota-signer.dmellok.workers.dev/firmware/pimoroni_inky_frame_73/0.3.1/" TLS_SELFTEST_SHA ".bin",
                                    NULL, 0, NULL, &r, NULL, selftest_sink, NULL, 120000, 10000);
            uint8_t d[32]; char hex[65];
            sha256_final(&s_st_sha, d); sha256_hex(d, hex);
            LOGI("tls selftest: big download -> %d, %lu bytes, sha256 %s", code, (unsigned long)s_st_got,
                 strcmp(hex, TLS_SELFTEST_SHA) == 0 ? "MATCHES" : "differs");
            http_resp_free(&r);
        }
    }
#endif
    if (ota_on_trial()) {
        // The update joined Wi-Fi: keep it. Buying drops the radio briefly.
        ota_confirm();
        if (!wifi_connect()) return fail_backoff("Wi-Fi unavailable", "Rejoin after the update failed.");
    }
    board_led(LED_CONNECTION, 20);
    s_on_usb = wifi_on_usb();
    if (settings_relay_mode()) return cycle_relay(wake_button);
    uint32_t vsys = power_sample_vsys_mv();
    LOGI("power: %s, vsys %lu mV", s_on_usb ? "USB" : "battery", (unsigned long)vsys);

    const char *base = wifi_server_base();
    if (!base[0]) return server_unreachable();
    LOGI("server base: %s", base);

    if (!storage_token()[0]) {
        int32_t s = pair(base);
        if (s) return s;
    } else if (storage_fallback_kind()) {
        // Registered under the stock kind: see whether the catalog entry has
        // been installed since, and let the MAC-match discover heal the kind.
        choose_kind(base);
        if (!storage_fallback_kind()) {
            tess_pair_t r; tess_identity_t id = identity();
            tess_discover(base, &id, &r);
            if (r.registered) apply_pairing(&r);
            else storage_set_fallback_kind(true);
        }
    }

    // 1. Frame.
    int button = wake_button;
    uint32_t event_id = button >= 0 ? storage_next_event_id() : 0;
    bool downloaded = false; char etag[80] = "";
    int frame_code = fetch_frame(base, &button, event_id, &downloaded, etag, sizeof etag);
    if (frame_code <= 0) return server_unreachable();
    if (!storage_token()[0]) { wifi_off(); return 30; }   // re-pair next wake

    // 2. Heartbeat: drives this cycle's sleep via next_poll_s and carries any
    //    button press the frame request could not deliver.
    tess_telemetry_t t; telemetry(&t, button, event_id);
    tess_status_t s;
    tess_post_status(base, storage_device_id(), storage_token(), &t, &s);
    if (s.code == 401 || s.code == 403) { storage_clear_pairing(); wifi_off(); return 30; }
    if (s.code == 200) {
        set_clock(s.server_time);
        if (s.sleep_interval_s > 0) storage_set_sleep_s(power_clamp_sleep_s(s.sleep_interval_s));
        if (s.button_wake_s >= 0) storage_set_button_wake_s(s.button_wake_s);
        if (s.always_on >= 0) storage_set_always_on(s.always_on == 1);
        if (s.awake_poll_s > 0) storage_set_awake_poll_s(s.awake_poll_s);
        diag_acknowledge(s.diag_sent);
        if (s.logs_upload) diag_upload_log(base, storage_device_id(), storage_token());
        cache_note_status(s.deck_version, s.has_deck, s.album_version, s.has_album, base, storage_device_id(), storage_token());
        // A signed update: on success this installs and reboots into it.
        if (s.ota_payload) ota_offer(s.ota_payload, s.ota_signature, current_kind(), s_on_usb, vsys);
#ifdef OTA_BENCH_DESCRIPTOR_URL
        else bench_offer(vsys);
#endif
    }
    storage_set_backoff(0);

    // 3. Paint (radio stays up only if we may need it again right away).
    bool linger = wake_button >= 0 && storage_button_wake_s() > 0;
    bool always_on = s_on_usb && storage_always_on();
    if (!linger && !always_on) wifi_off();
    if (downloaded) {
        paint_downloaded(etag);
        if (pcf_time_valid()) cache_note_network_frame(pcf_get_epoch());
    } else if (frame_code == 204) no_dashboard_page();

    if (linger) button_linger(base, storage_button_wake_s());
    if (always_on) always_on_loop(base);
    wifi_off();

    // 4. Sleep. next_poll_s wins; wake_at (synchronised wake) is converted to
    //    a delta at the moment we actually go down.
    int32_t sleep_s = s.next_poll_s > 0 ? power_clamp_sleep_s(s.next_poll_s) : power_clamp_sleep_s(storage_sleep_s());
    if (s.wake_at && pcf_time_valid()) {
        uint32_t now = pcf_get_epoch();
        if (now > 1600000000u && s.wake_at > now + 5) sleep_s = power_clamp_sleep_s((int32_t)(s.wake_at - now));
    }
    // Wake for the album's next frame if it comes first.
    int32_t album = pcf_time_valid() ? cache_album_wait_s(pcf_get_epoch()) : -1;
    if (album >= 0 && album < sleep_s) sleep_s = album < SLEEP_MIN_S ? SLEEP_MIN_S : album;
    return sleep_s;
}

// Write what the cycle learnt about the settings (radio is down by now).
static void settle_settings(void)
{
    settings_commit();   // relay pairing state, if it changed
    cache_commit();      // deck page and album position, if they changed
    if (s_reached_server) settings_set_verified();
    if (s_used_pairing_code) settings_clear_pairing_code();
    s_reached_server = s_used_pairing_code = false;
}

// Refresh held as the frame wakes: 5 s opens setup, 20 s factory-resets.
// Returns true if the press was a hold (so it is not also sent as a refresh).
static bool handle_setup_hold(void)
{
    const int b = BUTTON_SETUP_INDEX;
    if (!board_button_down(b)) return false;
    absolute_time_t start = get_absolute_time();
    bool setup_lit = false;
    while (board_button_down(b)) {
        int64_t held = absolute_time_diff_us(start, get_absolute_time()) / 1000;
        if (held >= HOLD_RESET_MS) {
            LOGW("setup: Refresh held %d s: factory reset", HOLD_RESET_MS / 1000);
            for (int i = 0; i < 5; i++) board_led_button(i, 100);
            settings_erase();
            storage_erase();
            set_screen(SCREEN_UNKNOWN);
            screen_message("Factory reset", "Wi-Fi, server and pairing are cleared.", "Restarting into setup.", "");
            watchdog_reboot(0, 0, 0);
            while (true) tight_loop_contents();
        }
        if (held >= HOLD_SETUP_MS && !setup_lit) {
            setup_lit = true;
            board_led_button(b, 100);   // let go now for setup; keep holding to reset
            LOGI("setup: Refresh held %d s", HOLD_SETUP_MS / 1000);
        }
        sleep_ms(20);
    }
    if (!setup_lit) return false;
    open_portal("Setup opened from the Refresh button.");
    return true;
}

int main(void)
{
    board_init();
    board_led(LED_ACTIVITY, 20);
    stdio_init_all();
#ifdef DEV_BOOT_LOG_DELAY_MS
    sleep_ms(DEV_BOOT_LOG_DELAY_MS);   // bench: let a serial monitor attach
#endif
    wake_event_t wake = board_wake_event();

    LOGI("tesserae-inkyframe v%s, kind %s, panel %dx%d", FW_VERSION, DEVICE_KIND, PANEL_W, PANEL_H);
    LOGI("wake: %s", board_wake_name(wake));
    storage_begin();
    settings_begin();
    ota_boot();
    pcf_init();
    diag_begin(board_wake_name(wake));
    s_screen = pcf_ram_get() & 0x0F;
    if (s_screen > SCREEN_ERROR) s_screen = SCREEN_UNKNOWN;
    epd_init();
    if (!psram_init() || !psram_test()) LOGE("psram: FAILED; frames cannot be stored");
    cache_begin();
    LOGI("device id '%s', kind '%s', screen state %u, rtc %s (%lu)", storage_device_id(), current_kind(), s_screen,
         pcf_time_valid() ? "valid" : "unset", (unsigned long)pcf_get_epoch());

    int wake_button = -1;
    if (wake >= WAKE_BUTTON_A && wake <= WAKE_BUTTON_E) {
        wake_button = wake - WAKE_BUTTON_A;
        board_led_button(wake_button, 40);
    }
    if (handle_setup_hold()) wake_button = -1;

    while (!settings_configured()) {
        open_portal("");
        // Closed unused on battery: power off until a button wakes the frame.
        board_release_power();
        sleep_ms(1000);
    }

    while (true) {
        s_boot_count++;
        int32_t sleep_s = cycle(wake_button);
        settle_settings();
        storage_commit();
        int pressed = -1;
        power_sleep((uint32_t)sleep_s, s_on_usb, &pressed);
        // Only reached on USB power: run the next cycle in place.
        for (int i = 0; i < 5; i++) board_led_button(i, 0);
        if (pressed >= 0) board_led_button(pressed, 40);
        if (pressed == BUTTON_SETUP_INDEX && handle_setup_hold()) pressed = -1;
        wake_button = pressed;
        board_led(LED_ACTIVITY, 20);
        LOGI("wake: %s (USB resident)", pressed >= 0 ? "button" : "timer");
    }
}
