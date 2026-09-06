#include "wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"

#include "config.h"
#include "log.h"

static bool s_inited = false;
static bool s_connected = false;
static char s_ip[16] = "0.0.0.0";
static char s_mac[18] = "00:00:00:00:00:00";
static char s_base[200] = "";
static char s_local_host[64] = "";      // the configured .local host, if any
static char s_resolved[16] = "";        // its IP once resolved

bool wifi_connect(void)
{
    if (!s_inited) {
        if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE) != 0) {
            LOGE("wifi: cyw43 init failed");
            return false;
        }
        s_inited = true;
    }
    cyw43_arch_enable_sta_mode();
    netif_set_hostname(netif_default, WIFI_HOSTNAME);

    uint8_t mac[6] = {0};
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    for (int a = 1; a <= WIFI_JOIN_ATTEMPTS; a++) {
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        int r = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, WIFI_CONNECT_TIMEOUT_MS);
        if (r == 0) {
            const ip4_addr_t *ip = netif_ip4_addr(netif_default);
            snprintf(s_ip, sizeof s_ip, "%s", ip ? ip4addr_ntoa(ip) : "0.0.0.0");
            s_connected = true;
            LOGI("wifi: connected to '%s', ip=%s rssi=%d (%lu ms)", WIFI_SSID, s_ip, wifi_rssi(),
                 (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
            return true;
        }
        LOGW("wifi: join attempt %d/%d failed (%d)", a, WIFI_JOIN_ATTEMPTS, r);
        if (a < WIFI_JOIN_ATTEMPTS) sleep_ms(1000);
    }
    return false;
}

void wifi_off(void)
{
    if (s_inited) {
        cyw43_arch_deinit();
        s_inited = false;
    }
    s_connected = false;
}

bool wifi_connected(void) { return s_connected; }

int wifi_rssi(void)
{
    int32_t rssi = 0;
    if (!s_inited || cyw43_wifi_get_rssi(&cyw43_state, &rssi) != 0) return 0;
    return (int)rssi;
}

const char *wifi_ip(void) { return s_ip; }
const char *wifi_mac(void) { return s_mac; }

bool wifi_on_usb(void)
{
    if (!s_inited) return false;
    return cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);
}

// ---- DNS ----
static volatile int s_dns_state;   // 0 pending, 1 ok, 2 fail
static ip_addr_t s_dns_ip;

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name; (void)arg;
    if (ipaddr) { s_dns_ip = *ipaddr; s_dns_state = 1; } else s_dns_state = 2;
}

static bool resolve_dns(const char *host, ip_addr_t *out, uint32_t timeout_ms)
{
    s_dns_state = 0;
    cyw43_arch_lwip_begin();
    err_t e = dns_gethostbyname(host, &s_dns_ip, dns_cb, NULL);
    cyw43_arch_lwip_end();
    if (e == ERR_OK) { *out = s_dns_ip; return true; }
    if (e != ERR_INPROGRESS) return false;
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (s_dns_state == 0 && !time_reached(deadline)) sleep_ms(10);
    if (s_dns_state != 1) return false;
    *out = s_dns_ip;
    return true;
}

// ---- one-shot mDNS A query ----
// Sends a query with the QU (unicast-response) bit so the responder answers
// us directly and no multicast group membership is needed.
static volatile bool s_mdns_got;
static ip_addr_t s_mdns_ip;
static char s_mdns_name[64];

static bool mdns_name_matches(const uint8_t *msg, size_t len, size_t off, const char *want, size_t *end)
{
    // Decode a possibly-compressed DNS name at off and compare to want.
    char name[96]; size_t n = 0; int jumps = 0; size_t o = off; bool jumped = false;
    while (o < len) {
        uint8_t l = msg[o];
        if (l == 0) { o++; break; }
        if ((l & 0xC0) == 0xC0) {
            if (o + 1 >= len || ++jumps > 5) return false;
            size_t ptr = ((l & 0x3F) << 8) | msg[o + 1];
            if (!jumped) *end = o + 2;
            jumped = true; o = ptr; continue;
        }
        o++;
        if (o + l > len || n + l + 1 >= sizeof name) return false;
        if (n) name[n++] = '.';
        memcpy(name + n, msg + o, l); n += l; o += l;
    }
    name[n] = 0;
    if (!jumped) *end = o;
    return strcasecmp(name, want) == 0;
}

static void mdns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg; (void)pcb; (void)addr; (void)port;
    if (!p) return;
    uint8_t msg[512];
    size_t len = p->tot_len < sizeof msg ? p->tot_len : sizeof msg;
    pbuf_copy_partial(p, msg, (u16_t)len, 0);
    pbuf_free(p);
    if (len < 12) return;
    unsigned qd = (msg[4] << 8) | msg[5], an = (msg[6] << 8) | msg[7];
    size_t off = 12;
    // Skip questions.
    for (unsigned i = 0; i < qd && off < len; i++) {
        size_t end;
        if (!mdns_name_matches(msg, len, off, "", &end)) { /* name != "" always; end still set */ }
        off = end + 4;
    }
    for (unsigned i = 0; i < an && off + 10 <= len; i++) {
        size_t end;
        bool match = mdns_name_matches(msg, len, off, s_mdns_name, &end);
        off = end;
        unsigned type = (msg[off] << 8) | msg[off + 1];
        unsigned rdlen = (msg[off + 8] << 8) | msg[off + 9];
        off += 10;
        if (off + rdlen > len) break;
        if (match && type == 1 && rdlen == 4) {
            IP4_ADDR(&s_mdns_ip, msg[off], msg[off + 1], msg[off + 2], msg[off + 3]);
            s_mdns_got = true;
            return;
        }
        off += rdlen;
    }
}

