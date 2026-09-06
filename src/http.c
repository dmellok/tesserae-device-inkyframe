#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#include "config.h"
#include "log.h"
#include "wifi.h"

#define HDR_MAX   2048
#define REQ_MAX   1536

typedef enum { ST_HEADERS, ST_BODY, ST_DONE } state_t;
typedef enum { CH_SIZE, CH_DATA, CH_CRLF, CH_TRAILER } chunk_state_t;

static struct {
    struct tcp_pcb *pcb;
    volatile bool pcb_dead, done, err;
    volatile state_t state;
    char hdr[HDR_MAX + 1];
    size_t hdr_len;
    bool hdr_overflow;
    // body
    bool chunked;
    size_t body_remaining;      // for content-length mode
    chunk_state_t cs;
    size_t chunk_remaining;
    char chunk_line[16];
    size_t chunk_line_len;
    bool chunk_ext;             // inside a ";ext=val" suffix on the size line
    http_resp_t *resp;
    http_length_cb on_length;
    http_sink_cb sink;
    void *arg;
    size_t received;
    volatile uint32_t last_data_ms;
    // request
    char req[REQ_MAX];
    size_t req_len, req_sent;
    const char *req_body;
    size_t req_body_len, req_body_sent;
} s;

static volatile bool s_busy;

// ---- helpers ----

static const char *find_header(const char *name)
{
    const char *p = s.hdr;
    size_t nl = strlen(name);
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
    }
    return NULL;
}

