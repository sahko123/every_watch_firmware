#pragma once

void light_init(void);

/*
 * Queue an ambient-light sample. ISR-safe — it only submits work.
 *
 * Must be called with the LEDs already dark. The sensor is under the same
 * glass as the display, so a reading taken with anything lit measures the
 * display rather than the room; the sampler double-checks and discards the
 * read if the display came on meanwhile.
 *
 * In practice display_off() calls this, after its blanking commit, and light.c
 * re-polls on its own timer while the display stays off. Calling it from a
 * path that is about to turn the display ON does not work — see the comment on
 * the definition for why that failed silently for so long.
 */
void light_sample_now(void);

/*
 * Tell the sampler the watch is being used, so it polls quickly for a while.
 *
 * Called from display_on(). Someone who has just looked at the watch is likely
 * to look again shortly, and may be walking between rooms while they do, so
 * the reading behind the next wake needs to be current rather than up to ten
 * seconds old. The rate decays back on its own after a minute of nothing —
 * callers do not need to signal that the user has stopped.
 */
void light_notify_activity(void);
