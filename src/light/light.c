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
 * A sample runs on this queue and not the system one.
 *
 * The BH1750 driver uses the sensor's one-shot modes, so sensor_sample_fetch()
 * arms a conversion and then k_msleep()s out the integration time — 120 ms
 * typical, 180 ms worst case. On the system workqueue that is 180 ms during
 * which nothing else queued there runs, and buttons.c samples its gesture state
 * machine from that same queue every 15 ms. A light reading would have stalled
 * gesture recognition for a dozen sample periods.
 *
 * Priority is deliberately below the sand (5) and IMU (4) threads: this blocks
 * for a long time and nothing is waiting on it.
 */
#define LIGHT_STACK_SIZE 1024
#define LIGHT_PRIORITY   10

static K_THREAD_STACK_DEFINE(light_stack, LIGHT_STACK_SIZE);
static struct k_work_q light_q;

/*
 * Poll rate, which adapts to whether the watch is in use.
 *
 * Sampling only when the display goes dark tracks a room that changes while
 * the watch is being used, but not one that changes while it sits idle — left
 * out overnight, it would wake at whatever brightness the room had when it was
 * last looked at.
 *
 * Two rates rather than one, because the two situations have opposite costs.
 * Just after someone has looked at the watch they are likely to look again
 * soon, and may well be carrying it between rooms, so the reading needs to be
 * current: poll every second. Once it has been sitting untouched for a minute,
 * nobody is waiting on freshness and the only thing that matters is idle
 * current: drop to every ten seconds.
 *
 * The fast rate is the expensive one — 180 ms of conversion per second is an
 * 18% duty cycle, about 22 uA on a part drawing ~120 uA active — but it only
 * runs in the minute after an interaction, which is active time anyway. The
 * idle rate is 1.8%, well inside the <10 uA target. Polls that land while the
 * display is on cost nothing at all: light_work_fn() bails before it touches
 * the sensor.
 */
#define LIGHT_POLL_ACTIVE_MS    1000
#define LIGHT_POLL_IDLE_MS     10000
#define LIGHT_ACTIVE_WINDOW_MS 60000

/*
 * Piecewise-linear lux → brightness curve.
 *
 * This is now the primary brightness control. It used to be scaled again by
 * led_max_brightness (32, ~12%), which compressed this whole 0-255 range into
 * a narrow output band and cost most of the colour resolution; that ceiling is
 * off by default now, so these values map to real drive levels and the current
 * budget handles the safety side. Anything tuned against the old behaviour is
 * worth revisiting — the top of this curve is roughly 8x brighter than it was.
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
 *
 * That floor was 16 and is now 24, because at 16 the densest part of the
 * waterfall tore holes in itself: 18% of the colour wheel's hues rounded to
 * black. 24 clears it, but only just — it is not the real fix, and should not
 * be treated as a value that can be lowered freely. build_buffers() now floors
 * any lit pixel to at least one channel, which is what actually makes this
 * robust; this bump is for headroom on top of it.
 */
