/*
 * portal.c: the setup portal. See portal.h.
 *
 * Ported from tesserae-device-pico-bin's net_portal.c: an access point, a DHCP
 * server and a wildcard DNS hijack (src/vendor, from the MicroPython and
 * pico-examples projects), and a small raw-TCP HTTP server on port 80 serving
 * the Tesserae ESP32 client's setup form, so every Tesserae device looks the
 * same during setup. The MQTT card is gone: this firmware speaks REST only.
 */
#include "portal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

#include "board.h"
#include "config.h"
#include "log.h"
#include "power.h"
#include "screen.h"
#include "settings.h"
#include "storage.h"
#include "vendor/dhcpserver.h"
#include "vendor/dnsserver.h"

/* ---------- page markup (verbatim from tesserae-device-esp32-bin) ---------- */

/* Page head in the Tesserae "Paper" theme (tesserae.ink's design tokens):
 * paper ground, cream sheets with hairline edges, ink text, red-ink kickers,
 * 2 px corners, Archivo where the phone has it. The mark is the Tesserae server
 * logo: ink tile, red square top right, paper square bottom left. */
static const char k_head[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<title>Tesserae setup</title>"
"<style>"
":root{--paper:#eeeae1;--sheet:#fffdf8;--ink:#1c1b19;--ink-2:#55524b;--ink-3:#7a766c;"
"--red:#b8352a;--red-hover:#9e2c22;--hairline:#e0dacd;--hairline-strong:#cfc8b8;"
"--focus:#2b4e9b;--sunk:#f1ece1;--radius:2px;"
"--ui:'Archivo','Helvetica Neue',Arial,sans-serif;"
"--mono:'Martian Mono',ui-monospace,Menlo,monospace}"
"*{box-sizing:border-box}"
"body{margin:0;padding:28px 16px env(safe-area-inset-bottom);font-family:var(--ui);"
"font-size:15px;background:var(--paper);color:var(--ink);line-height:1.45;"
"-webkit-text-size-adjust:100%;-webkit-font-smoothing:antialiased}"
"main{max-width:480px;margin:0 auto}"
".brand{display:flex;align-items:center;gap:10px;margin:0 0 4px;"
"font-weight:700;font-size:21px;letter-spacing:-0.01em}"
".brand-mark{flex:none;display:block;width:32px;height:32px}"
".tag{font-family:var(--mono);font-size:11px;letter-spacing:0.08em;text-transform:uppercase;"
"color:var(--red);margin:0 0 20px}"
".card{background:var(--sheet);border:1px solid var(--hairline);"
"border-radius:var(--radius);padding:18px 16px;margin-bottom:14px}"
".card h2{margin:0 0 14px;font-family:var(--mono);font-size:11px;font-weight:400;"
"text-transform:uppercase;letter-spacing:0.08em;color:var(--red)}"
".field{margin-bottom:14px}"
".field:last-child{margin-bottom:0}"
"label{display:block;font-weight:600;font-size:13px;margin-bottom:6px;color:var(--ink)}"
"input,select{width:100%;height:40px;padding:0 12px;border:1px solid var(--hairline-strong);"
"border-radius:var(--radius);background:var(--sheet);font:inherit;font-size:15px;"
"color:var(--ink);-webkit-appearance:none;appearance:none}"
"input:focus,select:focus{outline:2px solid var(--focus);outline-offset:1px;border-color:var(--ink)}"
".pw{position:relative}"
".pw input{padding-right:64px}"
".pw button{position:absolute;right:4px;top:29px;height:32px;"
"background:none;border:0;color:var(--red);font:inherit;font-size:13px;"
"font-weight:600;padding:0 10px;cursor:pointer;border-radius:var(--radius)}"
".pw button:hover{background:var(--sunk)}"
".hint{margin-top:6px;font-size:12px;color:var(--ink-3)}"
".rescan{display:inline-block;margin-top:10px;font-size:13px;font-weight:600;"
"color:var(--red);text-decoration:none}"
".rescan:hover{text-decoration:underline}"
".hint code{font-family:var(--mono);background:var(--sunk);padding:1px 5px;"
"border-radius:var(--radius);font-size:11px}"
"button.submit{width:100%;height:44px;border:0;border-radius:var(--radius);"
"background:var(--red);color:var(--sheet);font:inherit;font-size:15px;"
"font-weight:600;cursor:pointer;margin-top:4px}"
"button.submit:hover{background:var(--red-hover)}"
"button.submit:active{transform:translateY(1px)}"
"</style></head><body><main>"
"<div class=\"brand\">"
"<svg class=\"brand-mark\" viewBox=\"0 0 256 256\" xmlns=\"http://www.w3.org/2000/svg\" aria-hidden=\"true\">"
"<rect width=\"256\" height=\"256\" rx=\"72\" fill=\"#1C1B19\"/>"
"<path d=\"M128 55H174A27 27 0 0 1 201 82V128H128Z\" fill=\"#D0493C\"/>"
"<path d=\"M55 128H128V201H82A27 27 0 0 1 55 174Z\" fill=\"#FFFDF8\"/>"
"</svg>"
"<span>Tesserae</span></div>"
"<p class=\"tag\">Inky Frame setup</p>";

/* WiFi card; %s x2 = (ssid prefill, scan-picker HTML or "") */
static const char k_form_wifi_fmt[] =
"<form method=\"POST\" action=\"/save\">"
"<section class=\"card\"><h2>WiFi network</h2>"
"<div class=\"field\">"
"<label for=\"ssid\">Network name (SSID) *</label>"
"<input id=\"ssid\" name=\"ssid\" required maxlength=\"32\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"my-home-wifi\">"
"</div>"
"%s"
"<div class=\"field pw\">"
"<label for=\"wifi-pw\">Password</label>"
"<input id=\"wifi-pw\" name=\"pass\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"wifi-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</section>";

/* Tesserae server card; %s = server_url prefill. */
static const char k_form_server_fmt[] =
"<section class=\"card\"><h2>Tesserae server</h2>"
"<div class=\"field\">"
"<label for=\"server_url\">Server URL</label>"
"<input id=\"server_url\" name=\"server_url\" maxlength=\"159\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"http://tesserae.local:8765\">"
"</div>"
"<div class=\"field\">"
"<label for=\"pairing_code\">Pairing code (optional)</label>"
"<input id=\"pairing_code\" name=\"pairing_code\" maxlength=\"15\" "
"autocomplete=\"off\" inputmode=\"numeric\" placeholder=\"leave blank for auto-register\">"
"<p class=\"hint\">Leave blank: approve the device in Tesserae &rarr; Settings &rarr; "
"Devices (Discovered). Enter a code only for strict admin-gated pairing.</p>"
"</div>"
"</section>";

/* Cloud relay card; %s = relay URL prefill. For a frame away from home. */
static const char k_form_relay_fmt[] =
"<section class=\"card\"><h2>Or: cloud relay</h2>"
"<p class=\"hint\">For a frame that can't reach your server, such as one at another house. "
"Make a code in Tesserae &rarr; Settings &rarr; Cloud relay; then the server URL above is not needed.</p>"
"<div class=\"field\">"
"<label for=\"relay_code\">Relay pairing code</label>"
"<input id=\"relay_code\" name=\"relay_code\" maxlength=\"15\" autocomplete=\"off\" placeholder=\"leave blank to use the server\">"
"</div>"
"<div class=\"field\">"
"<label for=\"relay_url\">Relay URL</label>"
"<input id=\"relay_url\" name=\"relay_url\" maxlength=\"95\" autocomplete=\"off\" value=\"%s\">"
"<p class=\"hint\">Leave as is for Tesserae's hosted relay.</p>"
"</div>"
"</section>";

/* Why setup is showing, above the form; %s = the reason. */
static const char k_reason_fmt[] =
"<section class=\"card\"><p class=\"hint\" style=\"margin:0;color:#b42318\">%s</p></section>";

static const char k_submit[] =
"<button class=\"submit\" type=\"submit\">Save &amp; restart</button>"
"</form>";

static const char k_tail[] =
"<script>"
"document.querySelectorAll('[data-toggle]').forEach(b=>{"
"b.addEventListener('click',()=>{"
"const i=document.getElementById(b.dataset.toggle);"
"const s=i.type==='password';"
"i.type=s?'text':'password';"
"b.textContent=s?'Hide':'Show';"
"});"
"});"
"const pick=document.getElementById('ssid-pick');"
"if(pick){pick.addEventListener('change',e=>{"
"if(e.target.value){document.getElementById('ssid').value=e.target.value;"
"document.getElementById('wifi-pw').focus();}"
"});}"
"</script></main></body></html>";

static const char k_thanks_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Saved</title><style>"
"body{margin:0;padding:40px 16px;background:#eeeae1;color:#18181b;"
"font-family:'Archivo','Helvetica Neue',Arial,sans-serif;text-align:center}"
".card{max-width:380px;margin:0 auto;background:#fffdf8;border:1px solid #e0dacd;"
"border-radius:2px;padding:28px 20px}"
"h1{margin:0 0 8px;font-size:20px;color:#b8352a}"
"p{margin:0;color:#7a766c;font-size:14px;line-height:1.5}"
"</style></head><body>"
"<div class=\"card\"><h1>Saved</h1>"
"<p>The frame restarts and connects with the new settings. Its panel updates within a minute.</p></div>"
"</body></html>";

/* Shown while a rescan runs; auto-reloads the form once it completes. */
static const char k_scanning_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<meta http-equiv=\"refresh\" content=\"9;url=/\"><title>Scanning</title><style>"
"body{margin:0;padding:40px 16px;background:#eeeae1;color:#18181b;"
"font-family:'Archivo','Helvetica Neue',Arial,sans-serif;text-align:center}"
".card{max-width:380px;margin:0 auto;background:#fffdf8;border:1px solid #e0dacd;"
"border-radius:2px;padding:28px 20px}"
"h1{margin:0 0 8px;font-size:20px;color:#b8352a}"
"p{margin:0;color:#7a766c;font-size:14px;line-height:1.5}"
"</style></head><body>"
"<div class=\"card\"><h1>Scanning&hellip;</h1>"
"<p>Looking for nearby networks. This page reloads automatically.</p></div>"
"</body></html>";

/* ---------- helpers (from the esp32 portal) ---------- */

static void html_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        const char *rep;
        switch (*p) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '"': rep = "&quot;"; break;
            default:
                if (o + 1 >= dst_sz) { dst[o] = '\0'; return; }
                dst[o++] = *p;
                continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= dst_sz) break;
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
}

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

