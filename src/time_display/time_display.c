#include "time_display.h"
#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "battery/battery.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(time_display, LOG_LEVEL_INF);

/*
 * The 3x5 digit font now lives in led_matrix.c as led_font_3x5, shared with the
 * battery readout. Visualised (# = lit, . = dark):
 *
 *   0     1     2     3     4     5     6     7     8     9
 *  ###   .#.   ###   ###   #.#   ###   ###   ###   ###   ###
 *  #.#   ##.   ..#   ..#   #.#   #..   #..   ..#   #.#   #.#
 *  #.#   .#.   ###   ###   ###   ###   ###   ..#   ###   ###
 *  #.#   .#.   #..   ..#   ..#   ..#   #.#   ..#   #.#   ..#
 *  ###   ###   ###   ###   ..#   ###   ###   ..#   ###   ###
 */

/*
 * HH:MM layout on the 7×20 grid.
 *
 * Font is 5 rows tall, placed at display rows 1-5 (1px margin top/bottom).
 * Column positions (digit = 3 wide):
 *
 *   col:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19
 *         _  [H tens ]  _  [H units]  _  :  _  [M tens ]  _  [M units]  _  _
 */
#define ROW_OFFSET  1  /* font row 0 → display row 1 */
#define D0_COL      1  /* H tens  */
#define D1_COL      5  /* H units */
#define COLON_COL   9
#define D2_COL     11  /* M tens  */
#define D3_COL     15  /* M units */

/* Scratch bitmap. Built once and then either copied into the digit mask (for a
 * plain static display) or handed to the sand simulation as a fill target (for
 * the waterfall reveal). */
static uint8_t bitmap[LED_ROWS][LED_COLS];

static void stamp_digit(uint8_t out[LED_ROWS][LED_COLS], int digit, int col_start)
{
	led_stamp_digit(out, digit, ROW_OFFSET, col_start);
}

static void stamp_colon(uint8_t out[LED_ROWS][LED_COLS], int col)
{
	/* Two dots at font rows 1 and 3 */
	out[ROW_OFFSET + 1][col] = 1;
	out[ROW_OFFSET + 3][col] = 1;
}

/* Render HH:MM into the scratch bitmap. Caller decides what to do with it. */
static void build_bitmap(int hours, int minutes)
{
	memset(bitmap, 0, sizeof(bitmap));
	stamp_digit(bitmap, hours   / 10, D0_COL);
	stamp_digit(bitmap, hours   % 10, D1_COL);
	stamp_colon(bitmap, COLON_COL);
	stamp_digit(bitmap, minutes / 10, D2_COL);
	stamp_digit(bitmap, minutes % 10, D3_COL);
}

static void publish_digits(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	memcpy(led_mask[LED_LAYER_DIGITS], bitmap, sizeof(bitmap));
	k_mutex_unlock(&led_mask_mutex);
}

static void clear_digits(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear(LED_LAYER_DIGITS);
	k_mutex_unlock(&led_mask_mutex);
}

/* --------------------------------------------------------------------------
 * 1 Hz update via timer → work queue (I2C must not run in ISR context)
 * -------------------------------------------------------------------------- */

static const struct device *rtc_dev;
static bool revealing;

/* True only while a reveal session is live — set by time_display_reveal[_fill]()
 * and cleared by time_display_deactivate(). Gates the periodic tick below so
 * time can only appear on screen as the result of an explicit reveal, never as
 * a side effect of the display waking up for something else. */
static bool active;

/* Set while another screen owns the display (the battery readout). The 1 Hz
 * tick keeps running and keeps reading the RTC, it just stops republishing the
 * digit layer — otherwise the time would fade up behind the other screen within
 * a second of it appearing. */
static bool paused;

