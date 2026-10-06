// WireGuard VPN client: one tunnel to one peer, so the controller can be
// reached from outside the home network. The settings are a normal WireGuard
// client config (the text wg-quick uses), stored in NVS. A new config takes
// effect at once, the board does not reboot for it.
//
// Used from the config:
//   [Interface]  PrivateKey, Address (IPv4)
//   [Peer]       PublicKey, PresharedKey, Endpoint, PersistentKeepalive
// AllowedIPs is not used. The tunnel carries what is sent to our tunnel
// address and the answers to it, everything else goes over WiFi as before.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "wireguard-platform.h"     // lwIP-level API of the esp_wireguard component
#include "wireguard.h"
#include "wireguardif.h"
#include "config.h"
#include "vpn.h"
#include "wifi.h"

#define KEY_LEN     32
#define HOST_CHARS  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"

// The tunnel interface keeps its data in the lwIP state pointer, where
// esp_netif normally expects its own. Without this the board crashes when the
// tunnel gets its address, on every boot.
_Static_assert(LWIP_ESP_NETIF_DATA, "needs CONFIG_ESP_NETIF_BRIDGE_EN=y: delete sdkconfig so that sdkconfig.defaults applies");
// tunnel_remove() goes deep into lwIP, see sdkconfig.defaults.
_Static_assert(CONFIG_LWIP_TCPIP_TASK_STACK_SIZE >= 4096, "needs CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096: delete sdkconfig so that sdkconfig.defaults applies");

typedef struct {
    char private_key[48], public_key[48];   // base64, as in the config text
    uint8_t preshared_key[KEY_LEN];
    bool has_preshared_key;
    char host[80];
    uint16_t port, keepalive;
    ip4_addr_t address, netmask;
    int prefix;                             // netmask as a bit count
} vpn_config_t;

static const char *TAG = "vpn";
static vpn_config_t config;         // the config in use, only vpn_task() changes it
static bool configured;             // there is one
static char tunnel_texts[2][120];   // the one not shown is written, then they change places
static const char *volatile tunnel_text = "";
static const char *volatile state = "off";
static volatile bool reload;        // another config was stored, vpn_task() switches to it

// Owned by the TCP/IP task, see tunnel_connect().
static struct netif tunnel;
static bool tunnel_added;
static uint8_t peer_idx = WIREGUARDIF_INVALID_INDEX;

// Copies a value of the "name = value" lines of a WireGuard config to value.
// A value can be a list (a, b) and a name can come on several lines: the first
// entry is taken, with ipv4 set the first one that is not an IPv6 address.
// Returns its length, 0 if there is none, -1 if it does not fit.
static int conf_get(const char *conf_text, const char *name, bool ipv4, char *value, size_t size)
{
    size_t name_len = strlen(name);

    for (const char *line = conf_text; line; line = strchr(line, '\n')) {
        line += strspn(line, " \t\r\n");
        if (strncasecmp(line, name, name_len) != 0) {
            continue;
        }
        const char *p = line + name_len;
        p += strspn(p, " \t");
        if (*p++ != '=') {
            continue;       // a longer name that starts the same
        }
        for (;;) {
            p += strspn(p, " \t,");
            size_t len = strcspn(p, " \t\r\n,#");
            if (len == 0) {
                break;      // end of the line
            }
            if (!ipv4 || !memchr(p, ':', len)) {
                if (len >= size) {
                    return -1;
                }
                memcpy(value, p, len);
                value[len] = 0;
                return len;
            }
            p += len;
        }
    }
    return 0;
}

// Decodes a base64 WireGuard key, 32 bytes. Done by the library, so that it
// reads the keys later the same way.
static bool key_decode(const char *base64, uint8_t *key)
{
    size_t len = KEY_LEN;

    return wireguard_base64_decode(base64, key, &len) && len == KEY_LEN;
}

