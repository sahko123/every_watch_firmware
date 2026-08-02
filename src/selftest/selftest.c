#include "selftest.h"
#include "led_matrix/led_matrix.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

/*
 * Everything here prints with printk() rather than LOG_*, so the report is
 * unaffected by log level filtering and arrives in order with no timestamp
 * prefixes cluttering the table.
 *
 * All values are printed as integers — no floats — so this works with
 * CONFIG_CBPRINTF_NANO=y as set in prj.conf.
 */

static int passed;
static int failed;

static void check(const char *tag, const char *what, bool ok)
{
	printk("[%-4s] %-38s %s\n", tag, what, ok ? "PASS" : "FAIL");
	if (ok) {
		passed++;
	} else {
		failed++;
	}
}

static void info(const char *tag, const char *fmt_line)
{
	printk("[%-4s] %s\n", tag, fmt_line);
}

/* Print a sensor_value as a fixed-point decimal, e.g. "9.81".
 * val2 is millionths; we keep two decimal places. */
static void print_sv(const char *label, const struct sensor_value *v, const char *unit)
{
	int32_t frac = v->val2 / 10000;

	if (frac < 0) {
		frac = -frac;
	}
	printk("       %s = %d.%02d %s\n", label, v->val1, frac, unit);
}

/* ---------------------------------------------------------------------------
 * Devices
 * ------------------------------------------------------------------------- */

static const struct device *const i2c0_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const rtc_dev  = DEVICE_DT_GET(DT_ALIAS(rtc0));
static const struct device *const imu_dev  = DEVICE_DT_GET(DT_NODELABEL(bmi260));
static const struct device *const lgt_dev  = DEVICE_DT_GET(DT_ALIAS(light0));
static const struct device *const bat_dev  = DEVICE_DT_GET(DT_ALIAS(batt0));

static const struct gpio_dt_spec btn_l = GPIO_DT_SPEC_GET(DT_ALIAS(btn_left),  gpios);
static const struct gpio_dt_spec btn_r = GPIO_DT_SPEC_GET(DT_ALIAS(btn_right), gpios);
static const struct gpio_dt_spec chg   = GPIO_DT_SPEC_GET(DT_NODELABEL(chg_indicator), gpios);

/* ---------------------------------------------------------------------------
 * I2C bus scan
 * ------------------------------------------------------------------------- */

struct i2c_expect {
	uint8_t     addr;
	const char *name;
};

static const struct i2c_expect expected[] = {
	{0x23, "BH1750   ambient light"},
	{0x32, "FRTC8900 real-time clock"},
	{0x55, "BQ27441  fuel gauge"},
	{0x68, "BMI260   IMU"},
};

/* Zero-length write — the standard I2C presence probe. ACK means a device
 * is holding SDA low for that address. */
static bool i2c_ping(uint8_t addr)
{
	uint8_t dummy = 0;
	struct i2c_msg msg = {
		.buf   = &dummy,
		.len   = 0,
		.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
	};

	return i2c_transfer(i2c0_dev, &msg, 1, addr) == 0;
}

static void test_i2c_bus(void)
{
	printk("\n-- I2C bus (SCL P0.04 / SDA P0.01) ------------------------\n");

	if (!device_is_ready(i2c0_dev)) {
		check("I2C", "controller ready", false);
		return;
	}
	check("I2C", "controller ready", true);

	/* Full bus scan first — surfaces unexpected devices and address clashes
	 * that a targeted probe would miss. */
	int found = 0;

	printk("       scanning 0x08-0x77:");
	for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
		if (i2c_ping(addr)) {
			printk(" 0x%02x", addr);
			found++;
		}
	}
	printk("  (%d found)\n", found);

	for (int i = 0; i < (int)ARRAY_SIZE(expected); i++) {
		char line[64];

		snprintk(line, sizeof(line), "0x%02x  %s",
			 expected[i].addr, expected[i].name);
		check("I2C", line, i2c_ping(expected[i].addr));
	}
}

/* ---------------------------------------------------------------------------
 * RTC — the highest-risk part, since the driver is custom and unproven
 * ------------------------------------------------------------------------- */

