// WiFi station with a fallback setup access point.
// The credentials live in the WiFi driver's own NVS storage.

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "config.h"
#include "mtimer.h"
#include "wifi.h"

static const char *TAG = "wifi";
_Static_assert(sizeof(AP_PASS) > 8, "AP_PASS needs at least 8 characters");
static volatile bool connected;
static volatile bool has_ssid;
static bool ap_on;
static mtimer_t retry_timer, ap_timer;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT) {
        ESP_LOGI(TAG, "connected, http://" IPSTR "/", IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
        connected = true;
    } else {
        // Link dropped: retry right away, wifi_poll() keeps trying after that.
        if (connected && has_ssid) {
            esp_wifi_connect();
        }
        connected = false;
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
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_set_hostname(esp_netif_create_default_wifi_sta(), HOSTNAME);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    wifi_config_t sta;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sta));
    has_ssid = sta.sta.ssid[0] != 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (!has_ssid) {
        ap_start();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    mtimer_timeout_set(&ap_timer, WIFI_AP_AFTER_MS);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(HOSTNAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

// Keep the station connected. After WIFI_AP_AFTER_MS without a connection
// the setup AP comes up so the credentials can be fixed, and goes away once
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
    if (!mtimer_timeout(&retry_timer)) {
        return;
    }
    mtimer_timeout_set(&retry_timer, WIFI_RETRY_MS);

    if (!ap_on && (!has_ssid || mtimer_timeout(&ap_timer))) {
        ap_start();
    }
    if (has_ssid) {
        // A connect attempt scans all channels and stalls the setup AP,
        // so try less often while it is up.
        if (ap_on) {
            mtimer_timeout_set(&retry_timer, WIFI_AP_RETRY_MS);
        }
        esp_wifi_connect();
    }
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

esp_err_t wifi_set_credentials(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {
        .sta.scan_method = WIFI_ALL_CHANNEL_SCAN,   // pick the strongest AP, not the first one heard
    };
    size_t ssid_len = strlen(ssid), pass_len = strlen(pass);

    // The fields are fixed size and need no terminating NUL when full.
    if (ssid_len == 0 || ssid_len > sizeof(cfg.sta.ssid) || pass_len > sizeof(cfg.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    memcpy(cfg.sta.password, pass, pass_len);

    // The config cannot be changed in the middle of a connect attempt, so
    // stop the reconnecting (has_ssid) and cancel an attempt in progress.
    // An established link stays up, the request may have come in over it.
    bool had_ssid = has_ssid;
    has_ssid = false;
    if (!connected) {
        esp_wifi_disconnect();
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        has_ssid = had_ssid;
    }
    return err;
}
