#pragma once

#include <stdint.h>

/*
 * Setting the RTC from a Unix epoch, with the conversion in one place.
 *
 * Two front doors reach this: the `settime` shell command, for a bench script
 * on a wired console, and the BLE time characteristic, for a phone. Both take
 * the same integer and both need the same validation, so neither gets its own
 * copy of the calendar arithmetic.
 */

/* Latest epoch this firmware will accept — 2100-01-01T00:00:00Z. Anything
 * beyond it is a malformed number rather than a real date: the RTC and the
 * clock face have no business representing a time this watch will not see. */
#define TIME_SYNC_EPOCH_MAX 4102444800LL

/*
 * Set the RTC from Unix epoch seconds (UTC).
 *
 * Returns 0 on success, -EINVAL if the value is negative or beyond
 * TIME_SYNC_EPOCH_MAX, -ENODEV if the RTC is not ready, or the driver's own
 * error. Thread-safe to the extent the RTC driver is — this only writes.
 */
int time_sync_set_epoch(int64_t epoch);

/*
 * Current RTC time as Unix epoch seconds (UTC), or a negative errno.
 *
 * Exists so a client that just set the time can read it back and see what
 * actually landed, rather than trusting a write it cannot verify.
 */
int64_t time_sync_get_epoch(void);
