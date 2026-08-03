/*
 * Setting the FRTC8900 RTC from a Unix epoch timestamp.
 *
 * Zephyr's built-in `rtc set` takes broken-down date/time strings; an epoch
 * integer is what a one-line host script or a BLE write can send without
 * formatting anything. The conversion and the range checking live here so the
 * shell command and the BLE characteristic share them rather than each
 * carrying a copy.
 */

#include "time_sync.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <errno.h>

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#endif

static const struct device *const rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc0));

/* Civil calendar from days-since-epoch (Howard Hinnant's algorithm) — avoids
 * pulling in libc gmtime() just for this one conversion. */
static void epoch_to_rtc_time(int64_t epoch, struct rtc_time *out)
{
    int64_t days = epoch / 86400;
    int64_t rem  = epoch % 86400;

    if (rem < 0) {
        rem  += 86400;
        days -= 1;
    }

    int64_t z   = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  y   = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp  = (5 * doy + 2) / 153;
    uint64_t d   = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m   = mp + (mp < 10 ? 3 : -9);

    y += (m <= 2);

    *out = (struct rtc_time){0};
    out->tm_year = (int)(y - 1900);
    out->tm_mon  = (int)(m - 1);
    out->tm_mday = (int)d;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min  = (int)((rem % 3600) / 60);
    out->tm_sec  = (int)(rem % 60);
    out->tm_wday = (int)(((days % 7) + 7 + 4) % 7); /* 1970-01-01 = Thursday */
}

/* Inverse of the above — days-since-epoch from a civil date. Same algorithm
 * run backwards, so a value written and then read back round-trips exactly. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2);

    int64_t  era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + (int64_t)doe - 719468;
}

int time_sync_set_epoch(int64_t epoch)
{
    if (epoch < 0 || epoch > TIME_SYNC_EPOCH_MAX) {
        return -EINVAL;
    }
    if (!device_is_ready(rtc_dev)) {
        return -ENODEV;
    }

    struct rtc_time t;

    epoch_to_rtc_time(epoch, &t);

    return rtc_set_time(rtc_dev, &t);
}

int64_t time_sync_get_epoch(void)
{
    if (!device_is_ready(rtc_dev)) {
        return -ENODEV;
    }

    struct rtc_time t;
    int rc = rtc_get_time(rtc_dev, &t);

    if (rc) {
        return rc;
    }

    int64_t days = days_from_civil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

#ifdef CONFIG_SHELL

static int cmd_settime(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);

    char *end;

    errno = 0;
    long long epoch = strtoll(argv[1], &end, 10);

    /* strtoll() silently clamps to LLONG_MAX/MIN and sets errno on overflow
     * rather than failing outright — an unchecked errno here let a numeral
     * too large to represent sail through the *end check, get truncated into
     * epoch_to_rtc_time()'s tm_year computation, and get written to the RTC
     * as an arbitrary wrapped date while reporting success. */
    if (*end != '\0' || errno == ERANGE) {
        shell_error(sh, "usage: settime <unix-epoch-seconds>");
        return -EINVAL;
    }

    int rc = time_sync_set_epoch(epoch);

    if (rc == -EINVAL) {
        shell_error(sh, "epoch out of range (0 .. %lld, i.e. before year 2100)",
                    TIME_SYNC_EPOCH_MAX);
        return rc;
    }
    if (rc == -ENODEV) {
        shell_error(sh, "RTC not ready");
        return rc;
    }
    if (rc) {
        shell_error(sh, "rtc_set_time failed: %d", rc);
        return rc;
    }

    struct rtc_time t;

    epoch_to_rtc_time(epoch, &t);
    shell_print(sh, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
    return 0;
}

SHELL_CMD_ARG_REGISTER(settime, NULL,
                        "Set RTC from Unix epoch seconds: settime <epoch>",
                        cmd_settime, 2, 0);

#endif /* CONFIG_SHELL */