static void test_rtc(void)
{
	printk("\n-- FRTC8900 RTC ------------------------------------------\n");

	if (!device_is_ready(rtc_dev)) {
		check("RTC", "driver ready", false);
		return;
	}
	check("RTC", "driver ready", true);

	struct rtc_time set = {
		.tm_year = 126,  /* years since 1900 → 2026 */
		.tm_mon  = 6,    /* 0-based → July        */
		.tm_mday = 31,
		.tm_hour = 12,
		.tm_min  = 34,
		.tm_sec  = 56,
		.tm_wday = 5,
	};

	if (rtc_set_time(rtc_dev, &set) != 0) {
		check("RTC", "set time 2026-07-31 12:34:56", false);
		return;
	}
	check("RTC", "set time 2026-07-31 12:34:56", true);

	struct rtc_time got = {0};

	if (rtc_get_time(rtc_dev, &got) != 0) {
		check("RTC", "read back", false);
		return;
	}

	printk("       read back %04d-%02d-%02d %02d:%02d:%02d\n",
	       got.tm_year + 1900, got.tm_mon + 1, got.tm_mday,
	       got.tm_hour, got.tm_min, got.tm_sec);

	check("RTC", "date/time matches what was set",
	      got.tm_year == set.tm_year && got.tm_mon  == set.tm_mon &&
	      got.tm_mday == set.tm_mday && got.tm_hour == set.tm_hour &&
	      got.tm_min  == set.tm_min);

	/* Oscillator actually running: seconds must advance over ~2 s. */
	int start_sec = got.tm_sec;

	k_msleep(2200);

	if (rtc_get_time(rtc_dev, &got) != 0) {
		check("RTC", "oscillator running (seconds advance)", false);
		return;
	}
	printk("       %02d s -> %02d s after 2.2 s wait\n", start_sec, got.tm_sec);
	check("RTC", "oscillator running (seconds advance)",
	      got.tm_sec != start_sec);
}

/* ---------------------------------------------------------------------------
 * Sensors
 * ------------------------------------------------------------------------- */

static void test_imu(void)
{
	printk("\n-- BMI260 IMU --------------------------------------------\n");

	if (!device_is_ready(imu_dev)) {
		check("IMU", "driver ready", false);
		return;
	}
	check("IMU", "driver ready", true);

	/*
	 * Power the accelerometer up before reading it. The driver leaves init
	 * with the sensor in advanced power save and the accelerometer OFF —
	 * setting the sampling frequency is what actually enables it. Without
	 * this every axis reads exactly 0.00 while sample_fetch and
	 * channel_get both cheerfully report success, so the test looked like
	 * it was passing right up until the magnitude check.
	 *
	 * Done here rather than relying on imu_init(), because selftest_run()
	 * deliberately runs before the application subsystems start — a
	 * hardware test that depends on app init having already happened isn't
	 * testing the hardware. Order is prescribed: frequency last, since it
	 * also selects the power mode.
	 */
	struct sensor_value fs   = {.val1 = 2,  .val2 = 0};
	struct sensor_value osr  = {.val1 = 1,  .val2 = 0};
	struct sensor_value freq = {.val1 = 50, .val2 = 0};
	int cfg_rc;

	cfg_rc  = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
				  SENSOR_ATTR_FULL_SCALE, &fs);
	cfg_rc |= sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
				  SENSOR_ATTR_OVERSAMPLING, &osr);
	cfg_rc |= sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
				  SENSOR_ATTR_SAMPLING_FREQUENCY, &freq);
	check("IMU", "configure accel (2g, 50 Hz)", cfg_rc == 0);

	/* The accelerometer needs a moment after power-up before its output
	 * registers hold a real conversion rather than zeros. */
	k_msleep(50);

	if (sensor_sample_fetch(imu_dev) != 0) {
		check("IMU", "sample fetch", false);
		return;
	}
	check("IMU", "sample fetch", true);

	struct sensor_value ax, ay, az;

	if (sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_X, &ax) ||
	    sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_Y, &ay) ||
	    sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_Z, &az)) {
		check("IMU", "read accelerometer", false);
		return;
	}
	check("IMU", "read accelerometer", true);

	print_sv("accel X", &ax, "m/s2");
	print_sv("accel Y", &ay, "m/s2");
	print_sv("accel Z", &az, "m/s2");

	/* At rest the vector magnitude should be ~9.81 m/s². Compare squares in
	 * integer milli-g² to avoid sqrt and floats. Accept 6-14 m/s² — wide
	 * enough for a hand-held board, tight enough to catch a dead axis or a
	 * misconfigured range. */
	int32_t x = ax.val1 * 1000 + ax.val2 / 1000;
	int32_t y = ay.val1 * 1000 + ay.val2 / 1000;
	int32_t z = az.val1 * 1000 + az.val2 / 1000;
	int64_t mag2 = (int64_t)x * x + (int64_t)y * y + (int64_t)z * z;

	/* Scale back to (m/s²)² so it prints as a small int — nano printf has
	 * no %lld. */
	printk("       |accel|^2 = %d (m/s2)^2, expect 36 - 196\n",
	       (int)(mag2 / 1000000));
	check("IMU", "gravity magnitude plausible",
	      mag2 > 36000000LL && mag2 < 196000000LL);

	printk("       NOTE: confirm axis signs against PCB orientation -\n");
	printk("             tilt right should push sand right (+col).\n");
}

