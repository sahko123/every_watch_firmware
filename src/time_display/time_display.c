#include "time_display.h"
#include "led_matrix/led_matrix.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>

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

static void stamp_digit(int digit, int col_start)
{
	for (int r = 0; r < 5; r++) {
		uint8_t row_bits = font[digit][r];

		for (int c = 0; c < 3; c++) {
			if (row_bits & (0x4 >> c)) {
				led_mask[LED_LAYER_DIGITS][ROW_OFFSET + r][col_start + c] = 1;
			}
		}
	}
}

static void stamp_colon(int col)
{
	/* Two dots at font rows 1 and 3 */
	led_mask[LED_LAYER_DIGITS][ROW_OFFSET + 1][col] = 1;
	led_mask[LED_LAYER_DIGITS][ROW_OFFSET + 3][col] = 1;
}

static void render_time(int hours, int minutes)
{
	led_mask_clear(LED_LAYER_DIGITS);
	stamp_digit(hours   / 10, D0_COL);
	stamp_digit(hours   % 10, D1_COL);
	stamp_colon(COLON_COL);
	stamp_digit(minutes / 10, D2_COL);
	stamp_digit(minutes % 10, D3_COL);
}

/* --------------------------------------------------------------------------
 * 1 Hz update via timer → work queue (I2C must not run in ISR context)
 * -------------------------------------------------------------------------- */

static const struct device *rtc_dev;

static void time_work_fn(struct k_work *work)
{
	struct rtc_time t = {0};
	int err = rtc_get_time(rtc_dev, &t);

	if (err) {
		LOG_WRN("rtc_get_time failed: %d", err);
		return;
	}

	render_time(t.tm_hour, t.tm_min);
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

void time_display_init(const struct device *dev)
{
	rtc_dev = dev;

	/* Render immediately so digits appear before sand settles */
	struct rtc_time t = {0};

	if (rtc_get_time(dev, &t) == 0) {
		render_time(t.tm_hour, t.tm_min);
	} else {
		/* RTC not set yet — show 00:00 as a placeholder */
		render_time(0, 0);
	}

	k_timer_start(&time_timer, K_SECONDS(1), K_SECONDS(1));
	LOG_INF("Time display started");
}
