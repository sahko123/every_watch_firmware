#pragma once

#include <stdbool.h>

#define DISPLAY_TIMEOUT_S 10

void display_init(void);
void display_on(void);
void display_off(void);
bool display_is_on(void);
void display_reset_timeout(void);

/* Sample ambient light, wake the display, and start a time reveal — the same
 * sequence the left button ISR runs. ISR-safe: only submits work, does not
 * touch I2C or the LED matrix directly. Used by btn_isr and by the IMU's
 * wrist-tilt trigger. */
void display_wake_and_reveal(void);
