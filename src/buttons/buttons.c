#include "buttons.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

static const struct gpio_dt_spec btn_l = GPIO_DT_SPEC_GET(DT_ALIAS(btn_left),  gpios);
static const struct gpio_dt_spec btn_r = GPIO_DT_SPEC_GET(DT_ALIAS(btn_right), gpios);

static struct gpio_callback btn_l_cb;
static struct gpio_callback btn_r_cb;

static btn_handler_t user_handler;

/*
 * Timing.
 *
 * SAMPLE_MS doubles as the debounce: a bounce shorter than one sample is
 * simply never observed, which is cheaper and more robust than a separate
 * debounce timer. The buttons are mechanical with ~1 ms of bounce, so 15 ms
 * is comfortable.
 *
 * SIMUL_MS is how far apart the two buttons may be pressed and still count as
 * "both". Humans are not simultaneous; 120 ms is a deliberate two-thumb press
 * without being so wide that a fast left-then-right sequence gets swallowed.
 *
 * DOUBLE_MS is measured from release of the first press to press of the
 * second. It is also how long a single press is held back before being
 * emitted, since a single is only knowable once the double window has closed.
 */
#define SAMPLE_MS  15
#define SIMUL_MS  120
#define HOLD_MS  2000
#define DOUBLE_MS 300

enum gesture_state {
	G_IDLE,          /* nothing down, timer stopped */
	G_PRESSED,       /* at least one button down, deciding what this is */
	G_WAIT_DOUBLE,   /* released once, waiting to see if a second comes */
};

static enum gesture_state state;

static bool l_down;
static bool r_down;

/* Which buttons were involved in the current gesture. Latched on press and
 * kept until the gesture completes, so releasing one of a two-button press
 * slightly early still reads as "both". */
static bool used_l;
static bool used_r;

static int64_t press_start;    /* when the first button of this gesture went down */
static int64_t release_time;   /* when the last button came up */
static bool    hold_fired;     /* hold already emitted; suppress the release event */
static bool    pending_l;      /* a single-press candidate waiting out DOUBLE_MS */
static bool    pending_r;

/*
 * Own work queue, at a priority above both the system workqueue and the USB
 * device stack's workqueue (both -1, see prj.conf/USB Kconfig defaults).
 * CONFIG_TIMESLICE_SIZE is 0 (off), so among threads at equal priority
 * nothing forces a switch back to the system workqueue if the USB workqueue
 * thread stays busy — a burst of CDC-ACM TX activity can occupy it for a
 * real, visible stretch. sample_work_fn runs on a 15 ms periodic timer while
 * a gesture is live (SAMPLE_MS); if its queue is starved for longer than one
 * press-and-release, the two samples that would have straddled it never run,
 * and gpio_pin_get_dt() reads "not down" on both sides — the whole press is
 * gone with no event ever firing. A dedicated, higher-priority queue means
 * button sampling always preempts USB activity instead of queuing behind it.
 */
#define BUTTON_WORKQ_PRIORITY -2
/* Matches CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE: this call chain (emit() ->
 * ui_handle_button() -> ui_goto() -> display/sand/time_display/battery code)
 * is identical to what used to run on the system workqueue, just moved to
 * its own thread — no reason for it to need less stack than that queue was
 * already proven to need for the same call depth. */
#define BUTTON_WORKQ_STACK_SIZE 2048

static struct k_work_q button_work_q;
static K_THREAD_STACK_DEFINE(button_work_q_stack, BUTTON_WORKQ_STACK_SIZE);

static void emit(enum btn_event ev)
{
	LOG_DBG("gesture: %s", btn_event_name(ev));

	if (user_handler) {
		user_handler(ev);
	}
}

const char *btn_event_name(enum btn_event ev)
{
	switch (ev) {
	case BTN_EV_L_SINGLE:   return "L single";
	case BTN_EV_L_DOUBLE:   return "L double";
	case BTN_EV_L_HOLD:     return "L hold";
	case BTN_EV_R_SINGLE:   return "R single";
	case BTN_EV_R_DOUBLE:   return "R double";
	case BTN_EV_R_HOLD:     return "R hold";
	case BTN_EV_BOTH_PRESS: return "both press";
	case BTN_EV_BOTH_HOLD:  return "both hold";
	default:                return "?";
	}
}

/* -------------------------------------------------------------------------
 * Sampling
 *
 * Runs only while something is happening. Started by the GPIO ISR on the
 * first press, stopped once the gesture has fully resolved — including the
 * double-press window, so a second press restarts it rather than being
 * missed.
 * ------------------------------------------------------------------------- */

static void sample_work_fn(struct k_work *w);
static K_WORK_DEFINE(sample_work, sample_work_fn);

static void sample_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit_to_queue(&button_work_q, &sample_work);
}

static K_TIMER_DEFINE(sample_timer, sample_timer_cb, NULL);

static void timer_start(void)
{
	k_timer_start(&sample_timer, K_MSEC(SAMPLE_MS), K_MSEC(SAMPLE_MS));
}

static void timer_stop(void)
{
	k_timer_stop(&sample_timer);
}

/* Fire the deferred single-press, if the double window closed without a
 * second press arriving. */
static void resolve_pending(void)
{
	if (pending_l) {
		pending_l = false;
		emit(BTN_EV_L_SINGLE);
	}
	if (pending_r) {
		pending_r = false;
		emit(BTN_EV_R_SINGLE);
	}
}

