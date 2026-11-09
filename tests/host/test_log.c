// Host tests for the log ring (src/log.c): whole lines out, the newest kept
// when it wraps or the buffer is small, and log_ring_copy_since(), which the
// OTA log uses so a log carried over a reboot does not repeat what already
// went out. Run tests/host/run.sh.
#include <stdio.h>
#include <string.h>

#include "log.h"

static int failures;
// Results go to stderr: log_emit() echoes every line to stdout, as on USB serial.
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static char out[LOG_RING_BYTES + 1];

static size_t copy(size_t cap, uint32_t mark)
{
    size_t n = mark ? log_ring_copy_since(out, cap, mark) : log_ring_copy(out, cap);
    out[n] = 0;
    return n;
}

static int count_lines(const char *s) { int k = 0; for (; *s; s++) k += *s == '\n'; return k; }

int main(void)
{
    // Nothing logged yet.
    CHECK(copy(sizeof out - 1, 0) == 0, "empty ring copied %u bytes", (unsigned)strlen(out));
    CHECK(log_ring_mark() == 0, "mark before logging");

    LOGI("first %d", 1);
    LOGW("second");
    copy(sizeof out - 1, 0);
    CHECK(strstr(out, "I first 1\n") && strstr(out, "W second\n") && count_lines(out) == 2, "two lines: %s", out);
    CHECK(out[0] == '[', "starts on a line: %.20s", out);

    // Since a mark: only what came after it.
    uint32_t mark = log_ring_mark();
    CHECK(mark == strlen(out), "mark %u = bytes logged %u", (unsigned)mark, (unsigned)strlen(out));
    CHECK(copy(sizeof out - 1, mark) == 0, "nothing since the mark: %s", out);
    LOGE("third");
    copy(sizeof out - 1, mark);
    CHECK(strcmp(strstr(out, "] ") + 2, "E third\n") == 0 && count_lines(out) == 1, "since mark: %s", out);

    // A small buffer keeps the newest whole lines.
    copy(40, 0);
    CHECK(out[0] == '[' && strstr(out, "E third\n") && !strstr(out, "first"), "small cap: %s", out);

    // Wrap the ring several times: still whole lines, the newest last.
    for (int i = 0; i < 3000; i++) LOGI("filler line %04d with some padding to fill the ring", i);
    size_t n = copy(sizeof out - 1, 0);
    CHECK(n > LOG_RING_BYTES - 128 && n <= LOG_RING_BYTES, "wrapped ring copied %u", (unsigned)n);
    CHECK(out[0] == '[', "wrapped ring starts on a line: %.30s", out);
    CHECK(strstr(out, "filler line 2999 with some padding to fill the ring\n") != NULL, "newest line kept");
    CHECK(!strstr(out, "third"), "oldest lines gone");

    // A mark older than what the ring still holds: the whole ring, from a line.
    n = copy(sizeof out - 1, mark);
    CHECK(out[0] == '[' && n > LOG_RING_BYTES - 128, "old mark: %u bytes from %.20s", (unsigned)n, out);

    // A recent mark after wrapping.
    mark = log_ring_mark();
    LOGI("after wrap");
    copy(sizeof out - 1, mark);
    CHECK(count_lines(out) == 1 && strstr(out, "I after wrap\n"), "since mark after wrap: %s", out);

    if (failures) { fprintf(stderr, "%d log ring check(s) failed\n", failures); return 1; }
    fprintf(stderr, "log ring: all checks passed\n");
    return 0;
}
