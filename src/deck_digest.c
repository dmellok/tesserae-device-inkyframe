/*
 * deck_digest.c -- device-side SHA-256 backend for deck.c (ported from
 * tesserae-device-firmware), on this firmware's own sha256.c. A separate
 * translation unit so deck.c stays pure; the host test links its own.
 */
#include "deck.h"
#include "sha256.h"

void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}
