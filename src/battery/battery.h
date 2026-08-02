#pragma once
#include <stdint.h>
#include <stdbool.h>

void     battery_init(void);
uint8_t  battery_percent(void);    /* 0-100 */
uint32_t battery_voltage_mv(void); /* millivolts */
bool     battery_charging(void);   /* avg current > 0 */
bool     battery_is_low(void);     /* true when SoC < LOW_BATTERY_THRESHOLD */

/*
 * Show the charge percentage as centred digits. Bound to a 3 second hold of
 * the right button, for a 3 second peek — unless the watch is charging, in
 * which case the readout is already up persistently (see battery.c) and this
 * call is a no-op.
 *
 * Colour encodes charge level; motion encodes charging: static blue while
 * discharging, green with a travelling hue shimmer while charging, red below
 * 20% either way. Uses the cached gauge reading (at most POLL_INTERVAL_S old)
 * rather than blocking the caller on I2C.
 *
 * ISR-safe: only submits work onto the system workqueue.
 */
void     battery_show_level(void);

/*
 * Tear down whatever battery screen is currently up (percentage peek or the
 * persistent charging view), if any. Call before anything else takes over
 * the whole display — currently only time_display_reveal(). Must be called
 * from workqueue context, not an ISR (calls level_dismiss(), which touches
 * timers and led_mask directly).
 */
void     battery_screen_dismiss(void);

/*
 * Tell battery.c the display just went fully dark (called by
 * display.c's display_off()), so an active low-battery warning can
 * re-assert itself on the next poll instead of being shown at most once
 * per low-battery episode.
 */
void     battery_notify_display_off(void);
