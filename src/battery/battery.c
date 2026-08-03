#include "battery.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"
#include "time_display/time_display.h"
#include "sand/sand.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

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

/*
 * 15 s rather than 60. The poll is what detects power being connected, and a
 * readout that can take a minute to appear after plugging in reads as broken.
 * The SGM41524 charge indicator below normally beats it to the punch; this is
 * the backstop for if that line is not behaving.
 */
#define POLL_INTERVAL_S        15

static const struct device *bq = DEVICE_DT_GET(DT_ALIAS(batt0));

/*
 * SGM41524 charge indicator, P0.05, active low. Used only as a hint that
 * something changed: it fires an immediate gauge read, and the BQ27441's
 * average current is what actually decides "charging". That keeps the decision
 * on the fuel gauge while still reacting the instant the cable goes in.
 *
 * Both edges, because unplugging matters too.
 */
#define CHG_NODE DT_NODELABEL(chg_indicator)
#if DT_NODE_EXISTS(CHG_NODE)
static const struct gpio_dt_spec chg_pin = GPIO_DT_SPEC_GET(CHG_NODE, gpios);
static struct gpio_callback chg_cb;
#endif

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

/* --------------------------------------------------------------------------
 * Battery level screen — right button, 3 second hold
 * -------------------------------------------------------------------------- */

#define LEVEL_SHOW_MS      3000  /* right-button hold, while not charging */
#define LEVEL_ROW             1  /* 5-row font on a 7-row grid, 1px margin */
#define LEVEL_LOW_PCT        20  /* below this the readout goes red */

#define WAVE_STEP_MS         60  /* animation tick */
#define WAVE_COL_SHIFT       12  /* phase offset per column — sets wavelength */

/*
 * Colour encodes charge level; motion encodes charging.
 *
 * Discharging is a flat colour, charging adds a hue wave travelling across the
 * text. Keeping the two on separate axes means a nearly-flat battery still
 * reads as red while you can see at a glance that it is charging — which is
 * exactly when you most want both facts at once.
 */
static struct led_rgb level_base_color(uint8_t pct, bool charging)
{
    if (pct < LEVEL_LOW_PCT) return (struct led_rgb){200,   0,  0}; /* red   */
    if (charging)            return (struct led_rgb){  0, 180, 40}; /* green */
    return (struct led_rgb){0, 90, 220};                            /* blue  */
}

/* Triangle wave, 0-254, so the shimmer runs smoothly both ways without a
 * sine table or any floating point. */
static uint8_t tri(uint8_t x)
{
    return (x < 128) ? (uint8_t)(x * 2) : (uint8_t)((255 - x) * 2);
}

static void level_dismiss(void);
static void level_clear_work_fn(struct k_work *work);
static K_WORK_DEFINE(level_clear_work, level_clear_work_fn);

static void level_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    k_work_submit(&level_clear_work);
}

static K_TIMER_DEFINE(level_timer, level_timer_cb, NULL);

static bool    level_showing;

/* True for the whole time the watch is plugged in and charging, not just a
 * brief peek: the readout stays up, refreshing each poll, until unplugged.
 * A manual right-button hold is ignored while this is set — the screen is
 * already showing the thing the hold would ask for. */
static bool    charging_mode;

static bool    level_wave;      /* animate — latched at show time */
static uint8_t level_pct;       /* latched, so the wave cannot change colour
                                 * band mid-animation if a poll lands */
static uint8_t wave_phase;

/* Repaint led_color[] for the current phase. The notification layer's
 * layer_color is left at {0,0,0} while the readout is up, so the compositor
 * takes each pixel's colour from led_color[] and we can vary it per column. */
static void level_paint(void)
{
    struct led_rgb base = level_base_color(level_pct, level_wave);

    /* led_color[] shares build_buffers()'s lock contract with led_mask[] —
     * see led_matrix.h. This used to write it unlocked, racing the
     * compositor every 60 ms for the whole duration of a charging session. */
    k_mutex_lock(&led_mask_mutex, K_FOREVER);

    for (int c = 0; c < LED_COLS; c++) {
        struct led_rgb px = base;

        if (level_wave) {
            /* Small excursion only — the text should shimmer, not strobe. */
            uint8_t d = tri((uint8_t)(wave_phase + c * WAVE_COL_SHIFT)) / 8;

            if (level_pct < LEVEL_LOW_PCT) {
                px.g = d;                       /* red -> slight orange   */
            } else {
                px.r = d;                       /* green -> yellow/cyan   */
                px.b = (uint8_t)(40 + (31 - d));
            }
        }

        for (int r = 0; r < LED_ROWS; r++) {
            led_color[r][c] = px;
        }
    }

    k_mutex_unlock(&led_mask_mutex);
}

