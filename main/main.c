// 8-channel 25 kHz PWM fan controller with a REST API, Modbus TCP and an
// optional WireGuard VPN, for ESP32-S3.
//
// WiFi and the TCP/IP stack run on core 0, this task and the HTTP server on
// core 1 (see sdkconfig.defaults). Settings are in config.h, the API in http.c,
// Modbus in modbus.c, the VPN in vpn.c.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "config.h"
#include "fan.h"
#include "http.h"
#include "led.h"
#include "modbus.h"
#include "mtimer.h"
#include "vpn.h"
#include "wifi.h"

// Time since boot for mtimer.
static uint32_t timer_get(uint16_t *msec)
{
    int64_t us = esp_timer_get_time();
    *msec = us / 1000 % 1000;
    return us / 1000000;
}

void app_main(void)
{
    mtimer_init(timer_get);

    // Fans first, so they get a defined (off) signal as early as possible.
    fan_init();
    led_init();

    esp_task_wdt_config_t wdt = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = 3,    // also watch the idle tasks of both cores
        .trigger_panic = true,  // panic -> reboot
    };
    esp_err_t err = esp_task_wdt_init(&wdt);
    if (err == ESP_ERR_INVALID_STATE) {     // already started by CONFIG_ESP_TASK_WDT_INIT
        err = esp_task_wdt_reconfigure(&wdt);
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    wifi_init();
    modbus_start();
    http_start();
    vpn_start();

    for (;;) {
        esp_task_wdt_reset();
        wifi_poll();
        led_poll(wifi_state());
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
