// Minimal synchronous HTTP/1.1 client on lwIP's raw TCP API. One request at a
// time. Sets request headers, exposes the status code and the few response
// headers the Tesserae protocol needs, and streams large bodies to a sink.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int status;              // HTTP status, or a negative HTTP_ERR_* code
    char etag[80];           // ETag header, quotes stripped
    int retry_after;         // Retry-After header (seconds), 0 if absent
    uint32_t date_epoch;     // Date header as unix time, 0 if absent
    bool has_length;
    size_t content_length;
    char *body;              // malloc'd, NUL-terminated (NULL when a sink was used)
    size_t body_len;
    bool truncated;          // body exceeded HTTP_BODY_MAX
} http_resp_t;

enum {
    HTTP_ERR_URL = -1,
    HTTP_ERR_DNS = -2,
    HTTP_ERR_CONNECT = -3,
    HTTP_ERR_TIMEOUT = -4,
    HTTP_ERR_TRANSPORT = -5,
    HTTP_ERR_ABORTED = -6,   // the length callback refused the body
    HTTP_ERR_BUSY = -7,
};

// Called once when the response headers are in, with the Content-Length (0 if
// unknown). Return false to abort. Called from lwIP's callback context.
typedef bool (*http_length_cb)(size_t content_length, void *arg);
// Receives body bytes in order. Called from lwIP's callback context.
typedef void (*http_sink_cb)(const uint8_t *data, size_t len, void *arg);

// `headers` are full header lines without CRLF. `body` (may be NULL) is sent
// as application/json. With a sink, the body streams to it and resp->body
// stays NULL; otherwise the body is collected into resp->body.
int http_request(const char *method, const char *url,
                 const char *const *headers, int nheaders, const char *body,
                 http_resp_t *resp, http_length_cb on_length, http_sink_cb sink, void *arg,
                 uint32_t timeout_ms, uint32_t stall_ms);

void http_resp_free(http_resp_t *resp);
