#include "ui.h"

#include "led_matrix/led_matrix.h"
#include "display/display.h"
#include "sand/sand.h"
#include "time_display/time_display.h"
#include "battery/battery.h"
#include "ble/ble.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

/* Fill percentage for the sand toy. 20-30% is the readable band: enough
 * grains to see flow, enough space that they can actually move. */
#define SAND_FILL_PCT 25

/* How long a page stays up with no interaction before returning to blank.
 * Zero means it stays until dismissed. */
#define TIMEOUT_CLOCK_MS   10000
#define TIMEOUT_BATTERY_MS 10000
#define TIMEOUT_NOTIF_MS    5000

struct page_ops {
	const char *name;
	void      (*enter)(void);
	void      (*exit)(void);
	bool        needs_sand;
	uint32_t    timeout_ms;
};

static enum ui_page current = UI_PAGE_BLANK;
static const struct page_ops pages[UI_PAGE_COUNT];

/* -------------------------------------------------------------------------
 * Auto-return timer
 * ------------------------------------------------------------------------- */

static void timeout_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	LOG_INF("page timeout -> blank");
	ui_goto(UI_PAGE_BLANK);
}

static K_WORK_DEFINE(timeout_work, timeout_work_fn);

static void timeout_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&timeout_work);
}

static K_TIMER_DEFINE(timeout_timer, timeout_cb, NULL);

/* -------------------------------------------------------------------------
 * Page implementations
 *
 * Each one only sets up or tears down its own content. None of them touch
 * display_on/off or blank the screen — ui_goto() owns that, so a page cannot
 * accidentally leave the display in a state the next page has to undo.
 * ------------------------------------------------------------------------- */

static void blank_enter(void)
{
	/* Nothing to draw. ui_goto() has already blanked and committed; all
	 * that remains is to drop the panel. */
	display_off();
}

static void clock_enter(void)
{
	display_on();
	time_display_reveal();
}

static void clock_exit(void)
{
	/* Ends the reveal session so the clock cannot reappear on its own the
	 * next time the display happens to come up. */
	time_display_deactivate();
}

static void sand_enter(void)
{
	display_on();
	sand_fill_random(SAND_FILL_PCT);
}

static void sand_exit(void)
{
	sand_clear();
}

static void battery_enter(void)
{
	display_on();
	battery_show_level();
}

static void battery_exit(void)
{
	battery_screen_dismiss();
}

static void notification_enter(void)
{
	display_on();
	/* ble.c owns the content: the category colour is known there and
	 * nowhere else. Called after ui_goto() has blanked, so it always
	 * paints onto a clean layer. */
	ble_paint_notification();
}

/*
 * Paint a "waiting for firmware" pattern and reboot into the bootloader.
 *
 * WS2812Bs latch their last frame and hold it as long as they have power,
 * and nothing refreshes them — so this frame survives the reset and stays lit
 * through MCUboot's entire DFU wait, even though the bootloader never touches
 * the LEDs. It also outlives a failed update: if a transfer dies partway the
 * image is invalid, and the watch sits there still saying "waiting for
 * firmware" rather than looking dead.
 */
static void dfu_enter(void)
{
	display_on();

	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	for (int col = 0; col < LED_COLS; col++) {
		led_mask[LED_LAYER_NOTIFICATION][0][col] = 1;
	}
	led_layer_color[LED_LAYER_NOTIFICATION] = (struct led_rgb){0, 80, 200};
	k_mutex_unlock(&led_mask_mutex);

	led_commit();

	/* led_commit() already waits out the transfer and latch, but give the
	 * frame a little margin before the reset tears the PWM down. */
	k_msleep(30);

	LOG_INF("DFU: rebooting into bootloader");
	sys_reboot(SYS_REBOOT_COLD);
}

static const struct page_ops pages[UI_PAGE_COUNT] = {
	[UI_PAGE_BLANK] = {
		.name = "blank", .enter = blank_enter,
	},
	[UI_PAGE_CLOCK] = {
		.name = "clock", .enter = clock_enter, .exit = clock_exit,
		.needs_sand = true,          /* the curtain reveal is sand */
		.timeout_ms = TIMEOUT_CLOCK_MS,
	},
	[UI_PAGE_SAND] = {
		.name = "sand", .enter = sand_enter, .exit = sand_exit,
		.needs_sand = true,
		/* No timeout: the right button toggles it off. A toy that
		 * blanks mid-play is worse than one you have to dismiss. */
		.timeout_ms = 0,
	},
	[UI_PAGE_BATTERY] = {
		.name = "battery", .enter = battery_enter, .exit = battery_exit,
		.timeout_ms = TIMEOUT_BATTERY_MS,
	},
	[UI_PAGE_NOTIFICATION] = {
		.name = "notification", .enter = notification_enter,
		.timeout_ms = TIMEOUT_NOTIF_MS,
	},
	[UI_PAGE_DFU] = {
		.name = "dfu", .enter = dfu_enter,
	},
};

const char *ui_page_name(enum ui_page p)
{
	return (p < UI_PAGE_COUNT && pages[p].name) ? pages[p].name : "?";
}

enum ui_page ui_current(void)
{
	return current;
}

/* -------------------------------------------------------------------------
 * Transition
 * ------------------------------------------------------------------------- */

