#pragma once

#include <zephyr/device.h>
#include <stdbool.h>

/* Initialise time display. Renders immediately then updates every second. */
void time_display_init(const struct device *rtc_dev);

/*
 * Start the waterfall reveal of the current time.
 *
 * Clears the static digit layer and hands the digit bitmap to the sand
 * simulation as a fill target, so the falling particles themselves form the
 * digits. Runs on fixed downward gravity regardless of how the watch is held.
 */
void time_display_reveal(void);

/* Return to the plain static digit display. */
void time_display_stop_reveal(void);

/* True while a reveal is in progress. */
bool time_display_revealing(void);