static void gesture_reset(void)
{
	state      = G_IDLE;
	used_l     = false;
	used_r     = false;
	hold_fired = false;
	timer_stop();
}

static void sample_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	l_down = gpio_pin_get_dt(&btn_l) == 1;
	r_down = gpio_pin_get_dt(&btn_r) == 1;

	bool any_down = l_down || r_down;
	int64_t now   = k_uptime_get();

	switch (state) {
	case G_IDLE:
		if (any_down) {
			/* A press arrived between the ISR and this sample, or
			 * the ISR started us. Begin a new gesture. */
			state       = G_PRESSED;
			press_start = now;
			used_l      = l_down;
			used_r      = r_down;
			hold_fired  = false;

			/* A new press cancels any single still waiting on its
			 * double window — that is what makes it a double. */
			if (pending_l && l_down) {
				pending_l = false;
				emit(BTN_EV_L_DOUBLE);
				/* Consumed: this press is the second of a pair,
				 * so do not also report it on release. */
				used_l = false;
			}
			if (pending_r && r_down) {
				pending_r = false;
				emit(BTN_EV_R_DOUBLE);
				used_r = false;
			}
		} else {
			timer_stop();
		}
		break;

	case G_PRESSED:
		/* Latch anything that joins within the simultaneity window, so
		 * a two-thumb press registers as "both" even though the two
		 * contacts are milliseconds apart. */
		if (now - press_start <= SIMUL_MS) {
			used_l |= l_down;
			used_r |= r_down;
		}

		if (any_down) {
			if (!hold_fired && now - press_start >= HOLD_MS) {
				hold_fired = true;

				if (used_l && used_r) {
					emit(BTN_EV_BOTH_HOLD);
				} else if (used_l) {
					emit(BTN_EV_L_HOLD);
				} else if (used_r) {
					emit(BTN_EV_R_HOLD);
				}
			}
			break;
		}

		/* Everything released. */
		if (hold_fired) {
			/* Hold already reported; do not also emit a press. */
			gesture_reset();
			break;
		}

		if (used_l && used_r) {
			/* Two-button presses have no double variant — nothing
			 * is waiting on, so emit immediately. */
			emit(BTN_EV_BOTH_PRESS);
			gesture_reset();
			break;
		}

		/* Single-button short press: hold it back until the double
		 * window closes, since a second press would make it a double. */
		if (used_l) {
			pending_l = true;
		}
		if (used_r) {
			pending_r = true;
		}

		release_time = now;
		state        = G_WAIT_DOUBLE;
		used_l       = false;
		used_r       = false;
		break;

	case G_WAIT_DOUBLE:
		if (any_down) {
			/* Second press inside the window — go round again;
			 * G_IDLE's entry logic turns the pending single into a
			 * double. */
			state = G_IDLE;
			k_work_submit_to_queue(&button_work_q, &sample_work);
			break;
		}

		if (now - release_time >= DOUBLE_MS) {
			resolve_pending();
			gesture_reset();
		}
		break;
	}
}

/* -------------------------------------------------------------------------
 * ISR — wakes the sampler, does nothing else
 * ------------------------------------------------------------------------- */

static void btn_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);

	/* Idle cost lives here: without a press the timer never runs. */
	timer_start();
	k_work_submit_to_queue(&button_work_q, &sample_work);
}

bool buttons_left_down(void)  { return gpio_pin_get_dt(&btn_l) == 1; }
bool buttons_right_down(void) { return gpio_pin_get_dt(&btn_r) == 1; }

void buttons_init(btn_handler_t handler)
{
	user_handler = handler;

	k_work_queue_start(&button_work_q, button_work_q_stack,
			    K_THREAD_STACK_SIZEOF(button_work_q_stack),
			    BUTTON_WORKQ_PRIORITY, NULL);
	k_thread_name_set(&button_work_q.thread, "button_workq");

	if (!gpio_is_ready_dt(&btn_l) || !gpio_is_ready_dt(&btn_r)) {
		LOG_ERR("button GPIO not ready — no input available");
		return;
	}

	int rc = 0;

	rc |= gpio_pin_configure_dt(&btn_l, GPIO_INPUT);
	rc |= gpio_pin_configure_dt(&btn_r, GPIO_INPUT);

	/* Both edges: the sampler needs to see releases too, and a release is
	 * what ends a gesture. */
	rc |= gpio_pin_interrupt_configure_dt(&btn_l, GPIO_INT_EDGE_BOTH);
	rc |= gpio_pin_interrupt_configure_dt(&btn_r, GPIO_INT_EDGE_BOTH);

	gpio_init_callback(&btn_l_cb, btn_isr, BIT(btn_l.pin));
	gpio_init_callback(&btn_r_cb, btn_isr, BIT(btn_r.pin));
	rc |= gpio_add_callback(btn_l.port, &btn_l_cb);
	rc |= gpio_add_callback(btn_r.port, &btn_r_cb);

	if (rc) {
		LOG_ERR("button GPIO setup failed (%d) — no input available", rc);
		return;
	}

	state = G_IDLE;
	LOG_INF("Buttons ready (hold %d ms, double %d ms)", HOLD_MS, DOUBLE_MS);
}