static uint8_t lux_to_brightness(uint32_t lux)
{
    static const struct {
        uint32_t lux;
        uint8_t  brightness;
    } pts[] = {
        {5,        24},   /* pitch dark, or face-down on a table / in a pocket */
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
     * pattern for the BMI260. Whether this has any real effect depends on
     * the BH1750 Zephyr driver actually implementing PM_DEVICE actions;
     * unverified here, same caveat as imu.c's equivalent calls. */
    (void)pm_device_action_run(bh, PM_DEVICE_ACTION_RESUME);

    int err = sensor_sample_fetch(bh);

    /*
     * Re-check, because the fetch above was not instant: the driver uses the
     * sensor's one-shot mode and sleeps out the integration time, up to 180 ms.
     * A button pressed inside that window lights the display part-way through
     * the conversion, and the result is then partly a measurement of the watch's
     * own LEDs — which, fed back into brightness, is the feedback loop the
     * display-off check exists to prevent. Checking only on entry misses it.
     */
    if (display_is_on()) {
        LOG_DBG("Light: display woke mid-conversion, discarding");
        (void)pm_device_action_run(bh, PM_DEVICE_ACTION_SUSPEND);
        return;
    }

    if (!err) {
        struct sensor_value lux_val;

        if (sensor_channel_get(bh, SENSOR_CHAN_LIGHT, &lux_val)) {
            LOG_WRN("BH1750 channel read failed");
        } else {
            uint32_t lux = (uint32_t)lux_val.val1;
            uint8_t  br  = lux_to_brightness(lux);

            led_brightness = br;
            /* INFO, not DEBUG: this fires once per display-off and once a
             * minute while idle, so it is not noisy — and it is the only
             * outward sign the sensor is feeding anything at all. The bug
             * this replaced was invisible precisely because nothing logged. */
            LOG_INF("Light: %u lux -> brightness %u", lux, br);
        }
    } else {
        LOG_WRN("BH1750 fetch failed: %d", err);
    }

    (void)pm_device_action_run(bh, PM_DEVICE_ACTION_SUSPEND);
}

static K_WORK_DEFINE(light_work, light_work_fn);

static void poll_timer_cb(struct k_timer *t);
static K_TIMER_DEFINE(poll_timer, poll_timer_cb, NULL);

static int64_t  last_activity_ms;
static uint32_t poll_period_ms;

/* Retune the timer only on an actual rate change. k_timer_start() restarts the
 * period from now, so calling it every tick would keep pushing the next
 * expiry back and the poll would drift slower than asked. */
static void set_poll_period(uint32_t ms)
{
    if (poll_period_ms == ms) {
        return;
    }
    poll_period_ms = ms;
    k_timer_start(&poll_timer, K_MSEC(ms), K_MSEC(ms));
}

static void poll_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);

    /* Self-gating: light_work_fn() returns immediately if the display is on,
     * so this can free-run without knowing the UI state. */
    light_sample_now();

    if (k_uptime_get() - last_activity_ms > LIGHT_ACTIVE_WINDOW_MS) {
        set_poll_period(LIGHT_POLL_IDLE_MS);
    }
}

/*
 * The watch was just woken. Sample often for a while.
 *
 * Called from display_on() — i.e. every time the display lights for any
 * reason, which is the definition of "someone is using this". The window
 * decays on its own from here: the poll callback drops back to the idle rate
 * once this timestamp is old enough, so nothing has to notice the user
 * stopping.
 */
void light_notify_activity(void)
{
    last_activity_ms = k_uptime_get();
    set_poll_period(LIGHT_POLL_ACTIVE_MS);
}

/*
 * Queue a reading. ISR-safe: only submits work, no I2C here.
 *
 * This is called when the display goes OFF, not when a button asks it to come
 * on, and the difference is the whole point. The old arrangement sampled from
 * the button handler — which could never work, because that handler runs on
 * the system workqueue and this submitted to the same queue, so the read could
 * not start until the handler had returned and already turned the display on.
 * light_work_fn()'s is_on guard then discarded every reading, silently, and
 * led_brightness sat at its power-on default forever.
 *
 * Sampling on the way down instead is correct by construction: the LEDs are
 * definitively dark, nothing is waiting on the result, and the value is ready
 * well before the next wake.
 */
void light_sample_now(void)
{
    k_work_submit_to_queue(&light_q, &light_work);
}

void light_init(void)
{
    if (!device_is_ready(bh)) {
        LOG_ERR("BH1750 not ready");
        return;
    }

    k_work_queue_init(&light_q);
    k_work_queue_start(&light_q, light_stack, K_THREAD_STACK_SIZEOF(light_stack),
                       LIGHT_PRIORITY, NULL);
    k_thread_name_set(&light_q.thread, "light");

    /* One baseline so led_brightness is not sitting at its default for the
     * first wake. ui_init() has not run yet, so the display still reports on
     * and an immediate sample would be discarded — start at the active rate
     * and the first tick lands after ui_init() has blanked and powered the
     * panel down. */
    poll_period_ms   = LIGHT_POLL_ACTIVE_MS;
    last_activity_ms = k_uptime_get();
    k_timer_start(&poll_timer, K_MSEC(500), K_MSEC(LIGHT_POLL_ACTIVE_MS));

    LOG_INF("Light sensor ready (%d ms polling in use, %d ms idle)",
            LIGHT_POLL_ACTIVE_MS, LIGHT_POLL_IDLE_MS);
}
