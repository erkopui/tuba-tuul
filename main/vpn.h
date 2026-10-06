#pragma once

#define VPN_CONF_MAX    1024    // longest WireGuard config text we take, with the ending NUL

// Starts the task that keeps the tunnel of the stored config up, if there is one.
void vpn_start(void);

// What the tunnel is doing, in words: "off", "connecting", "up", ...
const char *vpn_state(void);

// Tunnel address and peer of the config in use, like
// "10.0.0.2/24 via vpn.example.com:51820". Empty without a config.
const char *vpn_tunnel(void);

// Checks and stores a WireGuard client config; the tunnel switches over to it
// within a few seconds. NULL removes the config and the tunnel. Returns NULL
// when it is stored, else what is wrong: with the config, or
// vpn_store_failed when the board could not store it.
const char *vpn_config_save(const char *conf_text);
extern const char vpn_store_failed[];
