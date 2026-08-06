#include "watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

/*
 * Liveness, not just "interrupts still work".
 *
 * The feed timer below runs from the system clock ISR, so if it fed
 * unconditionally the watchdog could only ever catch a fault that takes
 * interrupts down with it — a hard fault with IRQs locked, or a full CPU
 * lockup. Every failure this codebase is actually prone to (a deadlock on
 * led_commit_mutex/sand_mutex, a work item blocking the shared system
 * workqueue on a wedged I2C bus, a thread spinning) leaves the timer ISR
 * running happily, feeding a watchdog that would then never fire.
 *
 * So the ISR only feeds if something has called watchdog_alive() since the
 * last tick. main()'s idle loop calls it every 500 ms, which makes that loop
 * the liveness point — and it runs at the lowest priority in the system, so
 * anything starving it has starved everything above it too. See watchdog.h
 * for why the rate was relaxed from the 50 ms it used when that same loop
 * still polled the buttons.
 */
static atomic_t alive = ATOMIC_INIT(1);

/* Comfortable 3x margin under MCUboot's fixed 30s hardware window (see
 * watchdog.h). Feeding this often is not a meaningful power cost on its
 * own: BLE scanning/advertising already wakes the CPU far more often than
 * every 10s as part of normal operation (500ms scan interval, 1-1.28s slow
 * advertising interval), so this timer adds no new wake source in practice.
 *
 * Worst case from "main loop stops" to reset is therefore roughly
 * 2 x WDT_FEED_PERIOD_MS + the 30s hardware window: the tick right after
 * the hang can still see a stale alive flag and feed once more. ~50s to
 * recover an unresponsive watch is well inside "user reaches for it and
 * finds it working again" rather than "user assumes it is dead".
 */
#define WDT_FEED_PERIOD_MS (10 * 1000)

void watchdog_alive(void)
{
	atomic_set(&alive, 1);
}

static void feed_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);

	if (wdt_channel < 0) {
		return;
	}

	/* Clear-on-read: the next tick only feeds if the main loop has run
	 * again in the meantime. */
	if (atomic_cas(&alive, 1, 0)) {
		/* wdt_feed() is a synchronous register write, not a blocking
		 * call — safe from timer (ISR) context, unlike the I2C-backed
		 * peripherals elsewhere in this codebase that defer to a work
		 * item for exactly that reason. */
		wdt_feed(wdt, wdt_channel);
	}
}

static K_TIMER_DEFINE(feed_timer, feed_timer_cb, NULL);

void watchdog_init(void)
{
	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready — if MCUboot armed it, this "
			"board will reset every ~30s until this is fixed");
		return;
	}

	/*
	 * wdt_install_timeout() here is pure software bookkeeping local to
	 * this image (allocates a channel index in this driver instance's own
	 * RAM state) — it does not touch the hardware reload register, which
	 * is already locked in from MCUboot having started the peripheral.
	 * The window value below is otherwise inert; it exists only because
	 * wdt_feed() needs a valid channel handle, and this is how one gets
	 * allocated. Both images start from a freshly-zeroed allocator, so
	 * this deterministically returns the same channel 0 MCUboot enabled
	 * in RREN, which is what makes the feed reach the real hardware.
	 *
	 * NOTE: nrfx_wdt_channel_feed() asserts the driver state is
	 * POWERED_ON, which is only true in the image that called
	 * wdt_setup() — i.e. MCUboot, not here. That assert compiles to
	 * nothing while CONFIG_ASSERT is off (it is, in every variant of this
	 * project). Turning CONFIG_ASSERT on for debugging will trip it on
	 * the first feed; that is a debug-build artifact of MCUboot and the
	 * app being separate binaries sharing one peripheral, not a real
	 * fault.
	 */
	struct wdt_timeout_cfg cfg = {
		.flags      = WDT_FLAG_RESET_SOC,
		.window.min = 0,
		.window.max = 30000,
	};

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("wdt_install_timeout failed: %d — watchdog will not be "
			"fed; if MCUboot armed it the board resets in ~30s",
			wdt_channel);
		return;
	}

	watchdog_alive();
	wdt_feed(wdt, wdt_channel);
	k_timer_start(&feed_timer, K_MSEC(WDT_FEED_PERIOD_MS), K_MSEC(WDT_FEED_PERIOD_MS));

	LOG_INF("Watchdog feed started (channel %d, every %d ms)",
		wdt_channel, WDT_FEED_PERIOD_MS);
}