/*
 * Placeholder written back to the RTC whenever it reports VLF (voltage-low
 * flag) — an arbitrary but validly-encodable date, chosen only so the write
 * succeeds. The FRTC8900 sets VLF itself, in hardware, whenever its own
 * supply dips below ~1.6 V or the oscillator drops out (see rtc_frtc8900.c);
 * once set, every future rtc_get_time() keeps returning -ENODATA forever,
 * by design, until something calls rtc_set_time() — which is also the only
 * thing that clears VLF. Without this, one power dip permanently blanks the
 * clock until someone happens to run `settime` over USB, with no on-screen
 * sign of why. 00:00 matches the placeholder time_display_init() already
 * shows for the "never been set" case, so a VLF recovery looks the same as
 * a fresh unconfigured RTC rather than a new, different failure mode.
 */
static const struct rtc_time RTC_RESET_DEFAULT = {
	.tm_sec = 0, .tm_min = 0, .tm_hour = 0,
	.tm_mday = 1, .tm_mon = 0, .tm_year = 100, /* 2000-01-01, a Saturday */
	.tm_wday = 6,
};

static bool read_time(int *hours, int *minutes)
{
	struct rtc_time t = {0};
	int ret;

	if (rtc_dev == NULL) {
		return false;
	}

	ret = rtc_get_time(rtc_dev, &t);

	if (ret == -ENODATA) {
		LOG_WRN("RTC lost power (VLF set) -- resetting to a"
			" placeholder; run settime to correct it");
		if (rtc_set_time(rtc_dev, &RTC_RESET_DEFAULT) == 0) {
			t   = RTC_RESET_DEFAULT;
			ret = 0;
		}
	}

	if (ret != 0) {
		return false;
	}

	*hours   = t.tm_hour;
	*minutes = t.tm_min;
	return true;
}

static void time_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Leave the digit layer alone unless a reveal session is live, and not
	 * mid-reveal or while another screen owns the display. Checked before
	 * the RTC read, not after: this tick runs forever at 1 Hz regardless
	 * of display state, and reading over I2C for a result that's about to
	 * be thrown away is a real, easily-avoided cost against this board's
	 * idle-current target — it used to happen on every tick, every
	 * second, for the device's entire life on battery. */
	if (!active || revealing || paused) {
		return;
	}

	int h, m;

	if (!read_time(&h, &m)) {
		LOG_WRN("rtc_get_time failed");
		return;
	}

	build_bitmap(h, m);
	publish_digits();
}

K_WORK_DEFINE(time_work, time_work_fn);

static void time_timer_cb(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&time_work);
}

K_TIMER_DEFINE(time_timer, time_timer_cb, NULL);

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/* Fired by the simulation the moment the curtain reaches full cover. The
 * digits go into LED_LAYER_DIGITS, which sits below LED_LAYER_SAND in the
 * compositor, so they stay hidden under the sand and are simply left behind
 * as it thins out. */
static void on_curtain_peak(void)
{
	/* Guard against a stale reveal: display_off() suspends the sand
	 * thread mid-animation rather than cancelling it (see
	 * time_display_deactivate() below), so a curtain that was mid-flight
	 * when the display went off can still reach its peak later, once
	 * something unrelated (e.g. a battery-screen wake) resumes sand.
	 * Without this check that silently painted time into LED_LAYER_DIGITS
	 * with no explicit reveal — the exact bug the `active` flag exists to
	 * prevent, just reached through this callback instead of the 1 Hz
	 * tick above. */
	if (active) {
		publish_digits();
	}
}

static void on_curtain_done(void)
{
	revealing = false;

	/*
	 * Deliberately leave led_color[] holding the curtain's rainbow.
	 *
	 * This used to led_color_fill() amber here, to put free-mode sand back
	 * to its normal colour. The side effect was that the digits changed
	 * colour a second or two after appearing: LED_LAYER_DIGITS has no
	 * layer_color, so composite() reads led_color[] per cell, and the
	 * digits were being revealed in rainbow and then repainted amber
	 * underneath the viewer.
	 *
	 * Masking the fill around the digit cells would fix the visible symptom
	 * but not the underlying one — the 1 Hz tick republishes the bitmap, so
	 * a minute rolling over mid-view lights cells that were background a
	 * moment ago, and those would come up amber against rainbow neighbours.
	 * Leaving the rainbow alone keeps every digit cell the same colour
	 * whenever it lights.
	 *
	 * Nothing else depends on the amber: the sand toy sets its own per-grain
	 * hues, and the battery screen restores led_color[] when it dismisses.
	 * The only cells this affects are grains left behind by the curtain,
	 * which now stay the colour of the curtain they fell from.
	 *
	 * The wave keeps that rainbow drifting rather than freezing it on
	 * whatever phase the curtain stopped at. It picks the phase up from
	 * where the curtain left it, so the handover is invisible.
	 */
	sand_set_hue_wave(true);

	LOG_INF("Time reveal complete");
}

