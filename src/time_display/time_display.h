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

/*
 * Stop / restart republishing the digit layer, for when another screen wants
 * the display to itself. The clock keeps ticking throughout; only the drawing
 * pauses. time_display_resume() redraws straight away rather than waiting up to
 * a second for the next tick.
 */
void time_display_pause(void);
void time_display_resume(void);

/*
 * Ends the current time-viewing session. After this, the 1 Hz tick keeps the
 * digit bitmap current internally but stops publishing it — so nothing shows
 * time again just because the display wakes up for an unrelated reason (a
 * battery check, a low-battery blip). Call when the display goes fully off.
 * A fresh time_display_reveal() is what starts a session again.
 */
void time_display_deactivate(void);
