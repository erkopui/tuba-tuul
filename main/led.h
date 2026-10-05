#pragma once

#include "wifi.h"

void led_init(void);
void led_poll(wifi_state_t state);  // call from the main loop
