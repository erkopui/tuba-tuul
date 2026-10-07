// Status LED:
//   short flash every 2 s  - connected, all good
//   slow blink (1 Hz)      - connecting to WiFi
//   fast blink (5 Hz)      - setup AP is up, waiting for WiFi credentials

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "config.h"
#include "led.h"
#include "mtimer.h"
#include "stat.h"
#include "wifi.h"

static void led_poll(wifi_state_t state)
{
    static mtimer_t timer;
    static bool on;

    if (!mtimer_timeout(&timer)) {
        return;
    }
    on = !on;
    mtimer_timeout_set(&timer, state == WIFI_CONNECTED ? (on ? 100 : 1900)
                             : state == WIFI_SETUP_AP  ? 100
                             :                           500);
    gpio_set_level(LED_GPIO, on ? LED_ON : !LED_ON);
}

static void led_task(void *arg)
{
    for (;;) {
        stat_begin(STAT_LED);
        led_poll(wifi_state());
        stat_end(STAT_LED);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void led_start(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    // The lowest priority there is, the one of the idle task, on core 1: it
    // runs when nothing else wants to, so its times in /api/tasks show how
    // long that can take. It gets its turn from the idle task at the next
    // 10 ms tick, so a round is about 30 ms, not 20.
    xTaskCreatePinnedToCore(led_task, "led", 2048, NULL, tskIDLE_PRIORITY, NULL, 1);
}
