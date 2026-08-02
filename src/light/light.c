#include "light.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(light, LOG_LEVEL_INF);

static const struct device *bh = DEVICE_DT_GET(DT_ALIAS(light0));

/*
 * Piecewise-linear lux → brightness curve.
 *
 * The output is a fraction of led_max_brightness (32 by default, ~12%), not
 * an absolute drive level, so 255 here means "as bright as this watch ever
 * gets": effective brightness = this value * led_max_brightness / 255, so
 * most of this 0-255 range maps into a fairly narrow real output band.
 *
 * These breakpoints are a starting guess for a watch worn on a wrist and
 * looked at up close, not a measured calibration — this LED/diffuser
 * combination is custom, so there's no standard WS2812B brightness reference
 * to lean on. Expect to retune by eye on the actual hardware (`led ambient`
 * and `led max` in the shell force a level without waiting on the sensor).
 *
 * The floor at 0 lux is deliberately not zero: full darkness should still
 * leave a dim, clearly-on glow, not a blackout — a watch that goes fully dark
 * when covered or face-down reads as broken, not power-saving.
 */
static uint8_t lux_to_brightness(uint32_t lux)
{
    static const struct {
        uint32_t lux;
        uint8_t  brightness;
    } pts[] = {
        {5,        16},   /* pitch dark, or face-down on a table / in a pocket */
        {10,       60},   /* dim room at night                                 */
        {50,       75},   /* normal indoor evening lighting                    */
        {200,      95},   /* well-lit room, office                             */
        {1000,    170},   /* bright indoors, or overcast outdoors              */
        {10000,   255},   /* direct sunlight — everything it has               */
    };

    /* lux below the first breakpoint must clamp to its floor, not fall into
     * the loop below: at i=1 the loop computes `lux - pts[0].lux`, and for
     * lux < 5 that's a uint32_t underflow (e.g. lux=0 -> 0-5 wraps to
     * 0xFFFFFFFB), producing an essentially arbitrary brightness instead of
     * the floor — breaking the darkness floor specifically for the darkness
     * it exists for (face-down on a table, in a pocket). */
    if (lux <= pts[0].lux) {
        return pts[0].brightness;
    }

    for (int i = 1; i < (int)ARRAY_SIZE(pts); i++) {
        if (lux <= pts[i].lux) {
            uint32_t dl = pts[i].lux        - pts[i-1].lux;
            uint32_t db = pts[i].brightness - pts[i-1].brightness;
            return (uint8_t)(pts[i-1].brightness +
                             (lux - pts[i-1].lux) * db / dl);
        }
    }
    return 255;
}

static void light_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Only sample when the LEDs are off — the WS2812B output would
     * otherwise reach the sensor and create a brightness feedback loop. */
    if (display_is_on()) {
        return;
    }

    /* Resume/suspend around the read rather than leaving the sensor
     * powered continuously between on-demand samples — mirrors imu.c's
     * pattern for the BMI270. Whether this has any real effect depends on
     * the BH1750 Zephyr driver actually implementing PM_DEVICE actions;
     * unverified here, same caveat as imu.c's equivalent calls. */
    (void)pm_device_action_run(bh, PM_DEVICE_ACTION_RESUME);

    int err = sensor_sample_fetch(bh);

    if (!err) {
        struct sensor_value lux_val;

        if (sensor_channel_get(bh, SENSOR_CHAN_LIGHT, &lux_val)) {
            LOG_WRN("BH1750 channel read failed");
        } else {
            uint32_t lux = (uint32_t)lux_val.val1;
            uint8_t  br  = lux_to_brightness(lux);

            led_brightness = br;
            LOG_DBG("Light: %u lux → brightness %u", lux, br);
        }
    } else {
        LOG_WRN("BH1750 fetch failed: %d", err);
    }

    (void)pm_device_action_run(bh, PM_DEVICE_ACTION_SUSPEND);
}

static K_WORK_DEFINE(light_work, light_work_fn);

/*
 * Sample on demand rather than on a fixed 2 s timer. A periodic poll meant
 * the reading backing the next button press could be up to 2 s stale; this
 * way it's taken right as the press happens instead. Callers must trigger
 * this while the display is still off — light_work_fn's own is_on check
 * would otherwise silently skip the read.
 *
 * ISR-safe: only submits work, does not touch I2C directly.
 */
void light_sample_now(void)
{
    k_work_submit(&light_work);
}

void light_init(void)
{
    if (!device_is_ready(bh)) {
        LOG_ERR("BH1750 not ready");
        return;
    }

    /* One baseline reading so led_brightness isn't sitting at its default
     * before the first button press. */
    light_sample_now();

    LOG_INF("Light sensor ready (sampled on button press)");
}
