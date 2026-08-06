#pragma once

#include <stdbool.h>

/*
 * Button gesture recogniser.
 *
 * Replaces the two ad-hoc paths this used to have: a GPIO interrupt in
 * display.c that only knew about "left pressed", and a 20 Hz poll loop in
 * main() that only knew about "held 3 s". Neither could see the other, so a
 * gesture spanning both buttons was not expressible at all.
 *
 * Events are delivered from a work queue, never from ISR context, so handlers
 * may take mutexes, touch I2C and drive the display directly.
 *
 * That queue is this module's own, at a priority above the system and USB
 * queues rather than sharing the system one — see buttons.c. Handlers
 * therefore preempt USB and workqueue activity, and must not assume they are
 * serialised against anything else running on the system queue.
 */
enum btn_event {
	BTN_EV_L_SINGLE,
	BTN_EV_L_DOUBLE,
	BTN_EV_L_HOLD,

	BTN_EV_R_SINGLE,
	BTN_EV_R_DOUBLE,
	BTN_EV_R_HOLD,

	/* Both buttons down together. BOTH_PRESS fires on release, BOTH_HOLD
	 * fires as soon as the hold time is reached and suppresses the
	 * release event — so a long press does not also emit a short one. */
	BTN_EV_BOTH_PRESS,
	BTN_EV_BOTH_HOLD,
};

const char *btn_event_name(enum btn_event ev);

typedef void (*btn_handler_t)(enum btn_event ev);

/*
 * Start watching the buttons. The handler is called once per recognised
 * gesture.
 *
 * Idle cost is zero: both pins sit on GPIO interrupts and the sampling timer
 * only runs while a button is actually down (plus the double-press window
 * after release). Nothing polls while the watch is sitting still.
 */
void buttons_init(btn_handler_t handler);

/* True while the given button is physically down. For callers that need
 * level rather than edges — the DFU pattern wants to know the user is still
 * holding through a reboot. */
bool buttons_left_down(void);
bool buttons_right_down(void);
