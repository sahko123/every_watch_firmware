#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "time_display/time_display.h"
#include "display/display.h"
#include "imu/imu.h"
#include "identity/identity.h"
#include "ble/ble.h"
#include "battery/battery.h"
#include "light/light.h"
#include "watchdog/watchdog.h"
#include "buttons/buttons.h"
#include "ui/ui.h"

#ifdef CONFIG_EW_SELFTEST
#include "selftest/selftest.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *rtc = DEVICE_DT_GET(DT_ALIAS(rtc0));

/*
 * usb_enable() has to run before the shell backend's own init, or the
 * shell never comes up at all: shell_uart.c registers enable_shell_uart()
 * as SYS_INIT(..., POST_KERNEL, CONFIG_SHELL_BACKEND_SERIAL_INIT_PRIORITY)
 * (that priority defaults to APPLICATION_INIT_PRIORITY), and it calls
 * device_is_ready() on the CDC-ACM UART once, at that point, with no retry
 * — if the USB stack hasn't been enabled yet the device isn't ready, the
 * hook returns -ENODEV, and shell_init() is simply never called for the
 * rest of the boot. main() runs after every POST_KERNEL and APPLICATION
 * SYS_INIT has already completed, so calling usb_enable() from there (as
 * this used to) is always too late — confirmed on hardware: USB enumerated
 * fine but the shell never echoed or responded to anything.
 *
 * A SYS_INIT hook of its own fixes it, but the priority within POST_KERNEL
 * still matters: usb_dc_nrfx.c's own driver init
 * (SYS_INIT(usb_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE),
 * default 50) has to run first or usb_enable() operates on an
 * uninitialised driver. 90 is after that but still POST_KERNEL, which as
 * a whole always finishes before APPLICATION starts — so this beats the
 * shell backend's init regardless of where
 * CONFIG_SHELL_BACKEND_SERIAL_INIT_PRIORITY is set within that later stage.
 */
static int usb_enable_early(void)
{
	if (!IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
		return 0;
	}

	int rc = usb_enable(NULL);

	if (rc && rc != -EALREADY) {
		LOG_ERR("USB enable failed: %d — settime over USB will"
			" not be reachable", rc);
	} else {
		LOG_INF("USB enabled (settime available once a host"
			" enumerates the CDC port)");
	}

	return 0;
}
SYS_INIT(usb_enable_early, POST_KERNEL, 90);

/*
 * Button gestures all go to the UI, which owns the display and decides what
 * each one means. DFU is bound there too (both buttons held) rather than
 * being special-cased here — it is just another page, and one that happens
 * to reboot on entry.
 *
 * Ambient light is NOT sampled here. It used to be, and it could never have
 * worked: this runs on the system workqueue (buttons.c dispatches from a work
 * item), and light_sample_now() submitted to that same queue — so the read
 * could not begin until this function had returned and ui_handle_button()
 * below had already lit the display. The sampler's own "skip if the display is
 * on" guard then threw the reading away, every time, leaving led_brightness at
 * its power-on default permanently.
 *
 * Sampling now happens on the way down instead, in display_off(), where the
 * LEDs are definitively dark. See light.c.
 */
static void on_button(enum btn_event ev)
{
	ui_handle_button(ev);
}

