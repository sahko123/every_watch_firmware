#pragma once

/*
 * Hardware self-test for first board bring-up and end-of-line manufacturing
 * test. Enabled with CONFIG_EW_SELFTEST=y (see bringup.conf).
 *
 * Probes every peripheral on the board and prints a PASS/FAIL report to the
 * console, then runs the interactive checks (LED pattern walk, button press)
 * that need a human watching.
 *
 * Call after led_matrix_init() and before the application threads start, so
 * it has exclusive use of the LED matrix.
 *
 * Returns the number of failed checks (0 = all good).
 */
int selftest_run(void);
