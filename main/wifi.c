// WiFi station with a fallback setup access point.
// Up to WIFI_NETS networks are saved in NVS, the board joins the strongest
// one in range. Its name on the network (mDNS) is saved there too.

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "config.h"
#include "mtimer.h"
#include "wifi.h"

static const char *TAG = "wifi";
_Static_assert(sizeof(AP_PASS) > 8, "AP_PASS needs at least 8 characters");
_Static_assert(WIFI_NETS <= 8, "connect_best() keeps one bit per saved network in a byte");
const char wifi_failed[] = "cannot store the setting";

// Saved networks, a free place has an empty ssid. Changed from the HTTP task
// while wifi_poll() reads them; at worst that gives one failed connect attempt.
static struct {
    char ssid[33], pass[65];
} nets[WIFI_NETS];
static char name[33] = HOSTNAME;
static volatile bool connected;
static volatile bool added;         // a network was added, do not wait for the retry timer
static uint8_t tried;               // bit per saved network that did not connect since the last connection
static bool ap_on;
static mtimer_t retry_timer, ap_timer;

static esp_err_t save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs);

    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "nets", nets, sizeof(nets));
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, "name", name);
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return err;
}

static bool saved(void)
{
    for (int i = 0; i < WIFI_NETS; i++) {
        if (nets[i].ssid[0]) {
            return true;
        }
    }
    return false;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT) {
        ESP_LOGI(TAG, "connected, http://" IPSTR "/", IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
        connected = true;
        tried = 0;
    } else {
        connected = false;      // wifi_poll() looks for a network again
    }
}

// Joins the strongest saved network in range, at its strongest access point.
// One that did not connect is left out the next time, until all in range had
// their turn. Each saved network is asked for by name, which also finds a
// hidden one; that takes about 1 s for each.
static void connect_best(void)
{
    static wifi_ap_record_t seen[WIFI_NETS];    // primary (the channel) is 0 for one not in range
    int best = -1;

    // Ends an attempt in progress, and a link that got no address.
    esp_wifi_disconnect();
    for (int i = 0; i < WIFI_NETS; i++) {
        // Half the usual time on each channel, an access point answers to its name much faster.
        wifi_scan_config_t scan = { .ssid = (uint8_t *)nets[i].ssid, .show_hidden = true,
                                    .scan_time = { .active.max = 60, .passive = 120 } };
        uint16_t count = 1;     // the strongest one

        if (!nets[i].ssid[0] || esp_wifi_scan_start(&scan, true) != ESP_OK
                || esp_wifi_scan_get_ap_records(&count, &seen[i]) != ESP_OK || count == 0) {
            seen[i].primary = 0;
        }
    }
    for (int round = 0; round < 2 && best < 0; round++) {
        for (int i = 0; i < WIFI_NETS; i++) {
            if (seen[i].primary && !(tried & 1 << i) && (best < 0 || seen[i].rssi > seen[best].rssi)) {
                best = i;
            }
        }
        if (best < 0) {
            tried = 0;      // all in range had their turn, start over with the strongest
        }
    }
    if (best < 0) {
        return;
    }
    // The fields are fixed size and need no terminating NUL when full.
    wifi_config_t cfg = { .sta.channel = seen[best].primary, .sta.bssid_set = true };
    strncpy((char *)cfg.sta.ssid, nets[best].ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, nets[best].pass, sizeof(cfg.sta.password));
    memcpy(cfg.sta.bssid, seen[best].bssid, sizeof(cfg.sta.bssid));
    tried |= 1 << best;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        esp_wifi_connect();
    }
}

// Bring the setup AP up. On failure it is retried from the next wifi_poll().
static void ap_start(void)
{
    wifi_config_t ap = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 2,
        },
    };
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the setup AP");
        esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }
    ESP_LOGW(TAG, "setup AP \"%s\" is up, http://192.168.4.1/", AP_SSID);
    ap_on = true;
}

void wifi_init(void)
{
    nvs_handle_t nvs;
    bool first_time = true;     // no list stored yet
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(nets);
        first_time = nvs_get_blob(nvs, "nets", nets, &size) == ESP_ERR_NVS_NOT_FOUND;
        size = sizeof(name);
        nvs_get_str(nvs, "name", name, &size);
        nvs_close(nvs);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_set_hostname(esp_netif_create_default_wifi_sta(), name);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    if (first_time) {
        // A board from before the list: its one network is in the WiFi driver's storage.
        wifi_config_t sta;
        ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sta));
        memcpy(nets[0].ssid, sta.sta.ssid, sizeof(sta.sta.ssid));
        memcpy(nets[0].pass, sta.sta.password, sizeof(sta.sta.password));
        save();
    }
    // The list is the storage now, the driver need not write each network it tries to the flash.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (!saved()) {
        ap_start();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    mtimer_timeout_set(&ap_timer, WIFI_AP_AFTER_MS);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

