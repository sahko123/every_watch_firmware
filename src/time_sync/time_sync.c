/*
 * Shell command to set the FRTC8900 RTC from a Unix epoch timestamp, e.g. for
 * a bench script that has no BLE stack to talk to. `rtc set` (Zephyr's
 * built-in RTC shell) takes broken-down date/time strings; this takes a raw
 * epoch integer instead, which is what a one-line host script can send
 * without formatting a timestamp.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <stdlib.h>

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

static int cmd_settime(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);

    if (!device_is_ready(rtc_dev)) {
        shell_error(sh, "RTC not ready");
        return -ENODEV;
    }

    char *end;

    errno = 0;
    long long epoch = strtoll(argv[1], &end, 10);

    /* strtoll() silently clamps to LLONG_MAX/MIN and sets errno on overflow
     * rather than failing outright — an unchecked errno here let a numeral
     * too large to represent sail through both the *end and epoch<0 checks,
     * get truncated into epoch_to_rtc_time()'s tm_year computation, and get
     * written to the RTC as an arbitrary wrapped date while reporting
     * success. Also reject anything past a sane range: the RTC/UI have no
     * business representing a date this firmware will never see. */
    if (*end != '\0' || epoch < 0 || errno == ERANGE) {
        shell_error(sh, "usage: settime <unix-epoch-seconds>");
        return -EINVAL;
    }
    if (epoch > 4102444800LL) {  /* 2100-01-01T00:00:00Z */
        shell_error(sh, "epoch out of range (must be before year 2100)");
        return -EINVAL;
    }

    struct rtc_time t;

    epoch_to_rtc_time(epoch, &t);

    int rc = rtc_set_time(rtc_dev, &t);

    if (rc) {
        shell_error(sh, "rtc_set_time failed: %d", rc);
        return rc;
    }

    shell_print(sh, "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
    return 0;
}

SHELL_CMD_ARG_REGISTER(settime, NULL,
                        "Set RTC from Unix epoch seconds: settime <epoch>",
                        cmd_settime, 2, 0);
