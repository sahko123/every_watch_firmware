#include "time_display.h"
#include "led_matrix/led_matrix.h"
#include "sand/sand.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(time_display, LOG_LEVEL_INF);

/*
 * 3×5 pixel font (digits 0-9)
 *
 * Each row is a uint8_t: bit2 = left col, bit1 = mid col, bit0 = right col.
 * Visualised (# = lit, . = dark):
 *
 *   0     1     2     3     4     5     6     7     8     9
 *  ###   .#.   ###   ###   #.#   ###   ###   ###   ###   ###
 *  #.#   ##.   ..#   ..#   #.#   #..   #..   ..#   #.#   #.#
 *  #.#   .#.   ###   ###   ###   ###   ###   ..#   ###   ###
 *  #.#   .#.   #..   ..#   ..#   ..#   #.#   ..#   #.#   ..#
 *  ###   ###   ###   ###   ..#   ###   ###   ..#   ###   ###
 */
static const uint8_t font[10][5] = {
	{0b111, 0b101, 0b101, 0b101, 0b111}, /* 0 */
	{0b010, 0b110, 0b010, 0b010, 0b111}, /* 1 */
	{0b111, 0b001, 0b111, 0b100, 0b111}, /* 2 */
	{0b111, 0b001, 0b111, 0b001, 0b111}, /* 3 */
	{0b101, 0b101, 0b111, 0b001, 0b001}, /* 4 */
	{0b111, 0b100, 0b111, 0b001, 0b111}, /* 5 */
	{0b111, 0b100, 0b111, 0b101, 0b111}, /* 6 */
	{0b111, 0b001, 0b001, 0b001, 0b001}, /* 7 */
	{0b111, 0b101, 0b111, 0b101, 0b111}, /* 8 */
	{0b111, 0b101, 0b111, 0b001, 0b111}, /* 9 */
};

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
	for (int r = 0; r < 5; r++) {
		uint8_t row_bits = font[digit][r];

		for (int c = 0; c < 3; c++) {
			if (row_bits & (0x4 >> c)) {
				out[ROW_OFFSET + r][col_start + c] = 1;
			}
		}
	}
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

static bool read_time(int *hours, int *minutes)
{
	struct rtc_time t = {0};

	if (rtc_dev == NULL || rtc_get_time(rtc_dev, &t) != 0) {
		return false;
	}
	*hours   = t.tm_hour;
	*minutes = t.tm_min;
	return true;
}

static void time_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int h, m;

	if (!read_time(&h, &m)) {
		LOG_WRN("rtc_get_time failed");
		return;
	}

	/* Leave the digit layer alone while a reveal owns it. */
	if (!revealing) {
		build_bitmap(h, m);
		publish_digits();
	}
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
	publish_digits();
}

static void on_curtain_done(void)
{
	revealing = false;

	/* Put the sand colour back — the curtain leaves led_color[] full of
	 * rainbow, and free-mode sand should be amber again. */
	led_color_fill(255, 160, 20);

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

	/* Blank the whole display, not just the digits. Anything left over —
	 * settled sand, a battery warning on the notification layer — would sit
	 * there through the reveal, and the point is that the curtain arrives on
	 * an empty screen. */
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	k_mutex_unlock(&led_mask_mutex);
	sand_clear();
	led_commit();

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
	clear_digits();
	sand_clear();
	sand_set_target(bitmap);
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
