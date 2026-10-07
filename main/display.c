// Two small OLED displays with an SSD1306 or SSD1315 on I2C (pins in
// config.h), each with two views that take turns:
//   128x64: the fan speeds, and the board on the network
//   128x32: the time, and the date
// A display that is not connected is left out, and found when it comes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "config.h"
#include "display.h"
#include "fan.h"
#include "font.h"
#include "stat.h"
#include "vpn.h"
#include "wifi.h"

#define COLS    16      // characters of 8x16 pixels on a line
#define ROWS    4

static const char *TAG = "display";

typedef struct {
    int sda_gpio, scl_gpio, addr, height;
    bool upside_down;
    i2c_master_bus_handle_t bus;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    volatile bool ready;        // set up and answering
    bool on;                    // switched on, which it is after its first picture
    // The picture the way the display keeps it: bands of 128 bytes, a byte is
    // 8 pixels below each other with the lowest bit on top. `sent` is what
    // the display has, a picture is only sent when it differs.
    uint8_t frame[128 * 8], sent[128 * 8];
} display_t;

static display_t displays[] = {
    { .sda_gpio = DISPLAY_SDA_GPIO, .scl_gpio = DISPLAY_SCL_GPIO, .addr = DISPLAY_ADDR, .height = 64,
      .upside_down = DISPLAY_UPSIDE_DOWN },
    { .sda_gpio = DISPLAY2_SDA_GPIO, .scl_gpio = DISPLAY2_SCL_GPIO, .addr = DISPLAY2_ADDR, .height = 32,
      .upside_down = DISPLAY2_UPSIDE_DOWN },
};
static uint8_t *frame;          // of the display that is being drawn
static volatile uint8_t brightness = DISPLAY_BRIGHTNESS;

// 0x81 is the brightness ("contrast") command of the SSD1306, the driver of
// the SDK has no call for it.
static bool brightness_send(display_t *display)
{
    return esp_lcd_panel_io_tx_param(display->io, 0x81, (uint8_t[]){ brightness }, 1) == ESP_OK;
}

// Called from the web server's task. The command is one transfer of its own
// on the bus, so it may come between two of the display task.
void display_brightness_set(int new_brightness)
{
    brightness = new_brightness;
    for (int i = 0; i < sizeof(displays) / sizeof(displays[0]); i++) {
        if (displays[i].ready) {
            brightness_send(&displays[i]);
        }
    }
}

int display_brightness(void)
{
    return brightness;
}

// Writes a text into the picture, what does not fit on the line is left out.
// text_col: 0..COLS-1, text_row: 0..ROWS-1
static void text(int text_col, int text_row, const char *str)
{
    for (; *str && text_col < COLS; str++, text_col++) {
        const uint8_t *glyph = font[*str >= 32 && *str < 127 ? *str - 32 : 0];

        for (int x = 0; x < 8; x++) {
            uint8_t upper = 0, lower = 0;

            for (int y = 0; y < 8; y++) {
                upper |= (glyph[y] >> (7 - x) & 1) << y;
                lower |= (glyph[y + 8] >> (7 - x) & 1) << y;
            }
            frame[text_row * 256 + text_col * 8 + x] = upper;
            frame[text_row * 256 + 128 + text_col * 8 + x] = lower;
        }
    }
}

// Writes one line as large as a 128x32 display takes it: the 10 rows of the
// font that digits use, three times as high (the top and the bottom row four
// times, which makes the 32) and twice as wide. A digit gets
// digit_width pixels and '.' or ':' punct_width (16 at most, less cuts the
// empty sides off), the line stands in the middle.
static void big_text(const char *str, int digit_width, int punct_width)
{
    int x = 128;

    for (const char *p = str; *p; p++) {
        x -= *p == '.' || *p == ':' ? punct_width : digit_width;
    }
    x /= 2;
    for (; *str; str++) {
        const uint8_t *glyph = font[*str >= 32 && *str < 127 ? *str - 32 : 0];
        int width = *str == '.' || *str == ':' ? punct_width : digit_width;

        for (int i = 0; i < width; i++, x++) {
            int font_col = ((16 - width) / 2 + i) / 2;
            uint32_t column = 0;    // 32 pixels, the top one in the lowest bit

            for (int row = 0; row < 10; row++) {
                if (glyph[4 + row] >> (7 - font_col) & 1) {
                    column |= (row == 0 || row == 9 ? 15u : 7u) << (3 * row + (row > 0));
                }
            }
            for (int band = 0; band < 4 && x >= 0 && x < 128; band++) {
                frame[band * 128 + x] = column >> (8 * band);
            }
        }
    }
}

// Two fans on a line: "1:100%   5: 40%"
static void fans_view(void)
{
    int rows = (FAN_COUNT + 1) / 2;

    for (int i = 0; i < rows && i < ROWS; i++) {
        char line[COLS + 1];
        int len = snprintf(line, sizeof(line), "%d:%3d%%", i + 1, fan_get(i));

        if (i + rows < FAN_COUNT) {
            snprintf(line + len, sizeof(line) - len, "   %d:%3d%%", i + rows + 1, fan_get(i + rows));
        }
        text(0, i, line);
    }
}