/*
 * Boot indicator — one blinking pixel, top-left.
 *
 * Something has to be on screen while the application starts, otherwise a slow
 * or stuck boot is indistinguishable from a dead watch. Drawn on LED_LAYER_BG
 * rather than the notification layer so it cannot collide with the low-battery
 * warning, which comes up during init.
 *
 * Note this only covers the application. When MCUboot enters USB DFU (button
 * held at boot) it runs before any of this, and the display is dark for all
 * of it — however long the wait, since that mode has no timeout either.
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


int main(void)
{
	/* First thing, before anything that could plausibly block for close
	 * to MCUboot's fixed 30s hardware timeout — the interactive self-test
	 * below in particular (button waits alone can run up to 30s) would
	 * blow through that window entirely if the feed timer weren't already
	 * running by the time it starts. */
	watchdog_init();

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
	/* Stop the indicator first: it was still ticking on a 120ms k_timer
	 * during selftest_run(), racing selftest.c's own LED tests (both write
	 * LED_LAYER_BG and call led_commit()) despite selftest.h's doc comment
	 * claiming exclusive use of the matrix. Restarted below so there's
	 * still something on screen for the rest of boot. */
	boot_indicator_stop();

	{
		int fails = selftest_run();

		if (IS_ENABLED(CONFIG_EW_SELFTEST_HALT_ON_FAIL) && fails > 0) {
			LOG_ERR("Self-test failed (%d) — halting before app start", fails);
			/* Keep signalling liveness: this halt is deliberate (an
			 * end-of-line manufacturing test stopping so an operator
			 * can read the report), not a hang. Without this the
			 * watchdog would starve and reset-loop the board, wiping
			 * the very report this state exists to show. */
			while (true) {
				watchdog_alive();
				k_sleep(K_SECONDS(1));
			}
		}
	}

	boot_indicator_start();
#endif

	/* Resting per-cell colour for any layer without a layer_color override. */
	led_color_reset();

	/*
	 * The digit layer deliberately has NO layer_color, so it reads
	 * led_color[] per cell — that is what lets the clock come up in the
	 * curtain's rainbow and keep drifting with the hue wave.
	 *
	 * A {220, 220, 255} cool white used to be set here. It never reached the
	 * display: ui_init() below calls blank_all_layers(), which zeroes every
	 * layer colour, and so does every ui_goto() after it. It read as the
	 * digits' colour while being nothing of the sort.
	 */

	if (rtc) {
		time_display_init(rtc);
	}

	/* No particles seeded here. The watch idles with the display off; sand
	 * appears when a mode asks for it. Seeding at boot left a pile of amber
	 * sitting in the bottom rows underneath the time, permanently. */
	sand_init();

	/* imu_init() before display_init(): display_init()'s GPIO-failure
	 * paths call imu_suspend() to make good on their "display permanently
	 * off" log line, but imu_suspend() early-returns as a no-op until
	 * imu_ready is set — which only happens once imu_init() has actually
	 * run. In the old order that made those failure paths silently leave
	 * the IMU polling at full rate forever despite the log claiming
	 * otherwise. */
	imu_init();
	display_init();
	identity_init();
	ble_init();
	battery_init();
	light_init();

	/* Boot finished. ui_init() parks the sand thread, blanks every layer
	 * and drops the display — the watch shows nothing until a button asks
	 * it to. Nothing above this point should have drawn: that was the
	 * cause of the boot flash, where time_display_init() published the
	 * clock into the digit layer while the sand thread was already
	 * committing at 30 Hz, so it was visibly pushed to the LEDs for the
	 * rest of init before finally being blanked. */
	boot_indicator_stop();
	ui_init();

	buttons_init(on_button);

	/* Bench/demo only, default off — see CONFIG_EW_SAND_DEMO_AT_BOOT.
	 * Goes through the UI rather than seeding directly, so the sand page
	 * owns the display like any other. */
	if (CONFIG_EW_SAND_DEMO_AT_BOOT > 0) {
		ui_goto(UI_PAGE_SAND);
		LOG_INF("Sand demo page opened at boot");
	}

	LOG_INF("Every Watch starting");

	/* With CONFIG_SINGLE_APPLICATION_SLOT=y, MCUboot has no secondary slot
	 * and no revert mechanism. boot_write_img_confirmed() is not needed. */

	/*
	 * Idle. Input is interrupt-driven in buttons.c and pages are driven by
	 * ui.c, so there is nothing to poll — this loop exists only to feed
	 * the watchdog.
	 *
	 * It remains the right liveness point: it runs on the main thread at
	 * the lowest priority in the system, so if it stops being scheduled
	 * something has gone badly wrong regardless of which subsystem caused
	 * it. Deliberately slower than the old 50 ms poll, since it no longer
	 * has any input latency to service — see watchdog.h for the margin.
	 */
	while (true) {
		watchdog_alive();
		k_sleep(K_MSEC(500));
	}
}
