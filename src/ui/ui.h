#pragma once

#include <stdbool.h>
#include "buttons/buttons.h"

/*
 * Display arbitration.
 *
 * Exactly one page owns the display at a time. Before this existed, six
 * modules wrote led_mask[] directly, seven called led_commit(), and four
 * called display_on() — none of them aware of each other. Every coordination
 * problem was patched bilaterally (battery_screen_dismiss(), time_display's
 * pause/resume and `active` flag), which is O(n^2) in modules and was already
 * showing: a button press during the charging view could leave the clock
 * frozen for a whole charging session.
 *
 * The rules:
 *
 *   - Only ui.c calls display_on()/display_off() for page content.
 *   - Every transition blanks all layers and commits, even when the next page
 *     draws immediately afterwards. Cheap, and it means no page can inherit
 *     pixels from the one before it.
 *   - Only the visible page ticks. The sand simulation and curtain animation
 *     burn no cycles while nothing is looking at them.
 *   - The sand thread runs only for pages that declare needs_sand.
 */
enum ui_page {
	UI_PAGE_BLANK = 0,   /* display off — the resting state */
	UI_PAGE_CLOCK,       /* curtain reveal, then the time */
	UI_PAGE_SAND,        /* free-physics sand toy */
	UI_PAGE_BATTERY,     /* charge percentage */
	UI_PAGE_NOTIFICATION,/* BLE notification flash */
	UI_PAGE_DFU,         /* "waiting for firmware", shown into the reboot */

	UI_PAGE_COUNT
};

const char *ui_page_name(enum ui_page p);

/* Current page. */
enum ui_page ui_current(void);

/*
 * Switch pages. Safe to call with the page already active — it re-enters,
 * which is what makes "press the button again to restart the reveal" work.
 *
 * Must be called from thread/workqueue context, not an ISR: it blanks and
 * commits, which blocks on DMA.
 */
void ui_goto(enum ui_page page);

/* Return to blank. Equivalent to ui_goto(UI_PAGE_BLANK), named for intent. */
void ui_dismiss(void);

/*
 * Stop the current page's auto-return timer, if one is running. Unlike
 * calling ui_goto() again (which blanks, re-enters and restarts the page),
 * this leaves the page exactly as it is — just retroactively cancels its
 * timeout, for information learned after the page was already entered.
 *
 * Used by the battery page: a manual peek gets the normal timeout_ms like
 * any other page, but if the watch turns out to be on power while it's up,
 * battery.c promotes it to the persistent on-power view by cancelling that
 * timeout here rather than re-entering the page (see battery_work_fn()).
 *
 * Must be called from the workqueue that owns ui_goto(), same as ui_goto()
 * itself.
 */
void ui_cancel_timeout(void);

/*
 * Feed a recognised button gesture into the UI. Global bindings (DFU) are
 * handled first, then the active page gets a look.
 */
void ui_handle_button(enum btn_event ev);

/*
 * Start the UI. Call last in boot, after every subsystem it might drive.
 *
 * Deliberately leaves the display blank and off: nothing draws during init,
 * which is what removes the boot flash. Previously time_display_init()
 * published the time into the digit layer while the sand thread was already
 * committing at 30 Hz, so the clock was visibly pushed to the LEDs for the
 * rest of boot before being blanked again.
 */
void ui_init(void);
