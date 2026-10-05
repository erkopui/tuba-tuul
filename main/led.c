// Status LED:
//   short flash every 2 s  - connected, all good
//   slow blink (1 Hz)      - connecting to WiFi
//   fast blink (5 Hz)      - setup AP is up, waiting for WiFi credentials

#include "driver/gpio.h"
#include "config.h"
#include "led.h"
#include "mtimer.h"

void led_init(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
}

void led_poll(wifi_state_t state)
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