static void wave_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    if (!level_showing || !level_wave) {
        return;
    }

    /* No longer needs to hold the display awake: page lifetime is the UI's
     * business now, and the battery page carries its own timeout. This used
     * to call display_reset_timeout() every tick purely to stop the old
     * global 10 s auto-off firing between battery polls (up to 15 s apart)
     * while the persistent charging view was up. */

    wave_phase += 4;
    level_paint();
    led_commit();
}

static K_WORK_DEFINE(wave_work, wave_work_fn);

static void wave_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    k_work_submit(&wave_work);
}

static K_TIMER_DEFINE(wave_timer, wave_timer_cb, NULL);

/* Take the readout down cleanly. Shared by the timed dismiss (right-button
 * peek) and the immediate one (unplugging out of the persistent charging
 * view) — both need the identical teardown. */
static void level_dismiss(void)
{
    if (!level_showing) {
        return;
    }
    level_showing = false;
    k_timer_stop(&level_timer);
    k_timer_stop(&wave_timer);

    k_mutex_lock(&led_mask_mutex, K_FOREVER);
    memset(led_mask[LED_LAYER_NOTIFICATION], 0,
           sizeof(led_mask[LED_LAYER_NOTIFICATION]));
    led_layer_color[LED_LAYER_NOTIFICATION] = (struct led_rgb){0, 0, 0};
    k_mutex_unlock(&led_mask_mutex);

    /* The readout painted led_color[] to drive the wave. Put the sand amber
     * back, or free-mode sand comes back green/blue. */
    led_color_reset();

    time_display_resume();

    /* The readout borrowed the notification layer, which is also where the low
     * battery stripe lives. Put it back if it was up when we took over —
     * otherwise clearing here would silently cancel the warning. */
    if (low_shown) {
        show_low_battery_indicator();
    } else {
        led_commit();
    }
}

static void level_clear_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    level_dismiss();
}

/* Called by time_display_reveal() before it takes over the whole screen.
 * Without this, a button press or wrist-tilt during the battery percentage
 * peek — or worse, during the persistent charging view — left level_showing/
 * charging_mode/wave_timer/level_timer all still live while the reveal wiped
 * the mask out from under them: wave_timer kept firing every 60ms, repainting
 * led_color[] over whatever the reveal had just drawn, and time_display's
 * `paused` flag (only ever cleared by level_dismiss()'s time_display_resume()
 * call) stayed stuck true for the rest of the charging session, freezing the
 * clock. Safe to call unconditionally from workqueue context: every caller of
 * time_display_reveal() (button ISR -> reveal_work, wrist-tilt trigger, both
 * via display_wake_and_reveal()) runs on the system workqueue, the same
 * thread every other battery.c state mutation runs on. */
void battery_screen_dismiss(void)
{
    if (level_showing) {
        level_dismiss();
    }

    /* charging_mode means "the persistent view is (meant to be) up", which
     * is no longer true once the reveal has taken over. Self-heals on the
     * next poll or charge-indicator edge: if still actually charging,
     * battery_work_fn() puts the view right back. */
    charging_mode = false;
}

/* Called by display.c's display_off(). The low-battery stripe on
 * LED_LAYER_NOTIFICATION is wiped along with everything else when the
 * display goes dark, but low_shown is edge-triggered specifically so it
 * doesn't re-assert on every poll — with nothing resetting it here, that
 * meant the warning could be shown at most once per low-battery episode: the
 * next redraw only happened if voltage recovered above the clear threshold
 * and dropped low again, or on reboot. Resetting it lets the next poll (up
 * to POLL_INTERVAL_S later) re-assert it if voltage is still low, so a
 * critically low battery keeps getting a periodic reminder instead of one
 * warning that silently stops meaning anything the moment the screen times
 * out. */
void battery_notify_display_off(void)
{
    low_shown = false;
}

