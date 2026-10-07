#pragma once

// Starts the task of the OLED display. Without a display connected it keeps
// looking for one and the rest of the firmware runs as before.
void display_start(void);

// new_brightness: 0..255. Shown at once and kept until the next reboot, which
// starts with DISPLAY_BRIGHTNESS again.
void display_brightness_set(int new_brightness);
int display_brightness(void);
