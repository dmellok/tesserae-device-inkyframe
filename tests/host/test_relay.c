// Host tests for the relay crypto against the contract's golden vectors
// (Tesserae docs/relay/contract.md, tests/test_relay_crypto.py).
#include <stdio.h>
#include <string.h>

#include "relay_crypto.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) sscanf(h + 2 * i, "%2hhx", &out[i]);
}

int main(void)
{
    uint8_t home_priv[32], panel_priv[32], home_pub[32], panel_pub[32], want[32];
    memset(home_priv, 0x01, 32);
    memset(panel_priv, 0x02, 32);
    relay_public_key(home_priv, home_pub);
    relay_public_key(panel_priv, panel_pub);
    hex("a4e09292b651c278b9772c569f5fa9bb13d906b46ab68c9df9dc2b4409f8a209", want, 32);
    CHECK(memcmp(home_pub, want, 32) == 0, "home_pubkey");
    hex("ce8d3ad1ccb633ec7b70c17814a5c76ecd029685050d344745ba05870e587d59", want, 32);
    CHECK(memcmp(panel_pub, want, 32) == 0, "panel_pubkey");

    uint8_t k_panel[32], k_home[32];
    CHECK(relay_derive_frame_key(panel_priv, home_pub, k_panel), "derive (panel side)");
    CHECK(relay_derive_frame_key(home_priv, panel_pub, k_home), "derive (home side)");
    hex("613376ae6bc97931b6d33c17aaf561fb2ff2e2f12937249705e1e5b75dd98e83", want, 32);
    CHECK(memcmp(k_panel, want, 32) == 0, "frame_key");
    CHECK(memcmp(k_home, k_panel, 32) == 0, "both sides agree");

    const char *sealed_hex = "00112233445566778899aabb4fa3210211222b8aa47f010d2a30fc71c"
                             "70e6de8d4345adf90c057a2b39262b5ab8cee9507d7839ce8";
    uint8_t sealed[64], out[128];
    size_t sl = strlen(sealed_hex) / 2;
    hex(sealed_hex, sealed, sl);
    uint8_t frame[25];
    hex("74657373657261652d6672616d652d0001027061796c6f6164", frame, sizeof frame);
    int n = relay_unseal(k_panel, sealed, sl, out, sizeof out);
    CHECK(n == (int)sizeof frame && memcmp(out, frame, sizeof frame) == 0, "unseal the golden frame (n=%d)", n);

    // Streamed in odd chunks, as a frame from PSRAM would be.
    relay_unseal_t u;
    uint8_t outs[128];
    size_t ct = sl - RELAY_SEAL_OVERHEAD, off = 0;
    CHECK(relay_unseal_begin(&u, k_panel, sealed), "begin");
    for (size_t step = 7; off < ct; off += step) {
        size_t m = ct - off < step ? ct - off : step;
        CHECK(relay_unseal_update(&u, sealed + 12 + off, m, outs + off), "update at %u", (unsigned)off);
    }
    CHECK(relay_unseal_finish(&u, sealed + 12 + ct), "finish");
    CHECK(memcmp(outs, frame, sizeof frame) == 0, "streamed plaintext");

    // Any flipped bit fails authentication.
    sealed[20] ^= 1;
    CHECK(relay_unseal(k_panel, sealed, sl, out, sizeof out) < 0, "tampered ciphertext rejected");
    sealed[20] ^= 1;
    sealed[sl - 1] ^= 0x80;
    CHECK(relay_unseal(k_panel, sealed, sl, out, sizeof out) < 0, "tampered tag rejected");

    // An all-zero peer key gives an all-zero shared secret: refused.
    uint8_t zero[32] = {0};
    CHECK(!relay_derive_frame_key(panel_priv, zero, k_panel), "all-zero peer key refused");

    printf(failures ? "%d FAILED\n" : "all relay host tests passed\n", failures);
    return failures ? 1 : 0;
}
