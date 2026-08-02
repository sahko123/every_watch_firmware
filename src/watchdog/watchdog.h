#pragma once

#include <zephyr/kernel.h>

#ifndef CONFIG_WATCHDOG
/* watchdog.c is only compiled when the driver is configured (see
 * CMakeLists.txt), but main.c calls into it unconditionally — stub out
 * rather than sprinkling #ifdefs through the boot sequence. */
static inline void watchdog_init(void)  { }
static inline void watchdog_alive(void) { }
#else

/*
 * MCUboot starts the hardware watchdog (see child_image/mcuboot.conf and its
 * local patch to bootloader/mcuboot's mcuboot_config.h) with a fixed 30 s
 * timeout — the nRF52's reload value can only be configured before the
 * peripheral starts, so this app cannot pick a different one, only feed
 * what's already running. The watchdog cannot be stopped once started, only
 * a reset clears it — call this early in main() (before anything that could
 * legitimately block for more than ~30 s, e.g. the interactive self-test) or
 * the board resets before it ever gets fed once.
 */
void watchdog_init(void);

/*
 * Liveness signal. The internal feed timer only feeds the watchdog if this
 * has been called since its last tick, so that a deadlock or a blocked
 * workqueue — which leaves the timer ISR itself running perfectly happily —
 * still starves the watchdog and forces a recovery reset.
 *
 * Call from a loop that is genuinely representative of the system doing its
 * job. Currently main()'s button-poll loop (every 50 ms), chosen because it
 * is also what services the 3 s left-button hold into DFU: if it stops
 * running, the watch has lost its only on-device route back to a reflash.
 *
 * Safe to call from any context (a single atomic store).
 */
void watchdog_alive(void);

#endif /* CONFIG_WATCHDOG */
