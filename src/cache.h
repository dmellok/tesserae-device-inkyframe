// Offline decks and albums on the microSD card (Tesserae's deck_cache and
// frame_cache, schema 1, as the ESP32 firmware and the Pi client implement
// them). The pure logic is deck.c and frame_collection.c, ported unchanged
// from tesserae-device-firmware with their tests.
//
// Deck: a small graph of pre-rendered pages. A button press the cached deck
// can answer paints from the card with no network at all, and the page shown
// is reported on the next check-in (never as a button press, which would
// double-count it). Album: frames painted from the card on their own
// interval, even while the server is down.
//
// On the card: /tesserae/deck/ and /tesserae/album/ (manifest.json plus one
// <digest>.bin per frame) and /tesserae/state.json (deck page, album position
// and deadline), so playback survives the frame powering off on battery.
// Every cached frame is verified against its manifest digest and length
// before it is painted; anything that fails is a miss and the network is used.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vendor/cJSON.h"

// Mount the card and load what is cached. Call once at boot.
void cache_begin(void);
bool cache_enabled(void);

// Capability and state blocks for the /status body.
void cache_add_status(cJSON *body);
// "&deck_page_id=..." for the frame GET once a page was served locally, or "".
const char *cache_frame_query(void);

// The status response: version announcements for the bound deck / album
// ("" when absent, which unbinds). Syncs whatever moved (Wi-Fi up).
void cache_note_status(const char *deck_version, bool has_deck,
                       const char *album_version, bool has_album,
                       const char *base, const char *device_id, const char *token);

// A button press: paint the linked page from the card if the deck has one.
// True when it was served locally (do not report the press).
bool cache_local_nav(const char *button);

// A network frame was painted over the album: it resumes at its existing
// deadline rather than being pushed back.
void cache_note_network_frame(uint32_t now);

// The album frame is due: paint it from the card. True when painted.
bool cache_album_due(uint32_t now);
// Seconds until the album's next frame (or -1 with no album).
int32_t cache_album_wait_s(uint32_t now);

// Write the state file if anything changed.
void cache_commit(void);
