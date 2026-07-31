#include "light.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(light, LOG_LEVEL_INF);

#define POLL_INTERVAL_MS 2000

static const struct device *bh = DEVICE_DT_GET(DT_ALIAS(light0));

/*
 * Piecewise-linear lux → brightness curve.
 *
 * The output is a fraction of led_max_brightness, not an absolute drive level,
 * so 255 here means "as bright as this watch ever gets" rather than full power.
 *
 * Tuned for something worn on a wrist and looked at up close. The bottom end
 * matters most: a watch glanced at in a dark room needs far less output than
 * the eye expects on paper, and the previous curve bottomed out at 30 (12%),
 * which is genuinely dazzling at night. Human brightness perception is roughly
 * logarithmic, which is why the lux breakpoints climb by decades while the
 * output climbs roughly linearly.
 */
static uint8_t lux_to_brightness(uint32_t lux)
{
    static const struct {
        uint32_t lux;
        uint8_t  brightness;
    } pts[] = {
        {0,        8},   /* pitch dark, or face-down on a table / in a pocket */
        {5,       20},   /* dim room at night                                 */
        {50,      60},   /* normal indoor evening lighting                    */
        {200,    110},   /* well-lit room, office                             */
        {1000,   180},   /* bright indoors, or overcast outdoors              */
        {10000,  255},   /* direct sunlight — everything it has               */
    };

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

    int err = sensor_sample_fetch(bh);
    if (err) {
        LOG_WRN("BH1750 fetch failed: %d", err);
        return;
    }

    struct sensor_value lux_val;
    if (sensor_channel_get(bh, SENSOR_CHAN_LIGHT, &lux_val)) {
        LOG_WRN("BH1750 channel read failed");
        return;
    }

    uint32_t lux = (uint32_t)lux_val.val1;
    uint8_t  br  = lux_to_brightness(lux);

    led_brightness = br;

    LOG_DBG("Light: %u lux → brightness %u", lux, br);
}

static K_WORK_DEFINE(light_work, light_work_fn);

static void light_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&light_work);
}

static K_TIMER_DEFINE(light_timer, light_timer_cb, NULL);

void light_init(void)
{
    if (!device_is_ready(bh)) {
        LOG_ERR("BH1750 not ready");
        return;
    }

    /* Read immediately, then every 2 s */
    k_work_submit(&light_work);
    k_timer_start(&light_timer,
                  K_MSEC(POLL_INTERVAL_MS),
                  K_MSEC(POLL_INTERVAL_MS));

    LOG_INF("Light sensor started (poll every %dms)", POLL_INTERVAL_MS);
}