static void level_show_for(uint32_t show_ms)
{
    uint8_t pct  = battery_percent();
    bool    chrg = battery_charging();

    /* Re-reading the gauge here would block this call on I2C for the sake of a
     * value the poll already keeps fresh. Use the cached reading. */

    uint8_t bitmap[LED_ROWS][LED_COLS];
    memset(bitmap, 0, sizeof(bitmap));

    /* Centre "NN%": each glyph is 3 wide with 1 column between, and the percent
     * sign occupies a glyph cell of its own. 100% is 4 glyphs = 15 columns,
     * which is the widest case and still fits the 20-column grid. */
    int digits = (pct >= 100) ? 3 : (pct >= 10) ? 2 : 1;
    int glyphs = digits + 1;
    int width  = glyphs * 3 + (glyphs - 1);
    int col    = (LED_COLS - width) / 2;

    if (digits == 3) {
        led_stamp_digit(bitmap, (pct / 100) % 10, LEVEL_ROW, col);
        led_stamp_digit(bitmap, (pct /  10) % 10, LEVEL_ROW, col + 4);
        led_stamp_digit(bitmap,  pct        % 10, LEVEL_ROW, col + 8);
    } else if (digits == 2) {
        led_stamp_digit(bitmap, (pct / 10) % 10, LEVEL_ROW, col);
        led_stamp_digit(bitmap,  pct       % 10, LEVEL_ROW, col + 4);
    } else {
        led_stamp_digit(bitmap, pct % 10, LEVEL_ROW, col);
    }
    led_stamp_percent(bitmap, LEVEL_ROW, col + digits * 4);

    /* Latch both, so a poll landing mid-animation cannot switch colour band or
     * start/stop the wave under the repaint. */
    level_pct  = pct;
    level_wave = chrg;

    /* Own the display outright: stop the clock republishing underneath, and
     * clear any settled sand so the number is not read against a pile of it. */
    time_display_pause();
    sand_clear();

    level_paint();

    k_mutex_lock(&led_mask_mutex, K_FOREVER);
    led_mask_clear_all();
    memcpy(led_mask[LED_LAYER_NOTIFICATION], bitmap, sizeof(bitmap));
    /* Zero = "use led_color[] per cell", which is what level_paint() drives. */
    led_layer_color[LED_LAYER_NOTIFICATION] = (struct led_rgb){0, 0, 0};
    k_mutex_unlock(&led_mask_mutex);

    level_showing = true;
    display_on();          /* also resets the auto-off timeout */
    led_commit();

    if (level_wave) {
        k_timer_start(&wave_timer, K_MSEC(WAVE_STEP_MS), K_MSEC(WAVE_STEP_MS));
    }

    /* show_ms == 0 means persistent (the charging view): no auto-dismiss,
     * and cancel any peek timer left running from an earlier call. */
    if (show_ms > 0) {
        k_timer_start(&level_timer, K_MSEC(show_ms), K_NO_WAIT);
    } else {
        k_timer_stop(&level_timer);
    }

    LOG_INF("Battery level: %u%% %umV %s",
            pct, battery_voltage_mv(), chrg ? "charging" : "discharging");
}

/* Runs the actual check-and-show on the system workqueue — the same context
 * that owns charging_mode/level_pct/level_wave/wave_phase everywhere else
 * (battery_work_fn, wave_work_fn, level_clear_work_fn). battery_show_level()
 * used to run this check-then-act directly on the calling (main) thread,
 * which was a second, unsynchronized writer of that state: a right-button
 * hold landing in the same instant as a plug-in (battery_work_fn fires
 * immediately off the SGM41524 edge) could read charging_mode as false right
 * before battery_work_fn set it true and called level_show_for(0), then
 * proceed to call level_show_for(LEVEL_SHOW_MS) anyway — silently imposing a
 * 3 s dismiss timer on what's supposed to be a persistent charging view. */
static void show_level_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Already up and will keep refreshing itself every poll — a hold here
     * would just impose a 3 s dismiss timer on a view meant to persist. */
    if (charging_mode) {
        return;
    }
    level_show_for(LEVEL_SHOW_MS);
}
static K_WORK_DEFINE(show_level_work, show_level_work_fn);

void battery_show_level(void)
{
    k_work_submit(&show_level_work);
}

