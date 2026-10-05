#pragma once

#include "esp_err.h"

typedef enum {
    WIFI_CONNECTING,
    WIFI_SETUP_AP,
    WIFI_CONNECTED,
} wifi_state_t;

void wifi_init(void);
void wifi_poll(void);               // call from the main loop
wifi_state_t wifi_state(void);
typedef struct {
    char ssid[33];
    int rssi;       // signal strength in dBm
} wifi_network_t;

// Looks for networks in range, takes a few seconds. Fills in the strongest
// ones, strongest first, and returns how many, or -1 if the scan failed.
int wifi_scan(wifi_network_t *networks, int max_count);

// Stores the credentials, they take effect after a reboot.
esp_err_t wifi_set_credentials(const char *ssid, const char *pass);
