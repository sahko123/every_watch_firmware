#include "display.h"
#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "imu/imu.h"
#include "time_display/time_display.h"
#include "light/light.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

static bool is_on;

/* -------------------------------------------------------------------------
 * Work items — button ISR and timer callback run in ISR context, so they
 * can't call k_thread_suspend/resume directly. Defer to the system work queue.
 * ------------------------------------------------------------------------- */

static void on_work_fn(struct k_work *w);
static void off_work_fn(struct k_work *w);

K_WORK_DEFINE(on_work,  on_work_fn);
K_WORK_DEFINE(off_work, off_work_fn);

/* -------------------------------------------------------------------------
 * Auto-off timer
 * ------------------------------------------------------------------------- */

static void timeout_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&off_work);
}

K_TIMER_DEFINE(display_timer, timeout_cb, NULL);

/* -------------------------------------------------------------------------
 * Button interrupts
 * ------------------------------------------------------------------------- */

static const struct gpio_dt_spec btn_l = GPIO_DT_SPEC_GET(DT_ALIAS(btn_left),  gpios);

static struct gpio_callback btn_l_cb;

/* Left button wakes the display and starts the time reveal. Deferred to the
 * work queue like everything else here: the reveal reads the RTC over I2C,
 * which must not happen in ISR context.
 *
 * The right button deliberately has no press handler here. It used to also
 * wake the display, but time_display.c republishes the current time into the
 * digit layer every second regardless of display state — so waking on a bare
 * right press showed the time instantly with no reveal, which read as a
 * separate, unintended "instant time" mode. The right button's own behaviour
 * (the 3 s hold for the battery readout, polled in main.c) already wakes the
 * display itself once it fires. */
static void reveal_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	time_display_reveal();
}

K_WORK_DEFINE(reveal_work, reveal_work_fn);

static void btn_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);

	/* Sample ambient light before anything below turns the LEDs on — the
	 * system workqueue runs these in submission order, so this reading is
	 * still taken with the display off. */
	light_sample_now();

	k_work_submit(&on_work);
	k_work_submit(&reveal_work);
}

/* -------------------------------------------------------------------------
 * State transitions
 * ------------------------------------------------------------------------- */

void display_on(void)
{
	/* Always reset the timeout, even if already on */
	k_timer_start(&display_timer, K_SECONDS(DISPLAY_TIMEOUT_S), K_NO_WAIT);

	if (is_on) {
		return;
	}

	is_on = true;
	imu_resume();
	sand_resume();
	LOG_INF("Display on");
}

void display_off(void)
{
	if (!is_on) {
		return;
	}

	is_on = false;
	k_timer_stop(&display_timer);

	/* Acquire the commit mutex before suspending the sand thread. Zephyr's
	 * k_mutex has priority inheritance: if the sand thread holds the mutex
	 * mid-DMA, it gets elevated to our priority and completes before we
	 * proceed. Once we hold it, sand cannot start a new commit. */
	k_mutex_lock(&led_commit_mutex, K_FOREVER);
	sand_suspend();
	imu_suspend();
	k_mutex_unlock(&led_commit_mutex);

	/* Sand is suspended; push one blank frame to clear the LEDs */
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	memset(led_mask, 0, sizeof(led_mask));
	k_mutex_unlock(&led_mask_mutex);
	led_commit();

	/* End the time-viewing session. Waking the display again (a battery
	 * check, a low-battery blip) must not bring time back on its own — only
	 * a fresh reveal should. */
	time_display_deactivate();

	LOG_INF("Display off");
}

bool display_is_on(void)
{
	return is_on;
}

void display_reset_timeout(void)
{
	if (is_on) {
		k_timer_start(&display_timer, K_SECONDS(DISPLAY_TIMEOUT_S), K_NO_WAIT);
	}
}

static void on_work_fn(struct k_work *w)  { ARG_UNUSED(w); display_on(); }
static void off_work_fn(struct k_work *w) { ARG_UNUSED(w); display_off(); }

/* -------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

void display_init(void)
{
	if (!gpio_is_ready_dt(&btn_l)) {
		LOG_ERR("display button GPIO not ready — display permanently off");
		return;
	}

	int rc;

	rc  = gpio_pin_configure_dt(&btn_l, GPIO_INPUT);
	rc |= gpio_pin_interrupt_configure_dt(&btn_l, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_init_callback(&btn_l_cb, btn_isr, BIT(btn_l.pin));
	rc |= gpio_add_callback(btn_l.port, &btn_l_cb);

	if (rc) {
		LOG_ERR("display button GPIO setup failed — display permanently off");
		return;
	}

	/* Set is_on directly: sand and IMU threads already start running from
	 * sand_init()/imu_init() in main(), so imu_resume()/sand_resume() are
	 * not needed here. */
	is_on = true;
	k_timer_start(&display_timer, K_SECONDS(DISPLAY_TIMEOUT_S), K_NO_WAIT);

	LOG_INF("Display state machine ready (%d s timeout)", DISPLAY_TIMEOUT_S);
}