// True if base64 can be a private or public key. The library takes no key of
// all zero bits for these; other keys it cannot use are only found when the
// tunnel is created.
static bool key_usable(const char *base64)
{
    static const uint8_t zero[KEY_LEN];
    uint8_t key[KEY_LEN];
    bool ok = key_decode(base64, key) && memcmp(key, zero, KEY_LEN) != 0;

    mbedtls_platform_zeroize(key, sizeof(key));
    return ok;
}

// Fills in parsed from the config text. Returns NULL, or what is wrong.
static const char *config_parse(const char *conf_text, vpn_config_t *parsed)
{
    char value[80], *end;
    int len, peers = 0;

    memset(parsed, 0, sizeof(*parsed));
    // The lines are looked up by name only, those of two peers would get mixed.
    for (const char *line = conf_text; line; line = strchr(line, '\n')) {
        line += strspn(line, " \t\r\n");
        peers += strncasecmp(line, "[Peer]", 6) == 0;
    }
    if (peers > 1) {
        return "only one [Peer] is supported";
    }
    if (conf_get(conf_text, "PrivateKey", false, parsed->private_key, sizeof(parsed->private_key)) <= 0
            || !key_usable(parsed->private_key)) {
        return "PrivateKey is missing or not a WireGuard key";
    }
    if (conf_get(conf_text, "PublicKey", false, parsed->public_key, sizeof(parsed->public_key)) <= 0
            || !key_usable(parsed->public_key)) {
        return "PublicKey is missing or not a WireGuard key";
    }
    char preshared[48];     // a buffer of its own, so that it can be wiped
    len = conf_get(conf_text, "PresharedKey", false, preshared, sizeof(preshared));
    parsed->has_preshared_key = len > 0 && key_decode(preshared, parsed->preshared_key);
    mbedtls_platform_zeroize(preshared, sizeof(preshared));
    if (len != 0 && !parsed->has_preshared_key) {
        return "PresharedKey is not a WireGuard key";
    }

    // Address = 10.0.0.2/24, without the /24 it is a single address like in wg-quick
    long prefix = 32;
    len = conf_get(conf_text, "Address", true, value, sizeof(value));
    char *slash = len > 0 ? strchr(value, '/') : NULL;
    if (slash) {
        *slash++ = 0;
        prefix = strtol(slash, &end, 10);
        if (end == slash || *end) {
            prefix = 0;
        }
    }
    if (len <= 0 || !ip4addr_aton(value, &parsed->address) || ip4_addr_isany_val(parsed->address)
            || prefix < 1 || prefix > 32) {
        return "Address must be IPv4 with /1 to /32, like 10.0.0.2/24";
    }
    parsed->prefix = prefix;
    parsed->netmask.addr = lwip_htonl((uint32_t)0xffffffff << (32 - prefix));

    // Endpoint = name-or-IPv4:port
    len = conf_get(conf_text, "Endpoint", false, value, sizeof(value));
    char *colon = len > 0 ? strrchr(value, ':') : NULL;
    long port = colon ? strtol(colon + 1, &end, 10) : 0;
    if (!colon || colon == value || value + strspn(value, HOST_CHARS) != colon
            || end == colon + 1 || *end || port < 1 || port > 65535) {
        return "Endpoint must be name:port or IPv4:port, 79 characters at most";
    }
    *colon = 0;
    strlcpy(parsed->host, value, sizeof(parsed->host));
    parsed->port = port;

    // Without it nothing is sent while idle, and a router on the way forgets
    // us. Then nobody can reach the controller, so it is on by default.
    parsed->keepalive = VPN_KEEPALIVE_S;
    len = conf_get(conf_text, "PersistentKeepalive", false, value, sizeof(value));
    if (len != 0) {
        long keepalive = -1;
        if (len > 0 && strcasecmp(value, "off") == 0) {
            keepalive = 0;
        } else if (len > 0) {
            keepalive = strtol(value, &end, 10);
            if (end == value || *end) {
                keepalive = -1;
            }
        }
        // The library subtracts it from the 180 s a session lasts; from 171 s
        // on that goes wrong and it would start a new handshake every 5 s.
        if (keepalive < 0 || keepalive > VPN_KEEPALIVE_MAX_S) {
            return "PersistentKeepalive must be 0 to " STR(VPN_KEEPALIVE_MAX_S) " seconds or off";
        }
        parsed->keepalive = keepalive;
    }
    return NULL;
}