/*
 * Everything else the BQ27441 knows, logged alongside the headline reading.
 *
 * The gauge exposes twelve channels and this code used three. The other nine
 * are worth having for two specific reasons:
 *
 * FULL_CHARGE_CAPACITY is the capacity the gauge has *learned*, as opposed to
 * the design-capacity someone typed into the devicetree. After one full charge
 * and discharge it is the empirical answer to whether that 400 mAh figure is
 * right — and taper-current is currently set to the C/10 convention against
 * that unverified number, so the gauge is modelling on a guess.
 *
 * AVG_CURRENT measures whole-board draw. The LED current budget, the display
 * page costs and the idle targets in this firmware are all arithmetic derived
 * from a 0.078 mA-per-sum-unit conversion that has never been checked against
 * a measurement. This makes checking it a matter of putting a page up and
 * reading the log.
 *
 * Note the resolution limit before trusting it too far: the BQ27441 reports
 * current in whole mA. That is ample for display-on currents in the tens to
 * hundreds, and useless for the sub-10 uA idle target, which still needs a
 * proper meter.
 *
 * Channels are read individually and a failure on one is not fatal — a driver
 * that does not implement a channel should not cost us the ones that work.
 */
static void log_gauge_detail(struct sensor_value current)
{
    struct sensor_value full_cap, rem_cap, health, power, temp;
    int rc;

    int full_mah   = (rc = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_FULL_CHARGE_CAPACITY,
                                              &full_cap)) ? -1 : full_cap.val1;
    int rem_mah    = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_REMAINING_CHARGE_CAPACITY,
                                        &rem_cap)  ? -1 : rem_cap.val1;
    int health_pct = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_STATE_OF_HEALTH,
                                        &health)   ? -1 : health.val1;
    int power_mw   = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_AVG_POWER,
                                        &power)    ? -1 : power.val1;
    int temp_c     = sensor_channel_get(bq, SENSOR_CHAN_GAUGE_TEMP,
                                        &temp)     ? -1 : temp.val1;

    ARG_UNUSED(rc);

    /*
     * val1 is AMPS and val2 is MICROAMPS — the driver takes the gauge's raw
     * mA and splits it as val1 = mA/1000, val2 = (mA%1000)*1000. Reading val1
     * as milliamps understates everything by a factor of a thousand, which is
     * an easy mistake to make and a hard one to notice: a watch drawing 3 mA
     * prints as "0.003" and looks like a plausible sleeping current.
     *
     * Signed, negative being discharge. C truncates division toward zero, so a
     * discharge of -50 mA arrives as val1 = 0, val2 = -50000.
     */
    int cur_ma = current.val1 * 1000 + current.val2 / 1000;

    LOG_INF("  gauge: %d mA  %d mW  %d/%d mAh  health %d%%  %d C",
            cur_ma, power_mw, rem_mah, full_mah, health_pct, temp_c);

    /* The learned capacity against what the devicetree claims. They diverging
     * is the point of printing it — design-capacity is an assertion, this is a
     * measurement, and the gauge only reaches a trustworthy figure after a full
     * cycle. -1 means the driver declined the channel. */
    if (full_mah > 0) {
        int design = DT_PROP(DT_NODELABEL(bq27441), design_capacity);

        if (full_mah < (design * 8) / 10 || full_mah > (design * 12) / 10) {
            LOG_WRN("  gauge: learned capacity %d mAh vs design-capacity %d mAh"
                    " — check the cell fitted, or let it finish a full cycle",
                    full_mah, design);
        }
    }
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
    /* val1 is AMPS, val2 is MICROAMPS — see log_gauge_detail() above, which
     * documents why that is easy to misread. This comment previously claimed
     * val1 was milliamps, which would make the val1 > 0 test require a whole
     * amp before it ever fired.
     *
     * The test is still correct as written, but only because val2 carries
     * everything below 1 A: charge currents on this watch never reach val1 at
     * all, so val2 > 0 is what actually detects charging. */
    bool     chrg = (current.val1 > 0) ||
                    (current.val1 == 0 && current.val2 > 0);

    atomic_store(&g_percent,  pct);
    atomic_store(&g_volt_mv,  (int)mv);
    atomic_store(&g_charging, chrg ? 1 : 0);

    LOG_INF("Battery: %u%% %umV %s",
            pct, mv, chrg ? "charging" : "discharging");

    log_gauge_detail(current);

    /* While charging, own the display for the whole session rather than a
     * timed peek: show the readout on plug-in, refresh it each poll so the
     * percentage climbs live, and take it down the moment power is removed.
     *
     * first_poll suppresses drawing anything at boot: battery_init() runs
     * before main() blanks the display and drops to idle, so a readout drawn
     * here would be wiped a moment later and look like a glitch. If the watch
     * boots already on charge, charging_mode still latches true here so the
     * very next poll (up to POLL_INTERVAL_S later) picks it up normally. */
    static bool prev_charging;
    static bool first_poll = true;

    if (first_poll) {
        first_poll    = false;
        prev_charging = chrg;
        charging_mode = chrg;
    } else if (chrg) {
        if (!prev_charging) {
            LOG_INF("Power connected");
        }
        prev_charging = true;
        charging_mode = true;
        level_show_for(0);
    } else if (prev_charging) {
        prev_charging = false;
        LOG_INF("Power disconnected");
        if (charging_mode) {
            charging_mode = false;
            level_dismiss();
        }
    }

    /* Edge-triggered: show it once on the way down, clear it once on the way
     * back up. Re-asserting every poll is what pinned the row on and kept the
     * display waking itself. */
    if (!low_shown && mv < LOW_BATTERY_MV) {
        low_shown = true;
        LOG_WRN("Battery low: %u mV", mv);
        if (!level_showing) {
            show_low_battery_indicator();
        }
    } else if (low_shown && mv >= LOW_BATTERY_CLEAR_MV) {
        low_shown = false;
        LOG_INF("Battery recovered: %u mV", mv);
        if (!level_showing) {
            clear_low_battery_indicator();
        }
    }

    /* If the level screen is up it owns the notification layer, so the state
     * change is only recorded in low_shown. level_clear_work_fn() applies it
     * when the screen comes down — a poll landing in that 3 s window must not
     * paint over the readout. */
}