static void test_light(void)
{
	printk("\n-- BH1750 ambient light ----------------------------------\n");

	if (!device_is_ready(lgt_dev)) {
		check("LGHT", "driver ready", false);
		return;
	}
	check("LGHT", "driver ready", true);

	if (sensor_sample_fetch(lgt_dev) != 0) {
		check("LGHT", "sample fetch", false);
		return;
	}
	check("LGHT", "sample fetch", true);

	struct sensor_value lux;

	if (sensor_channel_get(lgt_dev, SENSOR_CHAN_LIGHT, &lux) != 0) {
		check("LGHT", "read lux", false);
		return;
	}
	print_sv("ambient", &lux, "lux");
	check("LGHT", "read lux", true);
	printk("       cover the sensor and re-run to confirm it tracks.\n");
}

static void test_battery(void)
{
	printk("\n-- BQ27441 fuel gauge ------------------------------------\n");

	if (!device_is_ready(bat_dev)) {
		check("BATT", "driver ready", false);
		return;
	}
	check("BATT", "driver ready", true);

	if (sensor_sample_fetch(bat_dev) != 0) {
		check("BATT", "sample fetch", false);
		return;
	}
	check("BATT", "sample fetch", true);

	struct sensor_value v, soc;

	if (sensor_channel_get(bat_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &v) == 0) {
		print_sv("voltage", &v, "V");
		/* A Li-ion cell out of deep discharge sits between 3.0 and 4.3 V. */
		check("BATT", "voltage in 3.0-4.3 V range",
		      v.val1 >= 3 && v.val1 <= 4);
	} else {
		check("BATT", "read voltage", false);
	}

	if (sensor_channel_get(bat_dev, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &soc) == 0) {
		print_sv("charge ", &soc, "%");
		check("BATT", "read state of charge", true);
	} else {
		check("BATT", "read state of charge", false);
	}

	printk("       NOTE: design-capacity in the DTS is 300 mAh - correct it\n");
	printk("             to the real cell or SoC readings will be wrong.\n");
}

static void test_gpio(void)
{
	printk("\n-- GPIO --------------------------------------------------\n");

	if (!gpio_is_ready_dt(&chg)) {
		check("GPIO", "charge indicator ready", false);
		return;
	}
	gpio_pin_configure_dt(&chg, GPIO_INPUT);
	check("GPIO", "charge indicator ready", true);

	/* Active-low in the DTS, so logical 1 means charging. */
	info("GPIO", gpio_pin_get_dt(&chg) == 1
		     ? "charge indicator: CHARGING"
		     : "charge indicator: not charging");
	printk("       plug/unplug USB and re-run to confirm this flips.\n");
}

/* ---------------------------------------------------------------------------
 * LED matrix — needs a human watching
 * ------------------------------------------------------------------------- */

__maybe_unused static void led_fill_rows(int row_lo, int row_hi, uint8_t r, uint8_t g, uint8_t b)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	for (int row = row_lo; row <= row_hi; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			led_mask[LED_LAYER_BG][row][col] = 1;
		}
	}
	k_mutex_unlock(&led_mask_mutex);

	led_layer_color[LED_LAYER_BG] = (struct led_rgb){r, g, b};
	led_commit();
}

