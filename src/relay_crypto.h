// Cloud-relay crypto (Tesserae docs/relay/contract.md): X25519 + HKDF-SHA256
// to the 32-byte frame key, and AES-256-GCM to unseal frames and config.
//
//   frame_key = HKDF-SHA256(ikm = X25519(our_priv, their_pub), salt = none,
//                           info = "tesserae-relay-frame-key-v1", L = 32)
//   sealed    = nonce (12) || AES-256-GCM(plaintext, frame_key, nonce, aad = "") || tag (16)
//
// Portable C (Monocypher + Mbed TLS), so tests/host checks it against the
// contract's golden vectors.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/gcm.h"

#define RELAY_NONCE_BYTES 12
#define RELAY_TAG_BYTES   16
#define RELAY_SEAL_OVERHEAD (RELAY_NONCE_BYTES + RELAY_TAG_BYTES)

// The public key for a 32-byte private scalar (X25519 clamps it).
void relay_public_key(const uint8_t priv[32], uint8_t pub[32]);

// Derive the frame key. False if the shared secret is all zero (a hostile or
// broken peer key).
bool relay_derive_frame_key(const uint8_t priv[32], const uint8_t their_pub[32], uint8_t key[32]);

// Streaming unseal, for a frame too large to hold in RAM: begin with the
// nonce, update over the ciphertext in any chunking, then finish with the tag.
typedef struct {
    mbedtls_gcm_context gcm;
} relay_unseal_t;

bool relay_unseal_begin(relay_unseal_t *u, const uint8_t key[32], const uint8_t nonce[RELAY_NONCE_BYTES]);
bool relay_unseal_update(relay_unseal_t *u, const uint8_t *in, size_t len, uint8_t *out);
// True only if the tag matches: until then the plaintext must not be used.
bool relay_unseal_finish(relay_unseal_t *u, const uint8_t tag[RELAY_TAG_BYTES]);

// One-shot unseal of a small document (config). Returns the plaintext length,
// or -1 if it is malformed or does not authenticate.
int relay_unseal(const uint8_t key[32], const uint8_t *sealed, size_t len, uint8_t *out, size_t cap);
