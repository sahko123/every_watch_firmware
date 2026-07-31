#pragma once

#include <zephyr/device.h>
#include <stdbool.h>

/* Initialise time display. Renders immediately then updates every second. */
void time_display_init(const struct device *rtc_dev);

/*
 * Start the curtain reveal of the current time.
 *
 * A sheet of sand sweeps down the whole display: density fades in from nothing
 * to complete cover, holds, then fades back out. At full cover the digits are
 * placed on the layer beneath the sand, so they are simply left behind as the
 * curtain thins and clears.
 *
 * Fixed downward gravity — unaffected by how the watch is held, and needs no
 * accelerometer.
 */
void time_display_reveal(void);

/*
 * The alternative reveal: particles stream only into columns containing digits
 * and lock into the shape, building it bottom-up. Kept as a second effect.
 */
void time_display_reveal_fill(void);

/* Return to the plain static digit display. */
void time_display_stop_reveal(void);

/* True while a reveal is in progress. */
bool time_display_revealing(void);