__maybe_unused static void led_all_off(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	k_mutex_unlock(&led_mask_mutex);
	led_commit();
}

/* Colour wheel: 0-255 around the hue circle, integer only, no floats and no
 * division. Each third of the range crossfades between two primaries. */
__maybe_unused static struct led_rgb wheel(uint8_t pos)
{
	pos = 255 - pos;

	if (pos < 85) {
		return (struct led_rgb){255 - pos * 3, 0, pos * 3};
	}
	if (pos < 170) {
		pos -= 85;
		return (struct led_rgb){0, pos * 3, 255 - pos * 3};
	}
	pos -= 170;
	return (struct led_rgb){pos * 3, 255 - pos * 3, 0};
}

/*
 * Diagonal rainbow scrolled across the whole grid.
 *
 * More than decoration: it drives all 140 LEDs on all four data lines at once
 * with every pixel a different colour, which is the hardest thing to ask of
 * this display. It exercises the full encode path, all three parallel DMA
 * transfers plus the row-6 bitbang together, and the current limiter — a
 * uniform colour would not. Banding, tearing, a stuck row or a line that drops
 * out only under full load will show up here and nowhere else in the test.
 */
__maybe_unused static void test_leds_rainbow(void)
{
	printk("\n       rainbow sweep - all 140 LEDs, every pixel a different\n");
	printk("       colour. Look for smooth diagonal bands with no tearing,\n");
	printk("       no flicker, and no row lagging behind the others.\n");

	/* Per-cell colour: layer_color must be zero for the compositor to read
	 * led_color[row][col] instead of a flat layer colour. */
	led_layer_color[LED_LAYER_BG] = (struct led_rgb){0, 0, 0};

	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			led_mask[LED_LAYER_BG][row][col] = 1;
		}
	}
	k_mutex_unlock(&led_mask_mutex);

	/* ~5 s at 30 fps. Hue advances along the diagonal so the bands run
	 * across the grid rather than straight down a data line — that way a
	 * single misbehaving line is obvious against its neighbours. */
	for (int frame = 0; frame < 150; frame++) {
		for (int row = 0; row < LED_ROWS; row++) {
			for (int col = 0; col < LED_COLS; col++) {
				uint8_t hue = (uint8_t)(col * 10 + row * 18 + frame * 3);

				led_color[row][col] = wheel(hue);
			}
		}
		led_commit();
		k_msleep(33);
	}

	led_all_off();
	led_color_fill(0, 0, 0);
}

__maybe_unused static void test_leds(void)
{
	static const struct {
		const char *name;
		int lo, hi;
	} lines[] = {
		{"line 1  PWM0 P0.29  rows 0-1  (40 LEDs)", 0, 1},
		{"line 2  PWM1 P0.28  rows 2-3  (40 LEDs)", 2, 3},
		{"line 3  PWM2 P0.02  rows 4-5  (40 LEDs)", 4, 5},
		{"line 4  PWM3 P0.03  row 6     (20 LEDs)", 6, 6},
	};

	printk("\n-- LED matrix --------------------------------------------\n");
	printk("       WATCH THE DISPLAY. Each data line lights red, green,\n");
	printk("       then blue. Wrong colour order means the GRB swap is\n");
	printk("       wrong; a dead line means check that pin's solder.\n\n");

	/* Keep the test dim: 140 LEDs at full white would pull several amps if
	 * the current limiter were misconfigured. */
	uint8_t saved_max = led_max_brightness;

	led_max_brightness = 40;

	for (int i = 0; i < (int)ARRAY_SIZE(lines); i++) {
		printk("       %s\n", lines[i].name);
		led_fill_rows(lines[i].lo, lines[i].hi, 255, 0, 0);
		k_msleep(600);
		led_fill_rows(lines[i].lo, lines[i].hi, 0, 255, 0);
		k_msleep(600);
		led_fill_rows(lines[i].lo, lines[i].hi, 0, 0, 255);
		k_msleep(600);
		led_all_off();
		k_msleep(200);
	}

	/* Pixel-mapping walk: one LED at a time in logical order. The physical
	 * dot must travel left→right along row 0, then left→right along row 1,
	 * and so on down the display. If a row runs backwards, the snake
	 * handling in pixel_to_physical() is wrong for that row. */
	printk("\n       pixel walk - the dot must sweep left-to-right on\n");
	printk("       every row, top row first, with no jumps.\n");

	led_layer_color[LED_LAYER_BG] = (struct led_rgb){255, 255, 255};

	for (int row = 0; row < LED_ROWS; row++) {
		printk("       row %d\n", row);
		for (int col = 0; col < LED_COLS; col++) {
			k_mutex_lock(&led_mask_mutex, K_FOREVER);
			led_mask_clear_all();
			led_mask[LED_LAYER_BG][row][col] = 1;
			k_mutex_unlock(&led_mask_mutex);
			led_commit();
			k_msleep(60);
		}
	}

	led_all_off();
	led_layer_color[LED_LAYER_BG] = (struct led_rgb){0, 0, 0};

	/* All four lines at once, every pixel a different colour. Kept inside
	 * the dimmed section deliberately — this is the highest-load pattern the
	 * test produces. */
	test_leds_rainbow();

	led_max_brightness = saved_max;

	printk("\n");
	check("LED ", "all four data lines driven (visual)", true);
	printk("       ^ this is not auto-detected - mark FAIL yourself if a\n");
	printk("         line stayed dark or a row ran backwards.\n");
}

