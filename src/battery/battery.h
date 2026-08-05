#pragma once
#include <stdint.h>
#include <stdbool.h>

void     battery_init(void);
uint8_t  battery_percent(void);    /* 0-100, smoothed (see battery_work_fn()) */
uint32_t battery_voltage_mv(void); /* millivolts */
bool     battery_charging(void);   /* avg current > 0 */
/*
 * VBUS detected by the nRF52833's own POWER peripheral: a cable is
 * physically attached, independent of whether the fuel gauge currently sees
 * charge current flowing. Current tapers to ~0 once the cell tops off, which
 * reads identically to "unplugged" on its own — this doesn't.
 */
bool     battery_cable_present(void);
bool     battery_is_low(void);     /* true when SoC < LOW_BATTERY_THRESHOLD */

/*
 * Draw the charge percentage as centred digits. Only ever called from ui.c's
 * battery_enter(), i.e. on a genuine fresh entry to the battery page — ui.c
 * itself is what makes a right-button hold while already showing a no-op,
 * by not re-entering the page at all in that case (see
 * ui_handle_button()'s BTN_EV_R_HOLD).
 *
 * Only ever opened manually — plugging in does not show this page by
 * itself. It gets ui.c's normal page timeout like any other page, unless
 * the watch turns out to be on power while it's up, in which case
 * battery_work_fn() promotes it to stay open for as long as that lasts (see
 * battery.c). A quick right-press or unplugging (whichever comes first)
 * dismisses it either way.
 *
 * Colour encodes charge level (red under 20%, green while on power, blue
 * otherwise); a travelling hue shimmer overlays it while current is actually
 * flowing, so a cell sitting topped-off on the charger still reads green but
 * stops shimmering. Uses the cached gauge reading (at most POLL_INTERVAL_S
 * old) rather than blocking the caller on I2C.
 *
 * ISR-safe: only submits work onto the system workqueue.
 */
void     battery_show_level(void);

/*
 * Tear down whatever battery screen is currently up (a peek, promoted or
 * not), if any. Call before anything else takes over the whole display —
 * currently only time_display_reveal(). Must be called from workqueue
 * context, not an ISR (calls level_dismiss(), which touches timers and
 * led_mask directly).
 */
void     battery_screen_dismiss(void);

/*
 * Tell battery.c the display just went fully dark (called by
 * display.c's display_off()), so an active low-battery warning can
 * re-assert itself on the next poll instead of being shown at most once
 * per low-battery episode.
 */
void     battery_notify_display_off(void);
