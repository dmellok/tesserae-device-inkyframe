#include "ota_verify.h"

#include <stdio.h>
#include <string.h>

#include "optional/monocypher-ed25519.h"
#include "vendor/cJSON.h"

typedef struct {
    const char *id;
    uint8_t key[32];
} trusted_key_t;

// Tesserae's production signer, the same key the ESP32 firmware and the Pi
// client trust. Public verification material, not a secret. Indexed by id so a
// prod-2 can overlap prod-1 during a rotation.
static const trusted_key_t TRUSTED_KEYS[] = {
    { "prod-1", { 0x97, 0xf3, 0x29, 0x4e, 0x7b, 0xf8, 0xf2, 0xdf, 0xb9, 0x65, 0xac, 0x27, 0x39, 0xe6, 0xd2, 0xd7,
                  0xee, 0x34, 0x6c, 0x42, 0x68, 0x1a, 0x94, 0xd8, 0x7c, 0x91, 0x89, 0x94, 0x2d, 0xb3, 0x96, 0x5e } },
#if defined(OTA_TRUST_TEST_KEY) && OTA_TRUST_TEST_KEY
    // tests/fixtures/ota in the Tesserae repo: published seed, test builds only.
    { "test-ed25519-1", { 0x03, 0xa1, 0x07, 0xbf, 0xf3, 0xce, 0x10, 0xbe, 0x1d, 0x70, 0xdd, 0x18, 0xe7, 0x4b, 0xc0, 0x99,
                          0x67, 0xe4, 0xd6, 0x30, 0x9b, 0xa5, 0x0d, 0x5f, 0x1d, 0xdc, 0x86, 0x64, 0x12, 0x55, 0x31, 0xb8 } },
#endif
};
#define N_KEYS (sizeof TRUSTED_KEYS / sizeof TRUSTED_KEYS[0])

const char *ota_verify_reason(ota_verify_result_t r)
{
    switch (r) {
    case OTA_VERIFY_OK: return "ok";
    case OTA_VERIFY_BAD_SIGNATURE: return "bad_signature";
    case OTA_VERIFY_MALFORMED_DESCRIPTOR: return "malformed_descriptor";
    case OTA_VERIFY_MALFORMED_MANIFEST: return "malformed_manifest";
    case OTA_VERIFY_SCHEMA_VERSION: return "schema_version";
    case OTA_VERIFY_KIND_MISMATCH: return "kind_mismatch";
    case OTA_VERIFY_ALREADY_CURRENT: return "already_current";
    }
    return "unknown";
}

static int b64u_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int ota_b64url_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;   // tolerate padding
        int v = b64u_value(*p);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    // A lone leftover sextet (length % 4 == 1) cannot be valid base64.
    if (bits >= 6) return -1;
    return (int)n;
}

static bool copy_str(const cJSON *o, const char *k, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring) >= cap) return false;
    snprintf(out, cap, "%s", v->valuestring);
    return true;
}

static bool is_lower_hex64(const char *s)
{
    if (strlen(s) != 64) return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

ota_verify_result_t ota_verify_descriptor(const char *payload_b64, const char *signature_b64,
                                          const char *kind, const char *running_fw,
                                          ota_manifest_t *out)
{
    memset(out, 0, sizeof *out);
    if (!payload_b64 || !signature_b64) return OTA_VERIFY_MALFORMED_DESCRIPTOR;

    static uint8_t payload[1536];
    uint8_t sig[64];
    int plen = ota_b64url_decode(payload_b64, payload, sizeof payload - 1);
    int slen = ota_b64url_decode(signature_b64, sig, sizeof sig);
    if (plen <= 0 || slen != 64) return OTA_VERIFY_MALFORMED_DESCRIPTOR;

    // 1. Signature over the raw bytes, before anything parses them.
    int verified_by = -1;
    for (size_t i = 0; i < N_KEYS && verified_by < 0; i++)
        if (crypto_ed25519_check(sig, TRUSTED_KEYS[i].key, payload, (size_t)plen) == 0) verified_by = (int)i;
    if (verified_by < 0) return OTA_VERIFY_BAD_SIGNATURE;

    // 2. Shape.
    payload[plen] = 0;
    cJSON *o = cJSON_ParseWithLength((const char *)payload, (size_t)plen);
    if (!cJSON_IsObject(o)) { cJSON_Delete(o); return OTA_VERIFY_MALFORMED_MANIFEST; }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(o, "schema_version");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(o, "size_bytes");
    bool ok = cJSON_IsNumber(schema) && cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble < 4294967296.0
           && copy_str(o, "key_id", out->key_id, sizeof out->key_id)
           && copy_str(o, "device_kind", out->device_kind, sizeof out->device_kind)
           && copy_str(o, "fw_version", out->fw_version, sizeof out->fw_version)
           && copy_str(o, "image_url", out->image_url, sizeof out->image_url)
           && copy_str(o, "sha256", out->sha256, sizeof out->sha256);
    if (ok) {
        out->schema_version = (int)schema->valuedouble;
        out->size_bytes = (uint32_t)size->valuedouble;
    }
    cJSON_Delete(o);
    if (!ok || !is_lower_hex64(out->sha256)) return OTA_VERIFY_MALFORMED_MANIFEST;
    // The key that verified must be the key the manifest names.
    if (strcmp(out->key_id, TRUSTED_KEYS[verified_by].id) != 0) return OTA_VERIFY_BAD_SIGNATURE;
    if (out->schema_version != OTA_SCHEMA_VERSION) return OTA_VERIFY_SCHEMA_VERSION;

    // 3. Target.
    if (!kind || strcmp(out->device_kind, kind) != 0) return OTA_VERIFY_KIND_MISMATCH;
    if (running_fw && strcmp(out->fw_version, running_fw) == 0) return OTA_VERIFY_ALREADY_CURRENT;
    return OTA_VERIFY_OK;
}
