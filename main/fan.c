// 25 kHz PWM outputs for 4-pin fans, one LEDC channel per fan.

#include <sys/lock.h>
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "config.h"
#include "fan.h"

static const int fan_gpio[] = FAN_GPIOS;
_Static_assert(sizeof(fan_gpio) / sizeof(fan_gpio[0]) == FAN_COUNT, "FAN_GPIOS must list FAN_COUNT pins");
_Static_assert(FAN_COUNT <= LEDC_CHANNEL_MAX, "one LEDC channel per fan");
uint16_t fan_percent[FAN_COUNT];
static _lock_t fan_lock;     // fan_set is called from the HTTP server and the Modbus task

void fan_set(int fan_idx, int percent, int fade_ms)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = percent * (1 << PWM_BITS) / 100;

    _lock_acquire(&fan_lock);
    fan_percent[fan_idx] = percent;
    // Freeze a fade still in progress and continue from where the speed is now.
    ledc_fade_stop(LEDC_LOW_SPEED_MODE, fan_idx);
    uint32_t now = ledc_get_duty(LEDC_LOW_SPEED_MODE, fan_idx);
    uint32_t ms = (uint64_t)fade_ms * (duty > now ? duty - now : now - duty) >> PWM_BITS;
    if (ms) {
        ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, fan_idx, duty, ms, LEDC_FADE_NO_WAIT);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, fan_idx, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, fan_idx);
    }
    _lock_release(&fan_lock);
}

int fan_get(int fan_idx)
{
    return fan_percent[fan_idx];
}

void fan_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = PWM_BITS,
        .freq_hz = PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    for (int i = 0; i < FAN_COUNT; i++) {
        ledc_channel_config_t ch = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_sel = LEDC_TIMER_0,
            .channel = i,
            .gpio_num = fan_gpio[i],
            .duty = 0,
            .flags.output_invert = PWM_INVERT,
        };
        if (PWM_OPEN_DRAIN) {
            // LEDC only routes its signal to the pin, the open-drain setting stays.
            gpio_set_direction(fan_gpio[i], GPIO_MODE_OUTPUT_OD);
        }
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
}
