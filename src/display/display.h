#pragma once

#include <stdbool.h>

/*
 * Panel power. Turning the display on or off also starts or parks the threads
 * that drive it (sand simulation, IMU polling), so calling display_off() is
 * what actually stops those burning cycles.
 *
 * This is deliberately not the UI: which page is showing, how long it stays,
 * and what a button does all live in ui.c. Page code should generally not
 * call these directly — ui_goto() handles it as part of a transition.
 *
 * Not ISR-safe: suspends threads and blocks on DMA. Call from a thread or the
 * system workqueue.
 */
void display_init(void);
void display_on(void);
void display_off(void);
bool display_is_on(void);
