#include "display.h"
#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "imu/imu.h"
#include "time_display/time_display.h"
#include "battery/battery.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/*
 * Panel power only.
 *
 * This module used to be the whole UI: it owned a left-button interrupt, its
 * own idea of what a press meant, and a 10 s auto-off timer. All three have
 * moved — input to buttons.c, page lifetime and arbitration to ui.c — leaving
 * this with one job: turn the display on and off, and park or start the
 * threads that drive it.
 *
 * Callers are expected to be on a thread or the system workqueue. Nothing
 * here is ISR-safe; it suspends threads and blocks on DMA.
 */

static bool is_on;

void display_on(void)
{
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

	/* sand_suspend() takes led_commit_mutex and sand_mutex itself, so it
	 * cannot freeze the sand thread mid-DMA or mid-tick — see the comment
	 * on it. This used to be done here, which meant every other caller had
	 * to know about the handshake and one of them didn't. */
	sand_suspend();
	imu_suspend();

	/* Sand is suspended; push one blank frame to clear the LEDs. ui.c
	 * blanks before every transition too, but display_off() is also
	 * reachable directly and must not leave pixels lit. */
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	memset(led_mask, 0, sizeof(led_mask));
	k_mutex_unlock(&led_mask_mutex);
	led_commit();

	/* End the time-viewing session. Waking the display again (a battery
	 * check, a low-battery blip) must not bring time back on its own — only
	 * a fresh reveal should. */
	time_display_deactivate();

	/* The low-battery stripe on the notification layer was just wiped by
	 * the memset above along with everything else; let battery.c know so
	 * it can re-assert the warning on its next poll if still low, rather
	 * than the edge-triggered flag leaving it shown at most once per
	 * episode. */
	battery_notify_display_off();

	LOG_INF("Display off");
}

bool display_is_on(void)
{
	return is_on;
}

void display_init(void)
{
	/*
	 * is_on starts true because sand_init()/imu_init() have already started
	 * their threads by the time this runs. ui_init() then blanks and calls
	 * display_off(), which is what actually parks them — starting false
	 * would make that call a no-op and leave both running forever.
	 */
	is_on = true;

	LOG_INF("Display ready");
}
