#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "time_display/time_display.h"
#include "display/display.h"
#include "imu/imu.h"
#include "identity/identity.h"
#include "ble/ble.h"
#include "battery/battery.h"
#include "light/light.h"

#ifdef CONFIG_EW_SELFTEST
#include "selftest/selftest.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *rtc = DEVICE_DT_GET(DT_ALIAS(rtc0));

/* Hold left button for 3 seconds while USB is plugged in to enter DFU mode.
 * sys_reboot() hands control back to MCUboot, which opens a 5-second USB DFU
 * window. Works from battery: plug in USB first, then hold the button. */
static const struct gpio_dt_spec btn_dfu = GPIO_DT_SPEC_GET(DT_ALIAS(btn_left), gpios);

/*
 * Boot indicator — one blinking pixel, top-left.
 *
 * Something has to be on screen while the application starts, otherwise a slow
 * or stuck boot is indistinguishable from a dead watch. Drawn on LED_LAYER_BG
 * rather than the notification layer so it cannot collide with the low-battery
 * warning, which comes up during init.
 *
 * Note this only covers the application. MCUboot's USB DFU window runs for five
 * seconds before any of this, and the display is dark for all of it.
 */
static void boot_blink_fn(struct k_work *w)
{
	static bool lit;

	ARG_UNUSED(w);
	lit = !lit;

	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask[LED_LAYER_BG][0][0] = lit ? 1 : 0;
	led_layer_color[LED_LAYER_BG] = (struct led_rgb){0, 90, 160};
	k_mutex_unlock(&led_mask_mutex);

	led_commit();
}

K_WORK_DEFINE(boot_blink_work, boot_blink_fn);

static void boot_blink_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&boot_blink_work);
}

K_TIMER_DEFINE(boot_blink_timer, boot_blink_timer_cb, NULL);

static void boot_indicator_start(void)
{
	k_timer_start(&boot_blink_timer, K_NO_WAIT, K_MSEC(120));
}

static void boot_indicator_stop(void)
{
	k_timer_stop(&boot_blink_timer);

	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask[LED_LAYER_BG][0][0] = 0;
	led_layer_color[LED_LAYER_BG] = (struct led_rgb){0, 0, 0};
	k_mutex_unlock(&led_mask_mutex);
}

/*
 * Paint a "waiting for firmware" pattern, then let the reset happen.
 *
 * WS2812B latch the last frame they received and hold it for as long as they
 * have power — nothing refreshes them. So a frame committed here survives the
 * reset and stays lit through MCUboot's entire DFU window, even though the
 * bootloader never touches the LEDs. That would otherwise be impossible:
 * MCUboot's own CONFIG_MCUBOOT_INDICATION_LED drives a plain GPIO, and it has
 * no room for a WS2812B driver.
 *
 * It also outlives a failed update. If a DFU transfer dies partway the image
 * is invalid, MCUboot halts, and this pattern is still on screen — so a watch
 * in that state says "I am waiting for firmware" instead of looking dead.
 *
 * Blue along the top edge: distinct from the red low-battery row at the
 * bottom, and not something any normal mode draws.
 */
static void show_dfu_pattern(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	for (int col = 0; col < LED_COLS; col++) {
		led_mask[LED_LAYER_NOTIFICATION][0][col] = 1;
	}
	led_layer_color[LED_LAYER_NOTIFICATION] = (struct led_rgb){0, 80, 200};
	k_mutex_unlock(&led_mask_mutex);

	led_commit();

	/* led_commit() already waits for the transfer and the latch delay, but
	 * give the frame a little margin before the reset tears the PWM down. */
	k_msleep(30);
}

int main(void)
{
	if (!device_is_ready(rtc)) {
		LOG_ERR("RTC device not ready — continuing in degraded mode");
		/* Do NOT return: the DFU button loop below is the only recovery
		 * path. Returning here would prevent reflashing a device with a
		 * bad RTC. */
		rtc = NULL;
	}

	led_matrix_init();
	boot_indicator_start();

#ifdef CONFIG_EW_SELFTEST
	/* Runs before any application thread starts, so it has the LED matrix
	 * to itself. Note it sets the RTC to a fixed date as part of the test —
	 * re-sync over BLE afterwards. */
	{
		int fails = selftest_run();

		if (IS_ENABLED(CONFIG_EW_SELFTEST_HALT_ON_FAIL) && fails > 0) {
			LOG_ERR("Self-test failed (%d) — halting before app start", fails);
			while (true) {
				k_sleep(K_FOREVER);
			}
		}
	}
#endif

	/* Sand: warm amber (per-cell, no layer_color override) */
	led_color_fill(255, 160, 20);

	/* Digits: cool white — revealed beneath sand as particles clear */
	led_layer_color[LED_LAYER_DIGITS] = (struct led_rgb){220, 220, 255};

	if (rtc) {
		time_display_init(rtc);
	}

	/* No particles seeded here. The watch idles with the display off; sand
	 * appears when a mode asks for it. Seeding at boot left a pile of amber
	 * sitting in the bottom rows underneath the time, permanently. */
	sand_init();

	display_init();
	imu_init();
	identity_init();
	ble_init();
	battery_init();
	light_init();

	/* Boot finished: stop the indicator, blank everything and drop to idle.
	 * The watch shows nothing until a button asks it to — pressing left runs
	 * the reveal. */
	boot_indicator_stop();

	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	led_mask_clear_all();
	k_mutex_unlock(&led_mask_mutex);
	led_commit();

	display_off();

	bool dfu_ok = false;
	if (!gpio_is_ready_dt(&btn_dfu)) {
		LOG_ERR("DFU button GPIO not ready");
	} else {
		gpio_pin_configure_dt(&btn_dfu, GPIO_INPUT);
		dfu_ok = true;
	}

	LOG_INF("Every Watch starting");

	/* With CONFIG_SINGLE_APPLICATION_SLOT=y, MCUboot has no secondary slot
	 * and no revert mechanism. boot_write_img_confirmed() is not needed. */

	int32_t held_ms = 0;

	while (true) {
		k_sleep(K_MSEC(50));

		if (dfu_ok && gpio_pin_get_dt(&btn_dfu)) {
			held_ms = MIN(held_ms + 50, 5000);
			if (held_ms >= 3000) {
				LOG_INF("DFU: rebooting into bootloader");
				show_dfu_pattern();
				sys_reboot(SYS_REBOOT_COLD);
			}
		} else {
			held_ms = 0;
		}
	}
}