/* Pull a named field out of an x-www-form-urlencoded body into dst (decoded).
 * Returns true if the key was present. */
static bool form_field(const char *body, const char *key, char *dst, size_t dst_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= dst_sz) n = dst_sz - 1;
            memcpy(dst, v, n);
            dst[n] = '\0';
            url_decode(dst);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* ---------- WiFi scan (populate the form's network picker) ---------- */

#define SCAN_MAX 16
static char s_scan[SCAN_MAX][33];
static int  s_scan_count;

static int scan_cb(void *env, const cyw43_ev_scan_result_t *r)
{
    (void)env;
    if (!r || r->ssid_len == 0 || r->ssid_len > 32) return 0;
    if (s_scan_count >= SCAN_MAX) return 0;
    char ssid[33];
    memcpy(ssid, r->ssid, r->ssid_len);
    ssid[r->ssid_len] = '\0';
    for (int i = 0; i < s_scan_count; i++)
        if (strcmp(s_scan[i], ssid) == 0) return 0;   /* dedup */
    memcpy(s_scan[s_scan_count++], ssid, (size_t)r->ssid_len + 1);
    return 0;
}

/* Scan for nearby APs and (re)fill the picker cache. Runs in whatever mode is
 * active (we scan while the AP is up so a rescan never drops the client). Must
 * be called from the main loop, not an lwIP callback (it blocks ~5s). */
static void wifi_scan(void)
{
    s_scan_count = 0;
    cyw43_wifi_scan_options_t opts;
    memset(&opts, 0, sizeof opts);
    cyw43_arch_lwip_begin();
    int err = cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_cb);
    cyw43_arch_lwip_end();
    if (err != 0) { LOGW("portal: scan start failed (%d)", err); return; }
    uint32_t t = 0;
    while (cyw43_wifi_scan_active(&cyw43_state) && t < 8000) { sleep_ms(100); t += 100; }
    LOGI("portal: scan found %d networks", s_scan_count);
}

