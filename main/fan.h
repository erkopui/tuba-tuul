#pragma once

void fan_init(void);
// fan_idx: 0..FAN_COUNT-1
// percent: 0..100
// fade_ms: time a 0 to 100 % change would take, 0 = at once
void fan_set(int fan_idx, int percent, int fade_ms);
int fan_get(int fan_idx);          // target speed in percent
