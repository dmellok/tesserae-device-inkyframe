#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include "hardware/sync.h"

static char s_ring[LOG_RING_BYTES];
static size_t s_head;     // next write position
static size_t s_len;      // bytes held (<= LOG_RING_BYTES)
static uint32_t s_total;  // bytes ever appended (log_ring_mark)

static void ring_put(const char *s, size_t n)
{
    // The network stack logs from interrupt context: append atomically.
    uint32_t ints = save_and_disable_interrupts();
    for (size_t i = 0; i < n; i++) {
        s_ring[s_head] = s[i];
        s_head = (s_head + 1) % LOG_RING_BYTES;
    }
    s_len = s_len + n > LOG_RING_BYTES ? LOG_RING_BYTES : s_len + n;
    s_total += (uint32_t)n;
    restore_interrupts(ints);
}

void log_emit(char level, const char *fmt, ...)
{
    char line[256];
    int n = snprintf(line, sizeof line, "[%6lu] %c ", (unsigned long)to_ms_since_boot(get_absolute_time()), level);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    va_end(ap);
    size_t len = (size_t)n + (m < 0 ? 0 : (size_t)m);
    if (len > sizeof line - 2) len = sizeof line - 2;
    line[len++] = '\n';
    line[len] = 0;
    fputs(line, stdout);
    ring_put(line, len);
}

uint32_t log_ring_mark(void)
{
    uint32_t ints = save_and_disable_interrupts();
    uint32_t m = s_total;
    restore_interrupts(ints);
    return m;
}

size_t log_ring_copy(char *out, size_t cap)
{
    return log_ring_copy_since(out, cap, 0);
}

size_t log_ring_copy_since(char *out, size_t cap, uint32_t mark)
{
    uint32_t ints = save_and_disable_interrupts();
    size_t held = s_len;
    uint32_t after = s_total - mark;   // logged since the mark
    if (mark && after < held) held = after;
    size_t n = held < cap ? held : cap;
    size_t start = (s_head + LOG_RING_BYTES - held) % LOG_RING_BYTES;
    // Keep the newest bytes when out is smaller than what is wanted.
    start = (start + (held - n)) % LOG_RING_BYTES;
    for (size_t i = 0; i < n; i++) out[i] = s_ring[(start + i) % LOG_RING_BYTES];
    bool cut = n < (mark ? after : s_total);   // the first line may be partial
    restore_interrupts(ints);
    // Start on a whole line.
    char *nl = n && cut ? memchr(out, '\n', n) : NULL;
    if (nl) { size_t skip = (size_t)(nl - out) + 1; memmove(out, nl + 1, n - skip); n -= skip; }
    return n;
}
