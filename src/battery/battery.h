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
