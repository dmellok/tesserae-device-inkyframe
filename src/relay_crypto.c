#include "relay_crypto.h"

#include <string.h>

#include "monocypher.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

static const char INFO[] = "tesserae-relay-frame-key-v1";

void relay_public_key(const uint8_t priv[32], uint8_t pub[32])
{
    crypto_x25519_public_key(pub, priv);
}

bool relay_derive_frame_key(const uint8_t priv[32], const uint8_t their_pub[32], uint8_t key[32])
{
    uint8_t shared[32];
    crypto_x25519(shared, priv, their_pub);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= shared[i];
    if (!acc) { crypto_wipe(shared, sizeof shared); return false; }
    int rc = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), NULL, 0, shared, sizeof shared,
                          (const unsigned char *)INFO, sizeof INFO - 1, key, 32);
    crypto_wipe(shared, sizeof shared);
    return rc == 0;
}

bool relay_unseal_begin(relay_unseal_t *u, const uint8_t key[32], const uint8_t nonce[RELAY_NONCE_BYTES])
{
    mbedtls_gcm_init(&u->gcm);
    if (mbedtls_gcm_setkey(&u->gcm, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) return false;
    return mbedtls_gcm_starts(&u->gcm, MBEDTLS_GCM_DECRYPT, nonce, RELAY_NONCE_BYTES) == 0;
}

bool relay_unseal_update(relay_unseal_t *u, const uint8_t *in, size_t len, uint8_t *out)
{
    size_t produced = 0;
    // GCM may hold back up to 15 bytes until the next block arrives, so `out`
    // must be at least len + 15 bytes; callers pass a block-sized multiple.
    if (mbedtls_gcm_update(&u->gcm, in, len, out, len + 15, &produced) != 0) return false;
    return produced == len;
}

bool relay_unseal_finish(relay_unseal_t *u, const uint8_t tag[RELAY_TAG_BYTES])
{
    uint8_t want[RELAY_TAG_BYTES];
    size_t tail = 0;
    uint8_t spill[16];
    int rc = mbedtls_gcm_finish(&u->gcm, spill, sizeof spill, &tail, want, sizeof want);
    mbedtls_gcm_free(&u->gcm);
    if (rc != 0 || tail != 0) return false;
    uint8_t diff = 0;
    for (int i = 0; i < RELAY_TAG_BYTES; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    return diff == 0;
}

int relay_unseal(const uint8_t key[32], const uint8_t *sealed, size_t len, uint8_t *out, size_t cap)
{
    if (len < RELAY_SEAL_OVERHEAD) return -1;
    size_t n = len - RELAY_SEAL_OVERHEAD;
    if (cap < n + 15) return -1;
    relay_unseal_t u;
    if (!relay_unseal_begin(&u, key, sealed)) { mbedtls_gcm_free(&u.gcm); return -1; }
    if (n && !relay_unseal_update(&u, sealed + RELAY_NONCE_BYTES, n, out)) { mbedtls_gcm_free(&u.gcm); memset(out, 0, cap); return -1; }
    if (!relay_unseal_finish(&u, sealed + RELAY_NONCE_BYTES + n)) { memset(out, 0, cap); return -1; }
    return (int)n;
}