/* Serialises transitions. Two gestures arriving close together, or a gesture
 * racing the timeout, would otherwise interleave enter/exit halves. */
static K_MUTEX_DEFINE(ui_mutex);

/*
 * Ramp everything on screen down to black before the panel cuts out.
 *
 * Only on the way to BLANK — that is the one transition the eye reads as the
 * watch finishing, and snapping straight to black made a ten-second page end
 * like a power cut. Page-to-page switches still cut, because there is new
 * content arriving immediately to look at.
 *
 * Runs on the system workqueue, which is also where buttons.c samples its
 * gesture state machine every 15 ms — so this stalls input for its duration.
 * That is why it is this short: 100 ms is about six sample periods, and a
 * deliberate press lasts longer than that, so nothing a user actually means is
 * lost. Do not lengthen it without moving it off this queue.
 *
 * If the sand thread happens to be running it commits its own frames in
 * between these, which is harmless — it reads the same led_fade.
 */
#define FADE_STEPS   5
#define FADE_STEP_MS 20

static void fade_to_black(void)
{
	for (int i = FADE_STEPS - 1; i >= 0; i--) {
		led_fade = (uint8_t)((255 * i) / FADE_STEPS);
		led_commit();
		k_msleep(FADE_STEP_MS);
	}

	/* Restore before anything else draws. The blanking commit that follows
	 * has an empty mask, so it is black either way. */
	led_fade = 255;
}

static void blank_all_layers(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	for (int i = 0; i < LED_LAYER_COUNT; i++) {
		led_layer_color[i] = (struct led_rgb){0, 0, 0};
	}
	k_mutex_unlock(&led_mask_mutex);

	led_commit();
}

void ui_goto(enum ui_page next)
{
	if (next >= UI_PAGE_COUNT) {
		return;
	}

	k_mutex_lock(&ui_mutex, K_FOREVER);

	const struct page_ops *from = &pages[current];
	const struct page_ops *to   = &pages[next];

	LOG_INF("page %s -> %s", from->name, to->name);

	k_timer_stop(&timeout_timer);

	if (from->exit) {
		from->exit();
	}

	/* Fade out rather than cut, but only when the display is actually going
	 * away and there is something lit to fade. */
	if (next == UI_PAGE_BLANK && current != UI_PAGE_BLANK && display_is_on()) {
		fade_to_black();
	}

	/*
	 * Blank between every page, unconditionally — including when the next
	 * page draws immediately. It costs one frame and removes a whole class
	 * of bug where a page inherits pixels from its predecessor, which is
	 * exactly how the battery screen and the time reveal used to corrupt
	 * each other.
	 */
	blank_all_layers();

	/*
	 * Sand runs only for pages that need it. Ordering matters: resume
	 * before enter() so a page can seed grains, and suspend after exit()
	 * so a page can clear them first.
	 */
	if (to->needs_sand && !from->needs_sand) {
		sand_resume();
	}

	current = next;

	if (to->enter) {
		to->enter();
	}

	if (!to->needs_sand && from->needs_sand) {
		sand_suspend();
	}

	if (to->timeout_ms) {
		k_timer_start(&timeout_timer, K_MSEC(to->timeout_ms), K_NO_WAIT);
	}

	k_mutex_unlock(&ui_mutex);
}

void ui_dismiss(void)
{
	ui_goto(UI_PAGE_BLANK);
}

/* -------------------------------------------------------------------------
 * Button bindings
 * ------------------------------------------------------------------------- */

void ui_handle_button(enum btn_event ev)
{
	LOG_DBG("button %s on page %s", btn_event_name(ev), ui_page_name(current));

	switch (ev) {
	/* Global: available from any page, including blank. Both buttons held
	 * together — deliberately awkward, since it reboots the watch. */
	case BTN_EV_BOTH_HOLD:
		ui_goto(UI_PAGE_DFU);
		return;

	case BTN_EV_L_SINGLE:
		/* Re-entering CLOCK restarts the reveal, which is the wanted
		 * behaviour for pressing it again. */
		ui_goto(UI_PAGE_CLOCK);
		return;

	case BTN_EV_R_SINGLE:
		/* Toggle: same button takes the toy away again. */
		ui_goto(current == UI_PAGE_SAND ? UI_PAGE_BLANK : UI_PAGE_SAND);
		return;

	case BTN_EV_R_HOLD:
		ui_goto(UI_PAGE_BATTERY);
		return;

	/* Recognised by the driver and deliberately unbound for now, rather
	 * than guessing at a UX. Adding a binding is one case each. */
	case BTN_EV_L_DOUBLE:
	case BTN_EV_R_DOUBLE:
	case BTN_EV_L_HOLD:
	case BTN_EV_BOTH_PRESS:
		LOG_INF("unbound gesture: %s", btn_event_name(ev));
		return;

	default:
		return;
	}
}

void ui_init(void)
{
	current = UI_PAGE_BLANK;

	/*
	 * Start from a known-blank display with the sand thread parked.
	 * sand_init() creates the thread already running, and every page that
	 * does not need it expects it stopped.
	 */
	sand_suspend();
	blank_all_layers();
	display_off();

	LOG_INF("UI ready (page: blank)");
}
