#pragma once

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

// Saved networks: wifi_saved() gives the name in place idx (0..WIFI_NETS-1),
// "" for a free place. wifi_add() saves a network, or the new password of a
// saved one; without a connection the board tries it at once. A removed
// network that is in use stays connected until the link drops. Both return
// NULL, or what is wrong: with the request, or wifi_failed when the board
// could not store it.
const char *wifi_saved(int idx);
const char *wifi_add(const char *ssid, const char *pass);
const char *wifi_remove(const char *ssid);
extern const char wifi_failed[];

// The board's name on the network: http://<name>.local/. A new one is
// answered to at once and kept over reboots. Returns like wifi_add().
const char *wifi_name(void);
const char *wifi_name_set(const char *new_name);
