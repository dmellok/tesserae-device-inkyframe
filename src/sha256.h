// SHA-256 (FIPS 180-4), incremental. Small and portable so the OTA image can
// be hashed as it streams in, and so the code builds on the host for tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;        // bytes hashed so far
    uint8_t buf[64];
    size_t fill;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);
// Lowercase hex of a 32-byte digest into out (65 bytes with the NUL).
void sha256_hex(const uint8_t digest[32], char out[65]);