static bool in_tunnel(const vpn_config_t *checked, uint32_t addr)
{
    return addr && ((addr ^ checked->address.addr) & checked->netmask.addr) == 0;
}

// True if the tunnel network contains the board's own WiFi address, its
// router, a DNS server it uses or the setup AP. lwIP would then send into the
// tunnel what the board itself sends there, like its DNS and time requests.
static bool address_clash(const vpn_config_t *checked)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ap_ip = { 0 }, sta_ip = { 0 };

    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ap_ip);
    esp_netif_get_ip_info(sta, &sta_ip);
    if (in_tunnel(checked, ap_ip.ip.addr) || in_tunnel(checked, sta_ip.ip.addr) || in_tunnel(checked, sta_ip.gw.addr)) {
        return true;
    }
    // Only of the station: what the AP has there is not a server the board uses.
    for (int dns_idx = 0; dns_idx < ESP_NETIF_DNS_MAX; dns_idx++) {
        esp_netif_dns_info_t dns = { 0 };
        if (esp_netif_get_dns_info(sta, dns_idx, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4
                && in_tunnel(checked, dns.ip.u_addr.ip4.addr)) {
            return true;
        }
    }
    return false;
}

static const char clash_text[] = "the Address network contains the board's WiFi address, router, DNS server or setup AP (192.168.4.1)";
static const char wifi_text[] = "waiting for WiFi";
const char vpn_store_failed[] = "cannot store the config";

const char *vpn_config_save(const char *conf_text)
{
    nvs_handle_t nvs;

    if (conf_text) {
        vpn_config_t parsed;
        const char *problem = config_parse(conf_text, &parsed);

        // Checked again when connecting, the WiFi network may be another one by then.
        if (!problem && address_clash(&parsed)) {
            problem = clash_text;
        }
        mbedtls_platform_zeroize(&parsed, sizeof(parsed));
        if (problem) {
            return problem;
        }
    }
    if (nvs_open("vpn", NVS_READWRITE, &nvs) != ESP_OK) {
        return vpn_store_failed;
    }
    esp_err_t err = conf_text ? nvs_set_str(nvs, "conf", conf_text) : nvs_erase_key(nvs, "conf");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;       // nothing to remove
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return vpn_store_failed;
    }
    reload = true;
    return NULL;
}

// Reads the stored config into config and sets configured. Returns what is
// wrong with the stored text, NULL if it is fine or there is none.
static const char *config_load(void)
{
    static char conf_text[VPN_CONF_MAX];    // only vpn_task() comes here
    size_t size = sizeof(conf_text);
    nvs_handle_t nvs;
    const char *problem = NULL;

    configured = false;
    tunnel_text = "";
    mbedtls_platform_zeroize(&config, sizeof(config));      // the keys of the config before
    if (nvs_open("vpn", NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_str(nvs, "conf", conf_text, &size) == ESP_OK) {
            problem = config_parse(conf_text, &config);
            configured = !problem;
        }
        nvs_close(nvs);
    }
    mbedtls_platform_zeroize(conf_text, sizeof(conf_text));     // it may have held the private key
    if (!configured) {
        mbedtls_platform_zeroize(&config, sizeof(config));  // and so may what was parsed before the problem
    }
    if (configured) {
        static int shown;
        char *text = tunnel_texts[shown ^= 1];

        snprintf(text, sizeof(tunnel_texts[0]), IPSTR "/%d via %s:%d", IP2STR(&config.address),
                 config.prefix, config.host, config.port);
        tunnel_text = text;
    }
    return problem;
}