static bool resolve_mdns(const char *host, ip_addr_t *out, uint32_t timeout_ms)
{
    snprintf(s_mdns_name, sizeof s_mdns_name, "%s", host);
    uint8_t q[128]; size_t n = 0;
    uint16_t id = (uint16_t)to_ms_since_boot(get_absolute_time());
    q[n++] = id >> 8; q[n++] = id & 0xFF;
    q[n++] = 0; q[n++] = 0;              // flags: standard query
    q[n++] = 0; q[n++] = 1;              // QDCOUNT
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63 || n + l + 6 > sizeof q) return false;
        q[n++] = (uint8_t)l; memcpy(q + n, p, l); n += l;
        p += l; if (*p == '.') p++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;              // QTYPE A
    q[n++] = 0x80; q[n++] = 1;           // QCLASS IN, QU bit

    ip_addr_t group; IP4_ADDR(&group, 224, 0, 0, 251);
    for (int attempt = 0; attempt < MDNS_QUERY_ATTEMPTS; attempt++) {
        s_mdns_got = false;
        cyw43_arch_lwip_begin();
        struct udp_pcb *pcb = udp_new();
        if (pcb) {
            udp_bind(pcb, IP_ADDR_ANY, 0);
            udp_recv(pcb, mdns_recv, NULL);
            struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
            if (pb) { memcpy(pb->payload, q, n); udp_sendto(pcb, pb, &group, 5353); pbuf_free(pb); }
        }
        cyw43_arch_lwip_end();
        if (!pcb) return false;
        absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
        while (!s_mdns_got && !time_reached(deadline)) sleep_ms(10);
        cyw43_arch_lwip_begin();
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        if (s_mdns_got) { *out = s_mdns_ip; return true; }
    }
    return false;
}

bool wifi_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms)
{
    if (ipaddr_aton(host, out)) return true;
    size_t hl = strlen(host);
    if (hl > 6 && strcasecmp(host + hl - 6, ".local") == 0) return resolve_mdns(host, out, timeout_ms);
    return resolve_dns(host, out, timeout_ms);
}

// Split "scheme://host[:port]/path".
static bool split_url(const char *url, char *scheme, size_t sc, char *host, size_t hc, char *port, size_t pc, const char **path)
{
    const char *p = strstr(url, "://");
    if (!p) return false;
    snprintf(scheme, sc, "%.*s://", (int)(p - url), url);
    p += 3;
    const char *slash = strchr(p, '/');
    size_t hostport_len = slash ? (size_t)(slash - p) : strlen(p);
    const char *colon = memchr(p, ':', hostport_len);
    size_t hl = colon ? (size_t)(colon - p) : hostport_len;
    if (hl == 0 || hl >= hc) return false;
    memcpy(host, p, hl); host[hl] = 0;
    if (colon) snprintf(port, pc, "%.*s", (int)(hostport_len - hl), colon); else port[0] = 0;
    *path = slash ? slash : "";
    return true;
}

const char *wifi_server_base(void)
{
    if (s_base[0]) return s_base;
    char scheme[12], host[64], port[8]; const char *path;
    if (!split_url(TESSERAE_SERVER_URL, scheme, sizeof scheme, host, sizeof host, port, sizeof port, &path)) {
        LOGE("bad TESSERAE_SERVER_URL");
        return "";
    }
    size_t hl = strlen(host);
    const char *use_host = host;
    if (hl > 6 && strcasecmp(host + hl - 6, ".local") == 0) {
        snprintf(s_local_host, sizeof s_local_host, "%s", host);
        ip_addr_t ip;
        if (wifi_resolve(host, &ip, MDNS_QUERY_TIMEOUT_MS)) {
            snprintf(s_resolved, sizeof s_resolved, "%s", ipaddr_ntoa(&ip));
            LOGI("mdns: %s -> %s", host, s_resolved);
        } else if (strlen(TESSERAE_FALLBACK_IP) > 0) {
            snprintf(s_resolved, sizeof s_resolved, "%s", TESSERAE_FALLBACK_IP);
            LOGW("mdns: %s did not resolve; using fallback %s", host, s_resolved);
        } else {
            LOGE("mdns: %s did not resolve and no fallback IP is set", host);
            return "";
        }
        use_host = s_resolved;
    }
    snprintf(s_base, sizeof s_base, "%s%s%s%s", scheme, use_host, port, path);
    size_t l = strlen(s_base);
    while (l && s_base[l - 1] == '/') s_base[--l] = 0;
    return s_base;
}

void wifi_rewrite_host(const char *url, char *out, size_t cap)
{
    char scheme[12], host[64], port[8]; const char *path;
    if (s_local_host[0] && s_resolved[0] &&
        split_url(url, scheme, sizeof scheme, host, sizeof host, port, sizeof port, &path) &&
        strcasecmp(host, s_local_host) == 0) {
        snprintf(out, cap, "%s%s%s%s", scheme, s_resolved, port, path);
        return;
    }
    snprintf(out, cap, "%s", url);
}