static void battery_timer_cb(struct k_timer *timer)
{
    ARG_UNUSED(timer);
    k_work_submit(&battery_work);
}

static K_TIMER_DEFINE(battery_timer, battery_timer_cb, NULL);

#if DT_NODE_EXISTS(CHG_NODE)
/* ISR context: I2C cannot run here, so just kick the poll. */
static void chg_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    ARG_UNUSED(port); ARG_UNUSED(cb); ARG_UNUSED(pins);
    k_work_submit(&battery_work);
}

static void chg_indicator_init(void)
{
    if (!gpio_is_ready_dt(&chg_pin)) {
        LOG_WRN("Charge indicator GPIO not ready — plug-in detection falls "
                "back to the %ds poll", POLL_INTERVAL_S);
        return;
    }

    int rc = gpio_pin_configure_dt(&chg_pin, GPIO_INPUT);

    rc |= gpio_pin_interrupt_configure_dt(&chg_pin, GPIO_INT_EDGE_BOTH);
    if (rc) {
        LOG_WRN("Charge indicator setup failed (%d) — falling back to poll", rc);
        return;
    }

    gpio_init_callback(&chg_cb, chg_isr, BIT(chg_pin.pin));
    gpio_add_callback(chg_pin.port, &chg_cb);

    LOG_INF("Charge indicator interrupt armed");
}
#endif

void battery_init(void)
{
    if (!device_is_ready(bq)) {
        LOG_ERR("BQ27441 not ready");
        return;
    }

    /* Read immediately on boot, then on a timer */
    k_work_submit(&battery_work);
    k_timer_start(&battery_timer,
                  K_SECONDS(POLL_INTERVAL_S),
                  K_SECONDS(POLL_INTERVAL_S));

#if DT_NODE_EXISTS(CHG_NODE)
    chg_indicator_init();
#endif

    LOG_INF("Battery monitor started (poll every %ds)", POLL_INTERVAL_S);
}

uint8_t  battery_percent(void)    { return (uint8_t)atomic_load(&g_percent); }
uint32_t battery_voltage_mv(void) { return (uint32_t)atomic_load(&g_volt_mv); }
bool     battery_charging(void)   { return atomic_load(&g_charging) != 0; }
bool     battery_is_low(void)     { return battery_voltage_mv() < LOW_BATTERY_MV; }
