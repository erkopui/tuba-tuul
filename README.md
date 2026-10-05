# fan-pwm

8-channel 25 kHz PWM fan controller with a REST API.
ESP-IDF firmware for the LCKFB ESP32S3R8N8 board (LCSC C20626143, ESP32-S3R8 + 8 MB flash).

WiFi and the TCP/IP stack run on core 0, the application (main loop + HTTP server) on core 1.

## Wiring

| Fan | GPIO |
|-----|------|
| 1   | 7    |
| 2   | 8    |
| 3   | 9    |
| 4   | 10   |
| 5   | 11   |
| 6   | 12   |
| 7   | 13   |
| 8   | 14   |

Standard 4-pin PC fan connector: 1 GND, 2 +12 V, 3 tacho (unused), 4 PWM.

Each GPIO is wired straight to the PWM pin of its fan and drives it high (3.3 V) and low.

- Fan GND and ESP32 GND **must** be connected together.
- The fan is powered from its own 12 V supply, not from the board.

The fan pulls its PWM pin up internally, so while the ESP32 is unpowered, in reset or being flashed
the fan runs at full speed.

All fans are off (0 %) after power-up or reboot until the API sets a speed.
Note that many PC fans keep spinning at their minimum speed at 0 % duty.

## Build and flash

Only Docker is needed, `idf.sh` runs `idf.py` in the official `espressif/idf` image.

    ./idf.sh build
    ./idf.sh -p /dev/ttyACM0 flash monitor     # exit the monitor with Ctrl+]

The serial port may also show up as `/dev/ttyUSB0`.

## WiFi setup

On first boot the board starts the access point `fan-pwm-setup` (password `fancontrol`).
Connect to it, open http://192.168.4.1/, enter the home WiFi name and password, press Save.
The board reboots and joins that network. It is then reachable as http://fans.local/
(the IP address is also printed on the serial console).

If the home WiFi cannot be reached for 30 s the setup AP comes back until the connection works again.

Status LED (GPIO 48): short flash every 2 s = connected, slow blink = connecting, fast blink = setup AP is up.

## REST API

    curl http://fans.local/api/fans                 # [0,0,0,0,0,0,0,0]  speed of each fan in %
    curl -d 75 http://fans.local/api/fans/2         # fan 2 to 75 %
    curl -d 30 http://fans.local/api/fans/all       # every fan to 30 %
    curl -d 100 'http://fans.local/api/fans/1?fade=3000'   # with its own ramp time
    curl -d 0 'http://fans.local/api/fans/all?fade=0'      # at once
    curl -d $'ssid\npassword' http://fans.local/api/wifi   # change WiFi and reboot
    curl http://fans.local/api/wifi/scan            # networks in range, strongest first

A speed change is not applied at once: the fan ramps to the new speed, 10 s for the full 0 to 100 %
range (`FAN_FADE_MS` in `main/config.h`), smaller changes proportionally less. `?fade=<ms>` overrides
that time for one request, 0 to 40000. The API always reports the target speed.

The POST requests answer with the new speed list.

http://fans.local/ is a test page: a slider and 0/50/100 buttons per fan, an "All" row, a field for
the fade time, and a log line with the last request and the answer of the device.

There is no authentication, keep the device on a trusted network.

## Source

| File            | What                                   |
|-----------------|----------------------------------------|
| `main/config.h` | pins, PWM frequency, names, passwords  |
| `main/main.c`   | start-up, watchdog, main loop          |
| `main/fan.c`    | PWM outputs                            |
| `main/http.c`   | REST API                               |
| `main/index.html` | test page served at `/`              |
| `main/wifi.c`   | WiFi station + setup AP, mDNS          |
| `main/led.c`    | status LED                             |
| `main/mtimer.c` | millisecond timeouts                   |
