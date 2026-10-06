// All the knobs in one place.
#pragma once

// STR(FAN_COUNT) is "8", for putting a setting into a text
#define STR_(x) #x
#define STR(x) STR_(x)

#define FAN_COUNT       8       // LEDC has 8 channels, that is the maximum here
#define FAN_GPIOS       { 7, 8, 9, 10, 11, 12, 13, 14 }    // away from the antenna end of the board

#define PWM_HZ          25000
#define PWM_BITS        10      // 80 MHz / 25 kHz = 3200 steps, so 11 bits is the maximum
// Default: every GPIO is wired straight to the fan PWM pin and drives it
// high (3.3 V) and low. With an NPN transistor per channel set PWM_INVERT 1.
// PWM_OPEN_DRAIN 1 only pulls low and leaves the high level to the pull-up in
// the fan, use it only if that pull-up goes to 3.3 V at most.
#define PWM_INVERT      0
#define PWM_OPEN_DRAIN  0

// Time for a speed change from 0 to 100 %, smaller changes take proportionally
// less. 0 = change at once. This is the default, a request can bring its own.
#define FAN_FADE_MS     10000
#define FAN_FADE_MAX_MS 40000   // hardware limit at 25 kHz / 10 bits

#define MODBUS_PORT     502     // Modbus TCP: holding registers 0..FAN_COUNT-1 = fan speed in %

#define LED_GPIO        48      // green user LED on the LCKFB ESP32S3R8N8 board
#define LED_ON          1       // level that lights the LED

#define WDT_TIMEOUT_MS  30000   // reboot if the main loop or a CPU core hangs this long

#define WIFI_RETRY_MS       10000   // reconnect attempt interval
#define WIFI_AP_AFTER_MS    30000   // start the setup AP after this long without a connection
#define WIFI_AP_RETRY_MS    60000   // reconnect attempt interval while the setup AP is up

// WireGuard VPN, the tunnel itself is set up over the API (see vpn.c)
#define NTP_SERVER      "pool.ntp.org"  // the VPN needs the real time
#define VPN_KEEPALIVE_S 25      // for a config without PersistentKeepalive, 0 = off
#define VPN_KEEPALIVE_MAX_S 120 // largest PersistentKeepalive the WireGuard library works with
#define VPN_RETRY_MS    30000   // wait this long after a failed start
#define VPN_RESOLVE_MS  120000  // tunnel down this long: look the peer's name up again

// Firmware update over the air (see ota.c). An update built for another board
// type is refused.
#define BOARD_TYPE      "fans8"     // 15 characters at most
#define OTA_CONFIRM_MIN 5           // minutes a new firmware has to be confirmed, else the old one comes back

#define HOSTNAME        "fans2"         // http://fans2.local/
#define AP_SSID         "fan-pwm-setup"
#define AP_PASS         "fancontrol"    // at least 8 characters
