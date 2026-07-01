#pragma once

#include <zephyr/device.h>

/* Initialise time display. Renders immediately then updates every second. */
void time_display_init(const struct device *rtc_dev);
