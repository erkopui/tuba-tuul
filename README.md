# fan-pwm

8-channel 25 kHz PWM fan controller with a REST API, Modbus TCP, an optional WireGuard VPN and
firmware update over WiFi.
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

After that first time a new firmware can also be sent over WiFi, see
[Firmware update](#firmware-update).

The serial port may also show up as `/dev/ttyUSB0`.

## WiFi setup

On first boot the board starts the access point `fan-pwm-setup` (password `fancontrol`).
Connect to it, open http://192.168.4.1/, enter the home WiFi name and password, press Add.
The board joins that network. It is then reachable as http://fans2.local/
(the IP address is also printed on the serial console).

Up to 5 networks can be saved (`WIFI_NETS` in `main/config.h`). Without a connection the board
looks which of them are in range and joins the strongest, about 1 s of looking for each saved
one. One that does not let it in is left out the next time, so a wrong password does not keep the
others from being tried. Hidden networks work too. Adding a network
does not disturb a working connection. A removed network that is in use stays connected until
that link drops. The test page lists the saved networks by name; the passwords are never given back.

If no saved network can be reached for 30 s the setup AP comes back until a connection works again.

The name `fans2` can be changed on the test page, or with `/api/name`. The board answers to the
new name at once and keeps it over reboots. Give each board on a network its own name.

Status LED (GPIO 48): short flash every 2 s = connected, slow blink = connecting, fast blink = setup AP is up.

## REST API

    curl http://fans2.local/api/fans                 # [0,0,0,0,0,0,0,0]  speed of each fan in %
    curl -d 75 http://fans2.local/api/fans/2         # fan 2 to 75 %
    curl -d 30 http://fans2.local/api/fans/all       # every fan to 30 %
    curl -d 100 'http://fans2.local/api/fans/1?fade=3000'   # with its own ramp time
    curl -d 0 'http://fans2.local/api/fans/all?fade=0'      # at once
    curl -d $'ssid\npassword' http://fans2.local/api/wifi   # save a WiFi network
    curl -X DELETE -d ssid http://fans2.local/api/wifi      # remove a saved one
    curl http://fans2.local/api/wifi                 # {"name":"fans2","saved":["home","office"]}
    curl http://fans2.local/api/wifi/scan            # networks in range, strongest first
    curl -d fans3 http://fans2.local/api/name        # the board is http://fans3.local/ from now on

The VPN requests are under [VPN](#vpn-wireguard), the update under
[Firmware update](#firmware-update).

A speed change is not applied at once: the fan ramps to the new speed, 10 s for the full 0 to 100 %
range (`FAN_FADE_MS` in `main/config.h`), smaller changes proportionally less. `?fade=<ms>` overrides
that time for one request, 0 to 40000. The API always reports the target speed.

The POST requests answer with the new speed list.

http://fans2.local/ is a test page: a slider and 0/50/100 buttons per fan, an "All" row, a field for
the fade time, a log line with the last request and the answer of the device, the WiFi and
VPN settings and the firmware update.

There is no authentication, keep the device on a trusted network.

## Modbus TCP

Port 502, any unit id. Holding registers 0-7 are the speeds of fans 1-8 in percent
(functions 3, 6 and 16). A written value above 100 counts as 100. Speed changes use the default
ramp time. REST and Modbus show the same speeds.

## VPN (WireGuard)

Optional. With a WireGuard config stored, the board keeps a tunnel open to one WireGuard server.
The test page, the REST API and Modbus can then be reached through that server from anywhere,
at the board's tunnel address.

On the WireGuard server add the board as a client, the same way as a phone or a laptop, and take
the client config it gives:

    [Interface]
    PrivateKey = ...
    Address = 10.0.0.2/24

    [Peer]
    PublicKey = ...
    Endpoint = vpn.example.com:51820

Paste it into the VPN box of the test page, or send the file:

    curl --data-binary @fans.conf http://fans2.local/api/vpn   # store (-d would drop the line ends)
    curl http://fans2.local/api/vpn              # {"state":"up","tunnel":"10.0.0.2/24 via vpn.example.com:51820"}
    curl -X DELETE http://fans2.local/api/vpn    # remove

A stored or removed config takes effect within a few seconds, without a reboot, so the fans keep
running. The config is kept over reboots.

Used from the config: `PrivateKey`, `Address`, `PublicKey`, `PresharedKey`, `Endpoint` and
`PersistentKeepalive` (25 s if not given, 120 s at most). Everything else is ignored, `AllowedIPs`
and `DNS` too.

- IPv4 only, one peer. `Endpoint` is a name or an IPv4 address, with the port 79 characters at most.
- Only what is sent to the tunnel address, and the answers to it, go through the tunnel. Everything
  else the board sends still goes over WiFi.
- The board needs the time from the internet (`NTP_SERVER` in `main/config.h`) before the tunnel
  starts, because WireGuard refuses a handshake with an old time stamp. Until then the state is
  "waiting for the time".
- The tunnel network (the `Address` with its `/24`) must not contain the board's own WiFi address,
  its router or DNS server, or the setup AP's 192.168.4.1. Such a config is refused.
- Through the tunnel, open the page by the tunnel IP address (http://10.0.0.2/). Under another
  name the page loads, but its changes are refused as cross-site requests.
- When the server is restarted it takes about two minutes until the tunnel works again. The state
  can still say "up" for up to three minutes after the server stopped answering. When the server
  moves to a new address, the board follows after three to five minutes.
- A new config replaces the old one at once, there is no way back to the old one. When you change it
  through the tunnel and the new one is wrong, you cut your own access; then it has to be fixed from
  the home network.
- The private key is sent over plain HTTP when it is set and is stored unencrypted in the flash. The
  API never gives it back. Removing the config does not wipe it from the flash chip, erase the flash
  (`./idf.sh erase-flash`) before giving the board away.
- The API has no authentication on the tunnel either. Whoever can reach the tunnel address can set
  the fans and change the WiFi and VPN settings, so limit on the server who may reach the board.
  The same goes for the setup AP: change `AP_PASS` in `main/config.h`, or someone in radio range
  can point the tunnel at a server of their own while the AP is up.

## Firmware update

A new firmware can be sent over WiFi, or through the VPN. On the test page pick
`build/fan-pwm.bin` under "Board" and press Update, or send the file:

    curl --data-binary @build/fan-pwm.bin http://fans2.local/api/ota   # write it and reboot into it
    curl http://fans2.local/api/ota      # {"version":"68db7ba","board":"fans8","slot":"ota_0"}

The upload takes about 20 s, then the board reboots. A reboot switches all fans off until their
speeds are set again. WiFi and VPN settings stay.

The flash holds two firmware slots of 2 MB (`partitions.csv`). The new firmware is written to the
one that is not running, so a failed upload leaves the running firmware as it was.

A file is refused, before anything is written, unless it is

- a firmware for this chip and of this project (`fan-pwm`),
- and for this board type (`BOARD_TYPE` in `main/config.h`).

A damaged or incomplete file is refused at the end of the upload.

After the reboot the new firmware runs on trial. The first connection to its web server confirms
it: the test page, which asks by itself, or any API request. Modbus does not count. When it is not
confirmed within 5 minutes (`OTA_CONFIRM_MIN`), or it crashes or is reset before, the board goes
back to the previous firmware. So a firmware that cannot be reached replaces itself with the one
that could.

The board type is the only thing a firmware says about the hardware it is for. The board takes
its own type from the firmware it runs, so the first firmware, flashed over USB, has to be the
right one.

- The version is what `git describe` said when the firmware was built. An older version is
  accepted like a newer one.
- The firmware is not signed and the API has no authentication: whoever can reach the board, by
  WiFi or through the VPN, can replace its firmware.
- While the file is uploaded the web API answers nobody else, it takes one request at a time.
  Modbus keeps answering. The fan PWM signal comes from hardware and does not depend on either.
- A board flashed before the two slots were added needs one flash over USB to get them. Its
  settings stay.
- Flashing over USB always writes the first slot and makes it the one to start.

## Source

| File            | What                                   |
|-----------------|----------------------------------------|
| `main/config.h` | pins, PWM frequency, names, passwords  |
| `main/main.c`   | start-up, watchdog, main loop          |
| `main/fan.c`    | PWM outputs                            |
| `main/http.c`   | REST API                               |
| `main/modbus.c` | Modbus TCP server (esp-modbus)         |
| `main/vpn.c`    | WireGuard tunnel (esp_wireguard)       |
| `main/ota.c`    | firmware update over the air           |
| `partitions.csv` | flash layout with two firmware slots  |
| `main/index.html` | test page served at `/`              |
| `main/wifi.c`   | WiFi station + setup AP, mDNS          |
| `main/led.c`    | status LED                             |
| `main/mtimer.c` | millisecond timeouts                   |
