#include "battery.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <stdatomic.h>
#include <string.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

/*
 * Low-battery trigger is on VOLTAGE, not state of charge.
 *
 * The BQ27441's SoC output is only meaningful once design-capacity in the
 * devicetree matches the real cell, and it is still the 300 mAh placeholder.
 * On hardware that made the gauge report 5-10% on a healthy 3.65 V cell, which
 * fired the indicator on every poll: a red row painted on the highest-priority
 * layer, never cleared, plus a display_on() every 60 s that kept waking the
 * watch. Voltage needs no configuration to be trustworthy.
 *
 * Switch back to SoC once the real capacity is set and the gauge has learned.
 */
#define LOW_BATTERY_MV         3400  /* Li-ion getting genuinely low */
#define LOW_BATTERY_CLEAR_MV   3500  /* hysteresis, so it cannot chatter */
#define POLL_INTERVAL_S        60

static const struct device *bq = DEVICE_DT_GET(DT_ALIAS(batt0));

static atomic_int  g_percent  = ATOMIC_INIT(100);
static atomic_int  g_volt_mv  = ATOMIC_INIT(3700);
static atomic_int  g_charging = ATOMIC_INIT(0);

static void battery_work_fn(struct k_work *work);
static K_WORK_DEFINE(battery_work, battery_work_fn);

/* Only true while the warning is actually being displayed, so the indicator is
 * drawn and the display woken on the transition into low battery rather than on
 * every poll. */
static bool low_shown;

/* Dim red stripe on row 6 (lowest row) — visible but not alarming */
static void show_low_battery_indicator(void)
{
    k_mutex_lock(&led_mask_mutex, K_FOREVER);
    memset(led_mask[LED_LAYER_NOTIFICATION], 0,
           sizeof(led_mask[LED_LAYER_NOTIFICATION]));
    for (int col = 0; col < LED_COLS; col++) {
        led_mask[LED_LAYER_NOTIFICATION][LED_ROWS - 1][col] = 1;
    }
    led_layer_color[LED_LAYER_NOTIFICATION] = (struct led_rgb){180, 0, 0};
    k_mutex_unlock(&led_mask_mutex);
    display_on();
    led_commit();
}

static void clear_low_battery_indicator(void)
{
    k_mutex_lock(&led_mask_mutex, K_FOREVER);
    memset(led_mask[LED_LAYER_NOTIFICATION], 0,
           sizeof(led_mask[LED_LAYER_NOTIFICATION]));
    led_layer_color[LED_LAYER_NOTIFICATION] = (struct led_rgb){0, 0, 0};
    k_mutex_unlock(&led_mask_mutex);
    led_commit();
}

static void battery_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = sensor_sample_fetch(bq);
    if (err) {
        LOG_WRN("BQ27441 fetch failed: %d", err);
        return;
    }

    struct sensor_value soc, volt, current;
    int ch_err;

    ch_err = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &soc);
    if (ch_err) { LOG_WRN("SoC channel read failed: %d", ch_err); return; }
    ch_err = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_VOLTAGE, &volt);
    if (ch_err) { LOG_WRN("Voltage channel read failed: %d", ch_err); return; }
    ch_err = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_AVG_CURRENT, &current);
    if (ch_err) { LOG_WRN("Current channel read failed: %d", ch_err); return; }

    uint8_t  pct  = (uint8_t)soc.val1;
    uint32_t mv   = (uint32_t)(volt.val1 * 1000 + volt.val2 / 1000);
    /* BQ27441 current: val1 = mA integer part, val2 = µA fractional part.
     * val1 > 0 alone misses charge currents below 1 mA (val1=0, val2>0). */
    bool     chrg = (current.val1 > 0) ||
                    (current.val1 == 0 && current.val2 > 0);

    atomic_store(&g_percent,  pct);
    atomic_store(&g_volt_mv,  (int)mv);
    atomic_store(&g_charging, chrg ? 1 : 0);

    LOG_INF("Battery: %u%% %umV %s",
            pct, mv, chrg ? "charging" : "discharging");

    /* Edge-triggered: show it once on the way down, clear it once on the way
     * back up. Re-asserting every poll is what pinned the row on and kept the
     * display waking itself. */
    if (!low_shown && mv < LOW_BATTERY_MV) {
        low_shown = true;
        LOG_WRN("Battery low: %u mV", mv);
        show_low_battery_indicator();
    } else if (low_shown && mv >= LOW_BATTERY_CLEAR_MV) {
        low_shown = false;
        LOG_INF("Battery recovered: %u mV", mv);
        clear_low_battery_indicator();
    }
}

static void battery_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&battery_work);
}

static K_TIMER_DEFINE(battery_timer, battery_timer_cb, NULL);

void battery_init(void)
{
    if (!device_is_ready(bq)) {
        LOG_ERR("BQ27441 not ready");
        return;
    }

    /* Read immediately on boot, then every 60 s */
    k_work_submit(&battery_work);
    k_timer_start(&battery_timer,
                  K_SECONDS(POLL_INTERVAL_S),
                  K_SECONDS(POLL_INTERVAL_S));

    LOG_INF("Battery monitor started (poll every %ds)", POLL_INTERVAL_S);
}

uint8_t  battery_percent(void)    { return (uint8_t)atomic_load(&g_percent); }
uint32_t battery_voltage_mv(void) { return (uint32_t)atomic_load(&g_volt_mv); }
bool     battery_charging(void)   { return atomic_load(&g_charging) != 0; }
bool     battery_is_low(void)     { return battery_voltage_mv() < LOW_BATTERY_MV; }