/* Render the network picker (a <select> of scanned SSIDs when any) plus a
 * Rescan link, into dst. Always emits the field so the user can rescan. */
static void build_picker(char *dst, size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(dst + n, cap - n, "<div class=\"field\">");
    if (s_scan_count > 0) {
        n += (size_t)snprintf(dst + n, cap - n,
            "<label for=\"ssid-pick\">Or pick a nearby network</label>"
            "<select id=\"ssid-pick\"><option value=\"\">Choose a network&hellip;</option>");
        for (int i = 0; i < s_scan_count && n < cap - 96; i++) {
            char esc[100];
            html_escape(s_scan[i], esc, sizeof esc);
            n += (size_t)snprintf(dst + n, cap - n, "<option>%s</option>", esc);
        }
        n += (size_t)snprintf(dst + n, cap - n, "</select>");
    }
    n += (size_t)snprintf(dst + n, cap - n,
        "<a class=\"rescan\" href=\"/rescan\">&#x21bb; %s</a></div>",
        s_scan_count > 0 ? "Rescan networks" : "Scan for networks");
}

/* ---------- HTTP server (raw TCP) ---------- */

#define REQ_MAX   1280
#define PAGE_MAX  12288

static char           s_resp[PAGE_MAX];   /* one connection serviced at a time */
static volatile bool  s_reboot_pending;
static volatile bool  s_rescan_pending;   /* set by GET /rescan; run in main loop */
static volatile bool  s_client_seen;      /* set on each connection; resets idle timer */