static bool endpoint_resolve(ip_addr_t *endpoint_ip)
{
    struct addrinfo hints = { .ai_family = AF_INET }, *found = NULL;

    if (getaddrinfo(config.host, NULL, &hints, &found) != 0 || !found) {
        return false;
    }
    ip_addr_set_ip4_u32(endpoint_ip, ((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(found);
    return true;
}

// The three tunnel_ functions run in the TCP/IP task, through
// esp_netif_tcpip_exec(): lwIP may only be called from there.

// Takes the tunnel interface away again. The interface goes first: that
// closes the connections that came in through it, and the peer is still there
// to be told so.
static esp_err_t tunnel_remove(void *arg)
{
    if (tunnel_added) {
        netif_remove(&tunnel);
        if (peer_idx != WIREGUARDIF_INVALID_INDEX) {
            wireguardif_remove_peer(&tunnel, peer_idx);
            peer_idx = WIREGUARDIF_INVALID_INDEX;
        }
        wireguardif_shutdown(&tunnel);
        mbedtls_platform_zeroize(tunnel.state, sizeof(struct wireguard_device));    // it has the private key
        wireguardif_fini(&tunnel);
        tunnel_added = false;
    }
    return ESP_OK;
}

// Creates the tunnel interface on first use and (re)starts connecting to the
// peer at endpoint_ip.
static esp_err_t tunnel_connect(void *endpoint_ip)
{
    if (!tunnel_added) {
        struct wireguardif_init_data init = { .private_key = config.private_key };
        ip4_addr_t no_gateway = { 0 };

        if (!netif_add(&tunnel, &config.address, &config.netmask, &no_gateway, &init, wireguardif_init, ip_input)) {
            return ESP_FAIL;
        }
        netif_set_up(&tunnel);
        tunnel_added = true;
        // The link is down until the first handshake. If lwIP does not keep a
        // packet from the tunnel address on the tunnel now, our routing hook
        // below is not in use and such packets could leave unencrypted.
        if (ip4_route_src(netif_ip4_addr(&tunnel), netif_ip4_addr(&tunnel)) != &tunnel) {
            tunnel_remove(NULL);
            return ESP_FAIL;
        }
    }
    if (peer_idx == WIREGUARDIF_INVALID_INDEX) {
        struct wireguardif_peer peer;

        wireguardif_peer_init(&peer);   // allows any address through the tunnel
        peer.public_key = config.public_key;
        peer.preshared_key = config.has_preshared_key ? config.preshared_key : NULL;
        peer.keep_alive = config.keepalive;
        if (wireguardif_add_peer(&tunnel, &peer, &peer_idx) != ERR_OK) {
            peer_idx = WIREGUARDIF_INVALID_INDEX;
            return ESP_FAIL;
        }
    }
    if (wireguardif_update_endpoint(&tunnel, peer_idx, endpoint_ip, config.port) != ERR_OK
            || wireguardif_connect(&tunnel, peer_idx) != ERR_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Up means: keys from a handshake that is at most 3 minutes old. Not
// wireguardif_peer_is_up(), it also counts the keys of the session before, and
// those are never dropped, so after the first key renewal it says up forever.
static esp_err_t tunnel_is_up(void *arg)
{
    struct wireguard_device *device = tunnel.state;

    return peer_idx != WIREGUARDIF_INVALID_INDEX && device->peers[peer_idx].curr_keypair.valid ? ESP_OK : ESP_FAIL;
}

// lwIP asks this hook where a packet goes; main/CMakeLists.txt puts ours in
// front of the one of ESP-IDF. While the tunnel has no session its link is
// down, and lwIP would send a packet from our tunnel address out over WiFi
// without encryption. Keep those on the tunnel, which drops them. The same
// for a packet to the tunnel network that has no source address yet, unless
// the WiFi network covers that address too: then lwIP does not ask.
struct netif *__real_ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest);

struct netif *__wrap_ip4_route_src_hook(const ip4_addr_t *src, const ip4_addr_t *dest)
{
    if (tunnel_added && (src ? ip4_addr_cmp(src, netif_ip4_addr(&tunnel))
                             : ip4_addr_netcmp(dest, netif_ip4_addr(&tunnel), netif_ip4_netmask(&tunnel)))) {
        return &tunnel;
    }
    return __real_ip4_route_src_hook(src, dest);
}

// Waits, in steps of a second so that a new config is noticed soon. Returns
// false when there is one.
static bool wait_ms(int ms)
{
    for (; ms > 0 && !reload; ms -= 1000) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return !reload;
}

static void vpn_task(void *arg)
{
    bool random_ready = false, sntp_ready = false, time_ready = false;

    for (;;) {      // once for every config
        if (reload) {
            vTaskDelay(pdMS_TO_TICKS(1000));    // let the answer to the request that stored it go out first
        }
        reload = false;
        esp_netif_tcpip_exec(tunnel_remove, NULL);
        const char *problem = config_load();
        if (problem) {
            ESP_LOGE(TAG, "stored config: %s", problem);
        }
        state = problem ? problem : configured ? wifi_text : "off";
        while (!configured && wait_ms(1000)) {
        }
        while (wifi_state() != WIFI_CONNECTED && wait_ms(1000)) {
        }
        if (reload) {
            continue;
        }
        // The random numbers for the handshakes, and the time. A failure here
        // must not take the fans down with it, so no ESP_ERROR_CHECK.
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
        random_ready = random_ready || wireguard_platform_init() == ESP_OK;
        sntp_ready = sntp_ready || esp_netif_sntp_init(&sntp) == ESP_OK;
        if (!random_ready || !sntp_ready) {
            ESP_LOGE(TAG, "cannot start");
            state = "cannot start";
            wait_ms(VPN_RETRY_MS);
            continue;
        }
        // Every handshake carries the time, and the peer refuses one that is
        // not newer than the last it saw from us. So the clock has to be right
        // before the first one: wait for an answer from the time server. A
        // clock that only looks set will not do, after a reboot it runs on
        // from an inaccurate oscillator and may be ahead.
        while (!time_ready && !reload) {
            state = "waiting for the time from " NTP_SERVER;
            time_ready = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) == ESP_OK;
        }

        while (!reload) {
            ip_addr_t endpoint_ip;

            problem = wifi_state() != WIFI_CONNECTED ? wifi_text
                    : address_clash(&config) ? clash_text
                    : !endpoint_resolve(&endpoint_ip) ? "cannot find the Endpoint address"
                    : esp_netif_tcpip_exec(tunnel_connect, &endpoint_ip) != ESP_OK ? "cannot create the tunnel"
                    : NULL;
            if (problem == clash_text) {
                esp_netif_tcpip_exec(tunnel_remove, NULL);  // it may be there from before the WiFi network changed
            }
            if (problem && state != problem) {
                ESP_LOGW(TAG, "%s", problem);
            }
            // Watch the tunnel, WireGuard keeps it alive by itself. After a
            // problem try again in VPN_RETRY_MS. Without one, look the name up
            // again when the tunnel stays down for VPN_RESOLVE_MS: the peer
            // may have moved to a new address.
            int limit_ms = problem ? VPN_RETRY_MS : VPN_RESOLVE_MS;
            for (int down_ms = 0, seconds = 1; down_ms < limit_ms && !reload; down_ms += 1000, seconds++) {
                bool wifi_up = wifi_state() == WIFI_CONNECTED;

                if (wifi_up == (problem == wifi_text)) {
                    break;      // WiFi went away or came back: look at it again now
                }
                if (seconds % 30 == 0 && problem != clash_text && address_clash(&config)) {
                    break;      // the WiFi network changed under the tunnel
                }
                if (wifi_up && esp_netif_tcpip_exec(tunnel_is_up, NULL) == ESP_OK) {
                    state = "up";
                    down_ms = 0;
                } else {
                    state = problem ? problem : "connecting";
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void vpn_start(void)
{
    if (xTaskCreatePinnedToCore(vpn_task, "vpn", 4096, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "cannot start");
        state = "cannot start";
    }
}

const char *vpn_state(void)
{
    return state;
}

const char *vpn_tunnel(void)
{
    return tunnel_text;
}