// How to reach the board, and how it stands with WiFi and the VPN:
//   fans2.local         its name, in setup mode the name of the setup AP
//   192.168.8.183       its address there
//   172.20.255.2        its address in the VPN, when one is set up
//   WiFi ok  VPN ok     ok, .. (on the way), AP (setup AP), off, err
static void network_view(void)
{
    wifi_state_t state = wifi_state();
    const char *vpn = vpn_state(), *tunnel = vpn_tunnel();
    esp_netif_ip_info_t ip = { 0 };
    char line[COLS + 1];

    if (state == WIFI_SETUP_AP) {
        text(0, 0, AP_SSID);
    } else {
        snprintf(line, sizeof(line), "%s.local", wifi_name());
        text(0, 0, line);
    }
    if (state != WIFI_CONNECTING) {
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey(state == WIFI_SETUP_AP ? "WIFI_AP_DEF" : "WIFI_STA_DEF"), &ip);
        snprintf(line, sizeof(line), IPSTR, IP2STR(&ip.ip));
        text(0, 1, line);
    }
    // The tunnel text is "10.0.0.2/24 via ...", the address is its start.
    snprintf(line, sizeof(line), "%.*s", (int)strcspn(tunnel, "/"), tunnel);
    text(0, 2, line);
    snprintf(line, sizeof(line), "WiFi %s  VPN %s",
             state == WIFI_CONNECTED ? "ok" : state == WIFI_SETUP_AP ? "AP" : "..",
             strcmp(vpn, "up") == 0 ? "ok" : strcmp(vpn, "off") == 0 ? "off"
             : strcmp(vpn, "connecting") == 0 || strncmp(vpn, "waiting", 7) == 0 ? ".." : "err");
    text(0, 3, line);
}

// The time here, false while the board has not been told the time yet.
static bool local_time(struct tm *local)
{
    time_t now = time(NULL);

    localtime_r(&now, local);
    return local->tm_year >= 2025 - 1900;
}

static void time_view(void)
{
    struct tm local;
    char line[12] = "--:--:--";

    if (local_time(&local)) {
        strftime(line, sizeof(line), "%H:%M:%S", &local);
    }
    big_text(line, 16, 16);
}

static void date_view(void)
{
    struct tm local;
    char line[12] = "--.--.----";

    if (local_time(&local)) {
        strftime(line, sizeof(line), "%d.%m.%Y", &local);
    }
    big_text(line, 14, 8);
}

// Draws a view and sends it when it differs from what the display shows. A
// display that does not answer is looked for again on every call, and one
// that stops answering is set up again when it is back.
static void show(display_t *display, void (*view)(void))
{
    size_t size = 128 * display->height / 8;

    if (!display->panel) {
        return;
    }
    if (!display->ready) {
        // Asked for first, without a word in the log when none answers.
        display->ready = i2c_master_probe(display->bus, display->addr, 50) == ESP_OK
                      && esp_lcd_panel_init(display->panel) == ESP_OK
                      && esp_lcd_panel_mirror(display->panel, !display->upside_down, !display->upside_down) == ESP_OK
                      && brightness_send(display);
        display->on = false;
    }
    if (!display->ready) {
        return;
    }
    frame = display->frame;
    memset(frame, 0, size);
    view();
    if (display->on && memcmp(frame, display->sent, size) == 0) {
        return;
    }
    // Switched on after its first picture, not with what its memory held.
    display->ready = esp_lcd_panel_draw_bitmap(display->panel, 0, 0, 128, display->height, frame) == ESP_OK
                  && (display->on || esp_lcd_panel_disp_on_off(display->panel, true) == ESP_OK);
    display->on = display->ready;
    memcpy(display->sent, frame, size);
}

static void display_task(void *arg)
{
    for (;;) {
        bool second = esp_timer_get_time() / 1000 / DISPLAY_VIEW_MS % 2;

        stat_begin(STAT_DISPLAY);
        show(&displays[0], second ? network_view : fans_view);
        show(&displays[1], second ? date_view : time_view);
        stat_end(STAT_DISPLAY);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void display_start(void)
{
    setenv("TZ", TIME_ZONE, 1);
    tzset();
    for (int i = 0; i < sizeof(displays) / sizeof(displays[0]); i++) {
        display_t *display = &displays[i];
        i2c_master_bus_config_t bus_config = {
            .i2c_port = -1,     // any free one, each display has a bus of its own
            .sda_io_num = display->sda_gpio,
            .scl_io_num = display->scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,   // weak, the display module has its own
        };
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = display->addr,
            .scl_speed_hz = 400000,
            .control_phase_bytes = 1,   // a byte in front of everything tells commands from picture data,
            .dc_bit_offset = 6,         // by this bit of it
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        esp_lcd_panel_ssd1306_config_t ssd1306_config = { .height = display->height };
        esp_lcd_panel_dev_config_t panel_config = {
            .bits_per_pixel = 1,
            .reset_gpio_num = -1,
            .vendor_config = &ssd1306_config,
        };

        // A failure here must not take the fans down with it, so no ESP_ERROR_CHECK.
        if (i2c_new_master_bus(&bus_config, &display->bus) != ESP_OK
                || esp_lcd_new_panel_io_i2c(display->bus, &io_config, &display->io) != ESP_OK
                || esp_lcd_new_panel_ssd1306(display->io, &panel_config, &display->panel) != ESP_OK) {
            ESP_LOGE(TAG, "cannot set up display %d", i + 1);
            display->panel = NULL;
        }
    }
    if (xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 1, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "cannot start");
    }
}
