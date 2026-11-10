// CYW43 station connectivity, identity, and .local name resolution.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lwip/ip_addr.h"

bool wifi_connect(void);
void wifi_off(void);
bool wifi_connected(void);
int wifi_rssi(void);
const char *wifi_ip(void);
const char *wifi_mac(void);      // "aa:bb:cc:dd:ee:ff"
bool wifi_on_usb(void);          // VBUS present (needs the CYW43 up)

// Resolve a host: literal IP, DNS, or a one-shot mDNS query for *.local.
bool wifi_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms);

// Base URL of the server with any .local host replaced by its resolved IP,
// or "" if it cannot be resolved. Cached for the wake.
const char *wifi_server_base(void);
// The IP the server's .local name resolved to (or the fallback used for it)
// this wake; "" for a non-.local host or before wifi_server_base().
const char *wifi_server_ip(void);
// If `url` names the configured .local host, rewrite it to the resolved IP.
void wifi_rewrite_host(const char *url, char *out, size_t cap);
