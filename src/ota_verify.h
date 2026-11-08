// Signed OTA descriptor verification (Tesserae docs/ota/contract.md).
//
// Order and reason codes follow the contract: the Ed25519 signature is checked
// over the raw decoded payload BEFORE it is parsed, then the manifest shape,
// then the target (kind, not already running). Pure C with no device
// dependencies, so it builds and is tested on the host (tests/host).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_SCHEMA_VERSION 1

typedef enum {
    OTA_VERIFY_OK = 0,
    OTA_VERIFY_BAD_SIGNATURE,
    OTA_VERIFY_MALFORMED_DESCRIPTOR,
    OTA_VERIFY_MALFORMED_MANIFEST,
    OTA_VERIFY_SCHEMA_VERSION,
    OTA_VERIFY_KIND_MISMATCH,
    OTA_VERIFY_ALREADY_CURRENT,
} ota_verify_result_t;

typedef struct {
    int schema_version;
    char key_id[32];
    char device_kind[48];
    char fw_version[24];
    char image_url[320];
    uint32_t size_bytes;
    char sha256[65];   // lowercase hex
} ota_manifest_t;

// The contract's reason code ("bad_signature", ...; "ok" for OTA_VERIFY_OK).
const char *ota_verify_reason(ota_verify_result_t r);

// Verify a descriptor's base64url `payload` and `signature` against the
// built-in trusted keys, then check it targets `kind` and is not `running_fw`.
// `out` is filled whenever the manifest parsed (so a kind mismatch still names
// the target version for the report).
ota_verify_result_t ota_verify_descriptor(const char *payload_b64, const char *signature_b64,
                                          const char *kind, const char *running_fw,
                                          ota_manifest_t *out);

// base64url (no padding) decode. Returns the decoded length, or -1.
int ota_b64url_decode(const char *in, uint8_t *out, size_t cap);
