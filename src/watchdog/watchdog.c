#include "watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

/* Comfortable 3x margin under MCUboot's fixed 30s hardware window (see
 * watchdog.h). Feeding this often is not a meaningful power cost on its
 * own: BLE scanning/advertising already wakes the CPU far more often than
 * every 10s as part of normal operation (500ms scan interval, 1-1.28s slow
 * advertising interval), so this timer adds no new wake source in practice. */
#define WDT_FEED_PERIOD_MS (10 * 1000)

static void feed_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);

	/* wdt_feed() is a synchronous register write, not a blocking call —
	 * safe to call directly from timer (ISR) context, unlike the I2C-
	 * backed peripherals elsewhere in this codebase that defer to a
	 * work item for exactly that reason. */
	if (wdt_channel >= 0) {
		wdt_feed(wdt, wdt_channel);
	}
}

static K_TIMER_DEFINE(feed_timer, feed_timer_cb, NULL);

void watchdog_init(void)
{
	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready — MCUboot's timeout will "
			"fire and reset the board every ~30s until this is fixed");
		return;
	}

	/*
	 * wdt_install_timeout() here is pure software bookkeeping local to
	 * this image (allocates a channel index in this driver instance's own
	 * RAM state) — it does not touch the hardware reload register, which
	 * is already locked from MCUboot having started it. The window value
	 * below is otherwise inert; it exists only because wdt_feed() needs a
	 * valid channel handle to feed, and this is how one gets allocated.
	 */
	struct wdt_timeout_cfg cfg = {
		.flags      = WDT_FLAG_RESET_SOC,
		.window.min = 0,
		.window.max = 30000,
	};

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("wdt_install_timeout failed: %d — watchdog will not "
			"be fed, board will reset every ~30s", wdt_channel);
		return;
	}

	wdt_feed(wdt, wdt_channel);
	k_timer_start(&feed_timer, K_MSEC(WDT_FEED_PERIOD_MS), K_MSEC(WDT_FEED_PERIOD_MS));

	LOG_INF("Watchdog feed started (channel %d, every %d ms)",
		wdt_channel, WDT_FEED_PERIOD_MS);
}
