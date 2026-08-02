#pragma once

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