// Keep the station connected. After WIFI_AP_AFTER_MS without a connection
// the setup AP comes up so a network can be added, and goes away once
// connected.
void wifi_poll(void)
{
    if (connected) {
        mtimer_timeout_set(&ap_timer, WIFI_AP_AFTER_MS);
        if (ap_on) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            ap_on = false;
        }
        return;
    }
    if (added) {
        // Try it soon, but let the answer to that request get out first:
        // looking for a network takes the radio away from the setup AP.
        added = false;
        mtimer_timeout_set(&retry_timer, WIFI_ADDED_MS);
    }
    if (!mtimer_timeout(&retry_timer)) {
        return;
    }
    if (!ap_on && (!saved() || mtimer_timeout(&ap_timer))) {
        ap_start();
    }
    if (saved()) {
        connect_best();
    }
    // Counted from here, the attempt needs the whole time to get its address.
    // Looking for the networks stalls the setup AP, so less often while it is up.
    mtimer_timeout_set(&retry_timer, ap_on ? WIFI_AP_RETRY_MS : WIFI_RETRY_MS);
}

wifi_state_t wifi_state(void)
{
    return connected ? WIFI_CONNECTED : ap_on ? WIFI_SETUP_AP : WIFI_CONNECTING;
}

int wifi_scan(wifi_network_t *networks, int max_count)
{
    static wifi_ap_record_t records[20];
    uint16_t record_count = sizeof(records) / sizeof(records[0]);
    int count = 0;

    // Fails while a connect attempt is in progress, the caller can try again.
    if (esp_wifi_scan_start(NULL, true) != ESP_OK
            || esp_wifi_scan_get_ap_records(&record_count, records) != ESP_OK) {
        return -1;
    }
    for (int i = 0; i < record_count && count < max_count; i++) {
        const char *ssid = (const char *)records[i].ssid;
        bool skip = ssid[0] == 0;       // hidden network
        // An SSID served by several access points is listed once.
        for (int j = 0; j < count && !skip; j++) {
            skip = strcmp(networks[j].ssid, ssid) == 0;
        }
        if (!skip) {
            strlcpy(networks[count].ssid, ssid, sizeof(networks[count].ssid));
            networks[count].rssi = records[i].rssi;
            count++;
        }
    }
    return count;
}

const char *wifi_saved(int net_idx)
{
    return nets[net_idx].ssid;
}

const char *wifi_add(const char *ssid, const char *password)
{
    int place = -1;

    if (!ssid[0] || strlen(ssid) >= sizeof(nets[0].ssid) || strlen(password) >= sizeof(nets[0].pass)) {
        return "ssid must be 1..32 and password 0..64 bytes";
    }
    // The place of the same name, else the first free one.
    for (int i = WIFI_NETS - 1; i >= 0 && (place < 0 || strcmp(nets[place].ssid, ssid) != 0); i--) {
        if (!nets[i].ssid[0] || strcmp(nets[i].ssid, ssid) == 0) {
            place = i;
        }
    }
    if (place < 0) {
        return STR(WIFI_NETS) " networks are saved, remove one first";
    }
    strcpy(nets[place].pass, password);
    strcpy(nets[place].ssid, ssid);
    tried = 0;
    added = true;
    return save() == ESP_OK ? NULL : wifi_failed;
}

const char *wifi_remove(const char *ssid)
{
    for (int i = 0; i < WIFI_NETS; i++) {
        if (ssid[0] && strcmp(nets[i].ssid, ssid) == 0) {
            memset(&nets[i], 0, sizeof(nets[i]));
            tried = 0;
            return save() == ESP_OK ? NULL : wifi_failed;
        }
    }
    return "no such saved network";
}

const char *wifi_name(void)
{
    return name;
}

const char *wifi_name_set(const char *new_name)
{
    size_t len = strlen(new_name);

    if (len == 0 || len >= sizeof(name) || new_name[strspn(new_name, "abcdefghijklmnopqrstuvwxyz0123456789-")]) {
        return "the name must be 1..32 of a-z, 0-9 and -";
    }
    strcpy(name, new_name);
    mdns_hostname_set(name);
    esp_netif_set_hostname(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), name);  // for the next DHCP request
    return save() == ESP_OK ? NULL : wifi_failed;
}
