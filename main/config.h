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

// OLED displays with an SSD1306 or SSD1315 on I2C, each on a bus of its own:
// one of 128x64 for the fans and the network, one of 128x32 for time and date.
// The firmware runs without them too. (GPIO 48 drives the LED.)
#define DISPLAY_SDA_GPIO    47
#define DISPLAY_SCL_GPIO    38
#define DISPLAY_ADDR        0x3C    // 0x3D with the address pin of the display high
#define DISPLAY2_SDA_GPIO   39
#define DISPLAY2_SCL_GPIO   40
#define DISPLAY2_ADDR       0x3C
#define DISPLAY2_UPSIDE_DOWN 1      // 1 turns the picture by 180 degrees
#define DISPLAY_VIEW_MS     4000    // each view is shown this long
#define DISPLAY_BRIGHTNESS  255     // 0..255 at start (the display's own default is 127)
#define DISPLAY_UPSIDE_DOWN 0       // 1 turns the picture by 180 degrees

// Local time for the display: Estonia, with summer time from the last Sunday
// of March 03:00 to the last Sunday of October 04:00. The board has no list
// of time zones, the rule itself is given (POSIX TZ).
#define TIME_ZONE       "EET-2EEST,M3.5.0/3,M10.5.0/4"

#define WDT_TIMEOUT_MS  30000   // reboot if the main loop or a CPU core hangs this long

#define WIFI_NETS           5       // saved networks, 8 at most; a smaller number forgets the saved ones
#define WIFI_ADDED_MS       1500    // a network added without a connection is tried this much later
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

#define HOSTNAME        "fans2"         // http://fans2.local/, until another name is set over the API
#define AP_SSID         "fan-pwm-setup"
#define AP_PASS         "fancontrol"    // at least 8 characters
