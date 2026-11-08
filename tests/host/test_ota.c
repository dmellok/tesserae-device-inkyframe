// Host tests for the OTA verifier and SHA-256, against the signed fixtures
// from the Tesserae repo (tests/fixtures/ota, test-only key). Run tests/host/run.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_verify.h"
#include "sha256.h"
#include "vendor/cJSON.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("short read %s\n", path); exit(2); }
    fclose(f); b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}

static ota_verify_result_t verify_file(const char *name, const char *kind, const char *running, ota_manifest_t *m)
{
    char path[256];
    snprintf(path, sizeof path, "fixtures/%s.json", name);
    char *txt = slurp(path, NULL);
    cJSON *d = cJSON_Parse(txt);
    ota_verify_result_t r = ota_verify_descriptor(cJSON_GetObjectItem(d, "payload")->valuestring,
                                                  cJSON_GetObjectItem(d, "signature")->valuestring, kind, running, m);
    cJSON_Delete(d); free(txt);
    return r;
}

static void digest_of(const char *path, char hex[65])
{
    size_t n; char *b = slurp(path, &n);
    sha256_ctx_t c; sha256_init(&c);
    // Feed in odd chunks, as a stream would.
    for (size_t i = 0; i < n; i += 37) sha256_update(&c, b + i, n - i < 37 ? n - i : 37);
    uint8_t d[32]; sha256_final(&c, d); sha256_hex(d, hex);
    free(b);
}

int main(void)
{
    // SHA-256 known answers.
    char hex[65]; uint8_t d[32]; sha256_ctx_t c;
    sha256_init(&c); sha256_final(&c, d); sha256_hex(d, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "empty: %s", hex);
    sha256_init(&c); sha256_update(&c, "abc", 3); sha256_final(&c, d); sha256_hex(d, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "abc: %s", hex);
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&c); sha256_update(&c, m448, strlen(m448)); sha256_final(&c, d); sha256_hex(d, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "448-bit: %s", hex);

    ota_manifest_t m;
    CHECK(verify_file("valid", "esp32_client", "1.3.0", &m) == OTA_VERIFY_OK, "valid should verify");
    CHECK(strcmp(m.fw_version, "1.4.0") == 0 && m.size_bytes == 1152, "valid manifest fields");
    digest_of("fixtures/sample_image.bin", hex);
    CHECK(strcmp(hex, m.sha256) == 0, "image digest %s vs %s", hex, m.sha256);

    CHECK(verify_file("wrong_key", "esp32_client", "1.3.0", &m) == OTA_VERIFY_BAD_SIGNATURE, "wrong_key -> bad_signature");
    CHECK(verify_file("truncated", "esp32_client", "1.3.0", &m) == OTA_VERIFY_MALFORMED_DESCRIPTOR, "truncated -> malformed_descriptor");
    CHECK(verify_file("digest_mismatch", "esp32_client", "1.3.0", &m) == OTA_VERIFY_OK, "digest_mismatch passes the descriptor checks");
    CHECK(strcmp(hex, m.sha256) != 0, "digest_mismatch's sha256 does not match the image");
    CHECK(verify_file("valid", "pimoroni_inky_frame_73", "1.3.0", &m) == OTA_VERIFY_KIND_MISMATCH, "kind mismatch");
    CHECK(verify_file("valid", "esp32_client", "1.4.0", &m) == OTA_VERIFY_ALREADY_CURRENT, "already current");

    // A real production descriptor (tesserae-device-pi 0.9.3, key prod-1):
    // proves the embedded production key bytes.
    CHECK(verify_file("prod_pi_client_0.9.3", "pi_client", "0.9.2", &m) == OTA_VERIFY_OK, "prod-1 descriptor verifies");
    CHECK(strcmp(m.key_id, "prod-1") == 0 && strcmp(m.fw_version, "0.9.3") == 0, "prod manifest fields");

    // A tampered payload (one byte flipped after signing) fails the signature.
    char *txt = slurp("fixtures/valid.json", NULL);
    cJSON *jd = cJSON_Parse(txt);
    char *p = strdup(cJSON_GetObjectItem(jd, "payload")->valuestring);
    p[10] = p[10] == 'A' ? 'B' : 'A';
    CHECK(ota_verify_descriptor(p, cJSON_GetObjectItem(jd, "signature")->valuestring, "esp32_client", "1.3.0", &m) == OTA_VERIFY_BAD_SIGNATURE, "tampered payload");
    free(p); cJSON_Delete(jd); free(txt);

    uint8_t out[8];
    CHECK(ota_b64url_decode("Zm9v", out, sizeof out) == 3 && memcmp(out, "foo", 3) == 0, "b64url foo");
    CHECK(ota_b64url_decode("a+b", out, sizeof out) == -1, "b64url rejects '+'");

    printf(failures ? "%d FAILED\n" : "all OTA host tests passed\n", failures);
    return failures ? 1 : 0;
}