static void header_value(const char *name, char *out, size_t cap)
{
    out[0] = 0;
    const char *v = find_header(name);
    if (!v) return;
    const char *e = strstr(v, "\r\n");
    size_t n = e ? (size_t)(e - v) : strlen(v);
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// "Sat, 06 Sep 2026 15:04:05 GMT"
static uint32_t parse_http_date(const char *v)
{
    static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char m[4] = {0};
    int d, y, hh, mm, ss;
    const char *comma = strchr(v, ',');
    if (comma) v = comma + 1;
    if (sscanf(v, " %d %3s %d %d:%d:%d", &d, m, &y, &hh, &mm, &ss) != 6) return 0;
    const char *mp = strstr(mon, m);
    if (!mp) return 0;
    unsigned month = (unsigned)((mp - mon) / 3) + 1;
    int64_t days = days_from_civil(y, month, (unsigned)d);
    return (uint32_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

static void body_bytes(const uint8_t *d, size_t n)
{
    if (!n) return;
    s.received += n;
    s.last_data_ms = to_ms_since_boot(get_absolute_time());
    if (s.sink) { s.sink(d, n, s.arg); return; }
    http_resp_t *r = s.resp;
    if (!r->body) {
        r->body = malloc(HTTP_BODY_MAX + 1);
        if (!r->body) { r->truncated = true; return; }
        r->body_len = 0;
    }
    size_t room = HTTP_BODY_MAX - r->body_len;
    if (n > room) { n = room; r->truncated = true; }
    memcpy(r->body + r->body_len, d, n);
    r->body_len += n;
    r->body[r->body_len] = 0;
}

static void finish(void) { s.state = ST_DONE; s.done = true; }

// De-chunk (if needed) and deliver body bytes.
static void feed_body(const uint8_t *d, size_t n)
{
    if (!s.chunked) {
        if (s.resp->has_length) {
            if (n > s.body_remaining) n = s.body_remaining;
            body_bytes(d, n);
            s.body_remaining -= n;
            if (s.body_remaining == 0) finish();
        } else {
            body_bytes(d, n);
        }
        return;
    }
    while (n > 0 && s.state != ST_DONE) {
        switch (s.cs) {
        case CH_SIZE: {
            char c = (char)*d++; n--;
            if (c == '\n') {
                s.chunk_line[s.chunk_line_len] = 0;
                s.chunk_remaining = (size_t)strtoul(s.chunk_line, NULL, 16);
                s.chunk_line_len = 0;
                s.chunk_ext = false;
                s.cs = s.chunk_remaining ? CH_DATA : CH_TRAILER;
            } else if (c == ';') {
                // Chunk extension: everything to the newline is not the size.
                s.chunk_ext = true;
            } else if (!s.chunk_ext && c != '\r' && s.chunk_line_len < sizeof s.chunk_line - 1) {
                s.chunk_line[s.chunk_line_len++] = c;
            }
            break;
        }
        case CH_DATA: {
            size_t take = n < s.chunk_remaining ? n : s.chunk_remaining;
            body_bytes(d, take);
            d += take; n -= take; s.chunk_remaining -= take;
            if (s.chunk_remaining == 0) s.cs = CH_CRLF;
            break;
        }
        case CH_CRLF: {
            char c = (char)*d++; n--;
            if (c == '\n') s.cs = CH_SIZE;
            break;
        }
        case CH_TRAILER:
            finish();
            return;
        }
    }
}

static void on_headers_complete(void)
{
    // Status line.
    int status = 0;
    const char *sp = strchr(s.hdr, ' ');
    if (sp) status = atoi(sp + 1);
    s.resp->status = status;

    char v[64];
    header_value("ETag", v, sizeof v);
    {
        char *p = v;
        if (strncmp(p, "W/", 2) == 0) p += 2;
        size_t l = strlen(p);
        if (l >= 2 && p[0] == '"' && p[l - 1] == '"') { p[l - 1] = 0; p++; }
        snprintf(s.resp->etag, sizeof s.resp->etag, "%s", p);
    }
    header_value("Retry-After", v, sizeof v);
    s.resp->retry_after = atoi(v);
    header_value("Date", v, sizeof v);
    s.resp->date_epoch = parse_http_date(v);
    header_value("Transfer-Encoding", v, sizeof v);
    for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
    s.chunked = strstr(v, "chunked") != NULL;
    header_value("Content-Length", v, sizeof v);
    if (v[0]) {
        s.resp->has_length = true;
        s.resp->content_length = (size_t)strtoul(v, NULL, 10);
        s.body_remaining = s.resp->content_length;
    }
    s.cs = CH_SIZE; s.chunk_line_len = 0;
    s.state = ST_BODY;

    bool no_body = status == 204 || status == 304 || (status >= 100 && status < 200);
    if (s.on_length && !s.on_length(s.resp->has_length ? s.resp->content_length : 0, s.arg)) {
        s.err = true; s.resp->status = HTTP_ERR_ABORTED; finish(); return;
    }
    if (no_body || (s.resp->has_length && s.resp->content_length == 0 && !s.chunked)) finish();
}

static void feed(const uint8_t *d, size_t n)
{
    if (s.state == ST_HEADERS) {
        size_t room = HDR_MAX - s.hdr_len;
        size_t take = n < room ? n : room;
        memcpy(s.hdr + s.hdr_len, d, take);
        s.hdr_len += take;
        s.hdr[s.hdr_len] = 0;
        char *end = strstr(s.hdr, "\r\n\r\n");
        if (!end) {
            if (take < n) { s.hdr_overflow = true; s.err = true; finish(); }
            return;
        }
        size_t hdr_bytes = (size_t)(end + 4 - s.hdr);
        size_t extra = s.hdr_len - hdr_bytes;
        *end = 0;   // terminate headers; keep the rest for the body
        // Body bytes that arrived with the headers are still intact after `end`.
        const uint8_t *spill = (const uint8_t *)(end + 4);
        on_headers_complete();
        if (s.state == ST_BODY) {
            feed_body(spill, extra);
            if (s.state == ST_BODY && take < n) feed_body(d + take, n - take);
        }
        return;
    }
    if (s.state == ST_BODY) feed_body(d, n);
}

// ---- raw TCP callbacks (under the lwIP lock) ----

static void try_send(struct tcp_pcb *pcb)
{
    while (s.req_sent < s.req_len) {
        u16_t avail = tcp_sndbuf(pcb);
        if (!avail) return;
        size_t rem = s.req_len - s.req_sent;
        u16_t n = rem < avail ? (u16_t)rem : avail;
        if (tcp_write(pcb, s.req + s.req_sent, n, TCP_WRITE_FLAG_COPY | (s.req_body ? TCP_WRITE_FLAG_MORE : 0)) != ERR_OK) return;
        s.req_sent += n;
    }
    while (s.req_body && s.req_body_sent < s.req_body_len) {
        u16_t avail = tcp_sndbuf(pcb);
        if (!avail) return;
        size_t rem = s.req_body_len - s.req_body_sent;
        u16_t n = rem < avail ? (u16_t)rem : avail;
        if (tcp_write(pcb, s.req_body + s.req_body_sent, n, TCP_WRITE_FLAG_COPY) != ERR_OK) return;
        s.req_body_sent += n;
    }
    tcp_output(pcb);
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)len;
    try_send(pcb);
    return ERR_OK;
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (!p) {   // remote closed
        if (s.state == ST_BODY && !s.chunked && !s.resp->has_length) finish();
        // Terminal chunk seen; a FIN before its trailing CRLF is still a
        // complete body.
        else if (s.state == ST_BODY && s.chunked && s.cs == CH_TRAILER) finish();
        else if (s.state != ST_DONE) { s.err = true; finish(); }
        return ERR_OK;
    }
    if (err == ERR_OK) {
        for (struct pbuf *q = p; q; q = q->next)
            if (s.state != ST_DONE) feed((const uint8_t *)q->payload, q->len);
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err)
{
    (void)arg; (void)err;
    s.pcb = NULL; s.pcb_dead = true;
    if (s.state != ST_DONE) { s.err = true; finish(); }
}

static err_t connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK) { s.err = true; finish(); return ERR_OK; }
    try_send(pcb);
    return ERR_OK;
}

// ---- request ----

static bool split(const char *url, char *host, size_t hcap, uint16_t *port, const char **path)
{
    const char *p = strstr(url, "://");
    // Plain HTTP only: there is no TLS in this build, and silently connecting
    // to port 80 for an https:// URL would fail in a confusing way.
    if (p && !(p - url == 4 && strncasecmp(url, "http", 4) == 0)) return false;
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    size_t hp_len = slash ? (size_t)(slash - p) : strlen(p);
    const char *colon = memchr(p, ':', hp_len);
    size_t hl = colon ? (size_t)(colon - p) : hp_len;
    if (hl == 0 || hl >= hcap) return false;
    memcpy(host, p, hl); host[hl] = 0;
    *port = 80;
    if (colon) { int v = atoi(colon + 1); if (v > 0 && v < 65536) *port = (uint16_t)v; }
    *path = slash ? slash : "/";
    return true;
}

int http_request(const char *method, const char *url,
                 const char *const *headers, int nheaders, const char *body,
                 http_resp_t *resp, http_length_cb on_length, http_sink_cb sink, void *arg,
                 uint32_t timeout_ms, uint32_t stall_ms)
{
    memset(resp, 0, sizeof *resp);
    if (s_busy) return resp->status = HTTP_ERR_BUSY;
    s_busy = true;

    char host[96]; uint16_t port; const char *path;
    if (!split(url, host, sizeof host, &port, &path)) { s_busy = false; return resp->status = HTTP_ERR_URL; }

    memset(&s, 0, sizeof s);
    s.resp = resp; s.on_length = on_length; s.sink = sink; s.arg = arg;
    s.state = ST_HEADERS;

    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(s.req, sizeof s.req,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nUser-Agent: tesserae-inkyframe/%s\r\nAccept: application/json\r\n",
                     method, path, host, port, FW_VERSION);
    for (int i = 0; i < nheaders && n > 0 && n < (int)sizeof s.req; i++)
        n += snprintf(s.req + n, sizeof s.req - n, "%s\r\n", headers[i]);
    if (body && n > 0 && n < (int)sizeof s.req)
        n += snprintf(s.req + n, sizeof s.req - n, "Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)blen);
    if (n > 0 && n < (int)sizeof s.req) n += snprintf(s.req + n, sizeof s.req - n, "\r\n");
    if (n <= 0 || n >= (int)sizeof s.req) { s_busy = false; LOGE("http: request too large"); return resp->status = HTTP_ERR_URL; }
    s.req_len = (size_t)n;
    s.req_body = body; s.req_body_len = blen;

    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    ip_addr_t ip;
    if (!wifi_resolve(host, &ip, 5000)) { s_busy = false; LOGE("http: cannot resolve %s", host); return resp->status = HTTP_ERR_DNS; }

    cyw43_arch_lwip_begin();
    s.pcb = tcp_new_ip_type(IP_GET_TYPE(&ip));
    if (s.pcb) {
        tcp_err(s.pcb, err_cb);
        tcp_recv(s.pcb, recv_cb);
        tcp_sent(s.pcb, sent_cb);
        if (tcp_connect(s.pcb, &ip, port, connected_cb) != ERR_OK) { tcp_abort(s.pcb); s.pcb = NULL; s.pcb_dead = true; }
    }
    cyw43_arch_lwip_end();
    if (!s.pcb) { s_busy = false; LOGE("http: connect failed"); return resp->status = HTTP_ERR_CONNECT; }

    // Start the stall clock now, not before DNS: resolution can take seconds
    // and would otherwise be charged against the download's stall budget.
    s.last_data_ms = to_ms_since_boot(get_absolute_time());
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    bool timed_out = false;
    while (!s.done) {
        if (time_reached(deadline)) { timed_out = true; break; }
        if (stall_ms && s.state == ST_BODY && to_ms_since_boot(get_absolute_time()) - s.last_data_ms > stall_ms) { timed_out = true; break; }
        sleep_ms(5);
    }

    cyw43_arch_lwip_begin();
    if (s.pcb && !s.pcb_dead) {
        tcp_recv(s.pcb, NULL); tcp_sent(s.pcb, NULL); tcp_err(s.pcb, NULL);
        if (tcp_close(s.pcb) != ERR_OK) tcp_abort(s.pcb);
        s.pcb = NULL;
    }
    cyw43_arch_lwip_end();

    uint32_t dt = to_ms_since_boot(get_absolute_time()) - t0;
    if (timed_out) {
        LOGE("http: %s %s timed out after %lu ms (%u body bytes)", method, url, (unsigned long)dt, (unsigned)s.received);
        resp->status = HTTP_ERR_TIMEOUT;
    } else if (s.err && resp->status <= 0) {
        LOGE("http: %s %s transport error", method, url);
        if (resp->status == 0) resp->status = HTTP_ERR_TRANSPORT;
    } else if (s.err) {
        LOGW("http: %s %s -> %d but the body was cut short (%u bytes)", method, url, resp->status, (unsigned)s.received);
        resp->status = HTTP_ERR_TRANSPORT;
    } else {
        LOGI("http: %s %s -> %d (%lu ms, %u bytes)", method, url, resp->status, (unsigned long)dt, (unsigned)s.received);
        if (resp->status >= 400 && resp->body) LOGW("http: body: %.200s", resp->body);
    }
    s_busy = false;
    return resp->status;
}

void http_resp_free(http_resp_t *resp)
{
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}
