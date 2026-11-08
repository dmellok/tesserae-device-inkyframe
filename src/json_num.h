// Unsigned integers into cJSON as raw digits. cJSON prints any number past
// INT_MAX with "%g", which this firmware's printf does not format, so a
// 32-bit id or a card's free space would otherwise break the whole document.
#pragma once

#include <stdint.h>
#include "vendor/cJSON.h"

static inline void json_add_u64(cJSON *obj, const char *key, uint64_t v)
{
    char tmp[24], out[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    cJSON_AddRawToObject(obj, key, out);
}