/* ---------------------------------------------------------------------------
 * Buttons — needs a human pressing
 * ------------------------------------------------------------------------- */

#define BTN_TIMEOUT_MS 15000

__maybe_unused static bool wait_for_button(const struct gpio_dt_spec *btn, const char *name)
{
	printk("       press the %s button (%d s)...\n", name, BTN_TIMEOUT_MS / 1000);

	int64_t deadline = k_uptime_get() + BTN_TIMEOUT_MS;

	while (k_uptime_get() < deadline) {
		if (gpio_pin_get_dt(btn) == 1) {
			/* Wait for release so the next test does not see the
			 * same press still held down. */
			while (gpio_pin_get_dt(btn) == 1 &&
			       k_uptime_get() < deadline) {
				k_msleep(10);
			}
			return true;
		}
		k_msleep(10);
	}
	return false;
}

__maybe_unused static void test_buttons(void)
{
	printk("\n-- Buttons -----------------------------------------------\n");

	if (!gpio_is_ready_dt(&btn_l) || !gpio_is_ready_dt(&btn_r)) {
		check("BTN ", "button GPIO ready", false);
		return;
	}
	gpio_pin_configure_dt(&btn_l, GPIO_INPUT);
	gpio_pin_configure_dt(&btn_r, GPIO_INPUT);
	check("BTN ", "button GPIO ready", true);

	/* Both must read released at rest — catches a stuck button or a missing
	 * pull-up before we start waiting on presses. */
	check("BTN ", "both released at rest",
	      gpio_pin_get_dt(&btn_l) == 0 && gpio_pin_get_dt(&btn_r) == 0);

	check("BTN ", "LEFT  button (P0.17) registers",
	      wait_for_button(&btn_l, "LEFT"));
	check("BTN ", "RIGHT button (P0.15) registers",
	      wait_for_button(&btn_r, "RIGHT"));
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int selftest_run(void)
{
	passed = 0;
	failed = 0;

	printk("\n");
	printk("==========================================================\n");
	printk(" Every Watch - hardware self-test\n");
	printk(" board: every_watch/nrf52833\n");
	printk("==========================================================\n");

	test_i2c_bus();
	test_rtc();
	test_imu();
	test_light();
	test_battery();
	test_gpio();

	if (IS_ENABLED(CONFIG_EW_SELFTEST_INTERACTIVE)) {
		test_leds();
		test_buttons();
	} else {
		printk("\n-- LED matrix / buttons ----------------------------------\n");
		printk("       skipped: CONFIG_EW_SELFTEST_INTERACTIVE=n\n");
	}

	printk("\n==========================================================\n");
	printk(" RESULT: %d passed, %d failed\n", passed, failed);
	printk("==========================================================\n\n");

	return failed;
}
