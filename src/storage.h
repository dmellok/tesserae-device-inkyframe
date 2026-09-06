// Persistent device state in the last 4 KB flash sector. Records are appended
// within the sector and the sector is erased only when full, so a new frame
// every few minutes does not wear the flash. Commit only with the radio down.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void storage_begin(void);
// Write the in-RAM state to flash if anything changed. Interrupts are
// disabled for the few ms of the flash operation.
void storage_commit(void);

const char *storage_token(void);
void storage_set_token(const char *t);
const char *storage_device_id(void);        // server-assigned id, or DEVICE_ID
void storage_set_device_id(const char *id);
void storage_clear_pairing(void);           // token + etag; forces re-discovery

const char *storage_etag(void);
void storage_set_etag(const char *e);

uint32_t storage_next_event_id(void);

bool storage_fallback_kind(void);
void storage_set_fallback_kind(bool v);

int32_t storage_sleep_s(void);
void storage_set_sleep_s(int32_t s);
int32_t storage_button_wake_s(void);
void storage_set_button_wake_s(int32_t s);
bool storage_always_on(void);
void storage_set_always_on(bool v);
int32_t storage_awake_poll_s(void);
void storage_set_awake_poll_s(int32_t s);

uint8_t storage_backoff(void);
void storage_set_backoff(uint8_t s);