void time_display_reveal(void)
{
	int h, m;

	if (!read_time(&h, &m)) {
		LOG_WRN("reveal: RTC unreadable");
		return;
	}

	build_bitmap(h, m);

	/* Tear down a battery screen first if one is up — otherwise its
	 * timers (wave_timer repainting led_color[] every 60ms, level_timer)
	 * keep running underneath the reveal, and its call into
	 * time_display_pause() would leave `paused` stuck true for the rest
	 * of a charging session since nothing but level_dismiss() ever clears
	 * it. */
	battery_screen_dismiss();

	/* Blank the whole display, not just the digits. Anything left over —
	 * settled sand, a battery warning on the notification layer — would sit
	 * there through the reveal, and the point is that the curtain arrives on
	 * an empty screen. */
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	k_mutex_unlock(&led_mask_mutex);
	sand_clear();
	led_commit();

	active    = true;
	revealing = true;
	sand_rain_start(on_curtain_peak, on_curtain_done);

	LOG_INF("Curtain reveal started: %02d:%02d", h, m);
}

/*
 * The earlier reveal: particles stream only into the columns that contain
 * digits and lock into the shape, building it from the bottom up. Kept because
 * it is a good effect in its own right — a candidate for a second display mode
 * or a long-press action rather than the default.
 */
void time_display_reveal_fill(void)
{
	int h, m;

	if (!read_time(&h, &m)) {
		LOG_WRN("reveal: RTC unreadable");
		return;
	}

	build_bitmap(h, m);
	battery_screen_dismiss();
	clear_digits();
	sand_clear();
	sand_set_target(bitmap);
	active    = true;
	revealing = true;

	LOG_INF("Fill reveal started: %02d:%02d", h, m);
}

void time_display_stop_reveal(void)
{
	if (!revealing) {
		return;
	}
	revealing = false;
	sand_set_target(NULL);
	LOG_INF("Time reveal ended");
}

bool time_display_revealing(void)
{
	return revealing;
}

void time_display_pause(void)
{
	paused = true;
}

void time_display_resume(void)
{
	if (!paused) {
		return;
	}
	paused = false;

	/* Redraw immediately rather than leaving the display blank for up to a
	 * second until the next tick. */
	k_work_submit(&time_work);
}

void time_display_deactivate(void)
{
	active = false;

	/* Stop the drift however the page ends, not just on a completed
	 * reveal — it rewrites led_color[] on every sand tick, so leaving it
	 * running would bleed the clock's colours through whatever page comes
	 * next. Harmless if it was never started. */
	sand_set_hue_wave(false);

	/* A reveal still in flight when the display goes off must be
	 * cancelled outright, not just left to freeze with the sand thread
	 * (display_off() suspends sand right before calling this). Otherwise
	 * it silently resumes animating — visibly, in LED_LAYER_SAND — the
	 * next time anything unrelated resumes sand, even though on_curtain_
	 * peak() above is now guarded against actually publishing digits from
	 * it. sand_set_target(NULL) forces mode back to free regardless of
	 * whether the reveal was rain or constrained. */
	if (revealing) {
		time_display_stop_reveal();
		led_color_reset();
	}
}

void time_display_init(const struct device *dev)
{
	rtc_dev = dev;

	/* Render immediately so something is on screen before the first tick */
	int h, m;

	if (read_time(&h, &m)) {
		build_bitmap(h, m);
	} else {
		/* RTC not set yet — show 00:00 as a placeholder */
		build_bitmap(0, 0);
	}
	publish_digits();

	k_timer_start(&time_timer, K_SECONDS(1), K_SECONDS(1));
	LOG_INF("Time display started");
}