typedef struct {
    char     req[REQ_MAX];
    size_t   req_len;
    char    *resp;          /* this connection's copy of the response */
    size_t   resp_total;
    size_t   queued;        /* bytes handed to lwIP so far */
    size_t   acked;
    bool     reboot_after;
} conn_t;

static void conn_close(struct tcp_pcb *pcb, conn_t *c)
{
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    if (c) { free(c->resp); free(c); }
    tcp_close(pcb);
}

static char s_reason[96];

/* Build the setup form into s_resp (HTTP response incl. headers). */
static size_t build_form(void)
{
    char ssid[80], server[200], reason[160], relay[160];
    html_escape(settings_ssid(), ssid, sizeof ssid);
    html_escape(settings_server_url(), server, sizeof server);
    html_escape(s_reason, reason, sizeof reason);
    html_escape(settings_relay_url(), relay, sizeof relay);

    static char picker[640];
    build_picker(picker, sizeof picker);

    static char body[PAGE_MAX - 256];
    int n = 0;
    n += snprintf(body + n, sizeof body - n, "%s", k_head);
    if (reason[0]) n += snprintf(body + n, sizeof body - n, k_reason_fmt, reason);
    n += snprintf(body + n, sizeof body - n, k_form_wifi_fmt, ssid, picker);
    n += snprintf(body + n, sizeof body - n, k_form_server_fmt, server);
    n += snprintf(body + n, sizeof body - n, k_form_relay_fmt, relay);
    n += snprintf(body + n, sizeof body - n, "%s", k_submit);
    n += snprintf(body + n, sizeof body - n, "%s", k_tail);

    int h = snprintf(s_resp, sizeof s_resp,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    if (h + n >= (int)sizeof s_resp) n = (int)sizeof s_resp - h - 1;
    memcpy(s_resp + h, body, n);
    return (size_t)(h + n);
}

static size_t build_static(const char *html)
{
    int n = (int)strlen(html);
    int h = snprintf(s_resp, sizeof s_resp,
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
    memcpy(s_resp + h, html, n);
    return (size_t)(h + n);
}

/* The form is saved from the main loop, not the lwIP callback: settings_save
 * writes flash with interrupts off, which must not happen inside lwIP. */
static char s_pending_form[REQ_MAX];
static volatile bool s_save_pending;

static void save_form(const char *body)
{
    char ssid[40] = {0}, pass[80] = {0}, surl[200] = {0}, pcode[32] = {0}, rcode[32] = {0}, rurl[128] = {0};
    bool has_ssid  = form_field(body, "ssid", ssid, sizeof ssid);
    bool has_pass  = form_field(body, "pass", pass, sizeof pass);
    bool has_surl  = form_field(body, "server_url", surl, sizeof surl);
    bool has_pcode = form_field(body, "pairing_code", pcode, sizeof pcode);
    form_field(body, "relay_code", rcode, sizeof rcode);
    form_field(body, "relay_url", rurl, sizeof rurl);

    /* NULL leaves a field unchanged; a blank password keeps the current one. */
    bool new_code = has_pcode && pcode[0];
    settings_save(has_ssid ? ssid : NULL, (has_pass && pass[0]) ? pass : NULL,
                  has_surl ? surl : NULL, new_code ? pcode : NULL);
    /* A fresh pairing code means "(re)pair": drop the old token and ETag. */
    if (new_code) { storage_clear_pairing(); storage_commit(); }
    if (rcode[0]) {
        settings_save_relay(rurl, rcode);   // relay mode, a fresh pairing
        storage_clear_pairing();
    } else if (surl[0] && settings_relay_mode()) {
        settings_relay_drop(false);         // back to the server
        settings_commit();
    }
    storage_set_portal_offered(false);
    storage_commit();
    LOGI("portal: saved (ssid '%s', server '%s'%s)", ssid, surl, new_code ? ", pairing code" : "");
}

/* Queue as much of the response as the send buffer takes. The page is larger
 * than TCP_SND_BUF, so it goes out over several calls, from sent_cb as the
 * browser acknowledges what it has. */
static void pump(struct tcp_pcb *pcb, conn_t *c)
{
    while (c->queued < c->resp_total) {
        size_t n = c->resp_total - c->queued;
        u16_t room = tcp_sndbuf(pcb);
        if (n > room) n = room;
        if (n == 0 || tcp_sndqueuelen(pcb) >= TCP_SND_QUEUELEN) break;
        if (tcp_write(pcb, c->resp + c->queued, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        c->queued += n;
    }
    tcp_output(pcb);
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = (conn_t *)arg;
    if (!c) return ERR_OK;
    c->acked += len;
    if (c->queued < c->resp_total) pump(pcb, c);
    if (c->acked >= c->resp_total) {
        bool reboot = c->reboot_after;
        conn_close(pcb, c);
        if (reboot) s_reboot_pending = true;
    }
    return ERR_OK;
}

static void send_response(struct tcp_pcb *pcb, conn_t *c, size_t total)
{
    free(c->resp);
    c->resp = malloc(total);
    if (!c->resp) { LOGW("portal: no memory for a %u-byte response", (unsigned)total); conn_close(pcb, c); return; }
    memcpy(c->resp, s_resp, total);
    c->resp_total = total;
    c->queued = c->acked = 0;
    tcp_sent(pcb, sent_cb);
    pump(pcb, c);
}

static void handle_request(struct tcp_pcb *pcb, conn_t *c)
{
    bool is_post = strncmp(c->req, "POST", 4) == 0;
    const char *body = strstr(c->req, "\r\n\r\n");
    body = body ? body + 4 : "";

    if (is_post && strstr(c->req, "/save")) {
        snprintf(s_pending_form, sizeof s_pending_form, "%s", body);
        s_save_pending = true;
        c->reboot_after = true;
        send_response(pcb, c, build_static(k_thanks_html));
    } else if (strstr(c->req, "GET /rescan")) {
        s_rescan_pending = true;
        send_response(pcb, c, build_static(k_scanning_html));
    } else {
        send_response(pcb, c, build_form());
    }
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = (conn_t *)arg;
    if (p == NULL) { conn_close(pcb, c); return ERR_OK; }   /* peer closed */
    if (err != ERR_OK || c == NULL) { pbuf_free(p); return ERR_OK; }

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        size_t n = q->len;
        if (c->req_len + n > REQ_MAX - 1) n = REQ_MAX - 1 - c->req_len;
        memcpy(c->req + c->req_len, q->payload, n);
        c->req_len += n;
    }
    c->req[c->req_len] = '\0';
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    /* Wait for the end of headers; for POST, also for the full body. */
    const char *hdr_end = strstr(c->req, "\r\n\r\n");
    if (!hdr_end) {
        if (c->req_len >= REQ_MAX - 1) handle_request(pcb, c);   /* oversized; respond anyway */
        return ERR_OK;
    }
    if (strncmp(c->req, "POST", 4) == 0) {
        const char *cl = strstr(c->req, "Content-Length:");
        size_t want = cl ? (size_t)atoi(cl + 15) : 0;
        size_t have = c->req_len - (size_t)((hdr_end + 4) - c->req);
        if (have < want && c->req_len < REQ_MAX - 1) return ERR_OK;   /* need more body */
    }
    handle_request(pcb, c);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
    (void)err;
    conn_t *c = (conn_t *)arg;
    if (c) { free(c->resp); free(c); }   /* pcb already freed by lwIP */
}

static err_t accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;
    conn_t *c = (conn_t *)calloc(1, sizeof(conn_t));
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    s_client_seen = true;   /* activity: keep the portal awake */
    tcp_arg(pcb, c);
    tcp_recv(pcb, recv_cb);
    tcp_err(pcb, err_cb);
    return ERR_OK;
}

/* ---------- portal entry ---------- */

void portal_ap_name(char *out, size_t cap)
{
    /* Per-board suffix from the flash unique ID (always available; the CYW43
     * MACs read 0 before the interface is up). */
    pico_unique_board_id_t bid;
    pico_get_unique_board_id(&bid);
    snprintf(out, cap, "%s-%02X%02X", SETUP_AP_PREFIX,
             bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2], bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
}

bool portal_run(const char *reason)
{
    snprintf(s_reason, sizeof s_reason, "%s", reason ? reason : "");
    char ssid[33];
    portal_ap_name(ssid, sizeof ssid);

    /* Paint the instructions and join QR with the radio still down. */
    screen_setup(ssid, SETUP_AP_PASSWORD, SETUP_PORTAL_URL, s_reason);

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE)) {
        LOGE("portal: cyw43 init failed");
        return false;
    }
    bool on_usb = cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
    LOGI("portal: AP '%s' (password '%s'), open %s", ssid, SETUP_AP_PASSWORD, SETUP_PORTAL_URL);
    cyw43_arch_enable_ap_mode(ssid, SETUP_AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    board_led(LED_CONNECTION, 60);

    ip_addr_t gw, mask;
    IP4_ADDR(ip_2_ip4(&gw), 192, 168, 4, 1);
    IP4_ADDR(ip_2_ip4(&mask), 255, 255, 255, 0);
    static dhcp_server_t dhcp;
    static dns_server_t dns;
    dhcp_server_init(&dhcp, &gw, &mask);
    dns_server_init(&dns, &gw);

    cyw43_arch_lwip_begin();
    struct tcp_pcb *listener = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (listener && tcp_bind(listener, IP_ANY_TYPE, 80) == ERR_OK) {
        listener = tcp_listen_with_backlog(listener, 2);
        tcp_accept(listener, accept_cb);
    } else {
        LOGE("portal: failed to bind :80");
    }
    cyw43_arch_lwip_end();

    /* Initial scan with the AP up, so a client never drops, to fill the picker. */
    wifi_scan();

    /* Serve until the form is saved. Rescans run here, never in an lwIP
     * callback, since a scan blocks for seconds. Idle: any connection resets
     * the timer; after SETUP_IDLE_TIMEOUT_MS the portal closes. */
    uint32_t idle_ms = 0;
    bool timed_out = false;
    while (!s_reboot_pending) {
        if (s_rescan_pending) { s_rescan_pending = false; wifi_scan(); }
        if (s_client_seen) { s_client_seen = false; idle_ms = 0; }
        else if ((idle_ms += 50) >= SETUP_IDLE_TIMEOUT_MS && !on_usb) { timed_out = true; break; }
        sleep_ms(50);
    }

    if (timed_out) {
        LOGW("portal: no activity for %lu min; closing", (unsigned long)(SETUP_IDLE_TIMEOUT_MS / 60000));
        cyw43_arch_lwip_begin();
        if (listener) tcp_close(listener);
        cyw43_arch_lwip_end();
        dns_server_deinit(&dns);
        dhcp_server_deinit(&dhcp);
        cyw43_arch_deinit();
        board_led(LED_CONNECTION, 0);
        return false;
    }

    sleep_ms(800);   /* let the "Saved" page reach the browser */
    cyw43_arch_deinit();
    if (s_save_pending) save_form(s_pending_form);
    LOGI("portal: settings saved; rebooting");
    power_hold_over_reboot();   // on battery the reset would power the frame off
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}
