#include "display.h"
#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "imu/imu.h"

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
static const struct gpio_dt_spec btn_r = GPIO_DT_SPEC_GET(DT_ALIAS(btn_right), gpios);

static struct gpio_callback btn_l_cb;
static struct gpio_callback btn_r_cb;

static void btn_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
	k_work_submit(&on_work);
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
	if (!gpio_is_ready_dt(&btn_l) || !gpio_is_ready_dt(&btn_r)) {
		LOG_ERR("Button GPIO not ready");
		return;
	}

	gpio_pin_configure_dt(&btn_l, GPIO_INPUT);
	gpio_pin_configure_dt(&btn_r, GPIO_INPUT);

	/* GPIO_INT_EDGE_TO_ACTIVE = falling edge for active-low buttons */
	gpio_pin_interrupt_configure_dt(&btn_l, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_pin_interrupt_configure_dt(&btn_r, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_init_callback(&btn_l_cb, btn_isr, BIT(btn_l.pin));
	gpio_init_callback(&btn_r_cb, btn_isr, BIT(btn_r.pin));
	gpio_add_callback(btn_l.port, &btn_l_cb);
	gpio_add_callback(btn_r.port, &btn_r_cb);

	/* Set is_on directly: sand and IMU threads already start running from
	 * sand_init()/imu_init() in main(), so imu_resume()/sand_resume() are
	 * not needed here. */
	is_on = true;
	k_timer_start(&display_timer, K_SECONDS(DISPLAY_TIMEOUT_S), K_NO_WAIT);

	LOG_INF("Display state machine ready (%d s timeout)", DISPLAY_TIMEOUT_S);
}
