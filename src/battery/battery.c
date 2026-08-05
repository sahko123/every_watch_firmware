#include "battery.h"
#include "led_matrix/led_matrix.h"
#include "display/display.h"
#include "time_display/time_display.h"
#include "sand/sand.h"
#include "ui/ui.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <nrfx_power.h>

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
 * Percentage points a poll-to-poll change must exceed to be taken whole
 * rather than smoothed — see the filter in battery_work_fn(). Sized well
 * above the noise floor observed poll to poll and well below a plausible
 * real event (unplugging, a fresh boot reading).
 */
#define PCT_SNAP_THRESHOLD      5

/*
 * 5 s rather than 60. The poll is what detects power being connected, and a
 * readout that can take a minute to appear after plugging in reads as broken.
 * The SGM41524 charge indicator below normally beats it to the punch; this is
 * the backstop for if that line is not behaving.
 */
#define POLL_INTERVAL_S        5

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

#define LEVEL_ROW             1  /* 5-row font on a 7-row grid, 1px margin */
#define LEVEL_LOW_PCT        20  /* below this the readout goes red */

#define WAVE_STEP_MS         60  /* animation tick */
#define WAVE_COL_SHIFT       12  /* phase offset per column — sets wavelength */

/*
 * Colour encodes power state; motion encodes actual charge current.
 *
 * Discharging is a flat blue, being on power (cable attached, whether or not
 * current is presently flowing) is green, and a hue wave travels across the
 * text only while current is genuinely moving. Keeping those on separate
 * axes means a topped-off cell sitting on the charger still reads green
 * (it's on power) but stops shimmering (nothing is actually charging it
 * anymore) instead of looking indistinguishable from unplugged.
 */
static struct led_rgb level_base_color(uint8_t pct, bool on_power)
{
    if (pct < LEVEL_LOW_PCT) return (struct led_rgb){200,   0,  0}; /* red   */
    if (on_power)            return (struct led_rgb){  0, 180, 40}; /* green */
    return (struct led_rgb){0, 90, 220};                            /* blue  */
}

/* Triangle wave, 0-254, so the shimmer runs smoothly both ways without a
 * sine table or any floating point. */
static uint8_t tri(uint8_t x)
{
    return (x < 128) ? (uint8_t)(x * 2) : (uint8_t)((255 - x) * 2);
}

static void level_dismiss(void);

static bool    level_showing;

/* True once a manually-opened battery page has been promoted to stay open
 * for as long as the watch is on power, rather than fading out on the
 * page's normal timeout — see the promotion logic in battery_work_fn().
 * Never set on its own initiative: the page is only ever opened by a
 * right-button hold, this just changes how long it stays once it is. Left
 * via ui_dismiss() in battery_work_fn() (unplugging) or battery_screen_
 * dismiss() (navigating away, a quick right-press, a reveal taking over) —
 * never drawn directly, so ui.c's page state always agrees with what's
 * actually on screen. */
static bool    charging_mode;

static bool    level_wave;      /* animate — latched at show time, true only
                                 * while current is genuinely flowing */
static bool    level_onp;       /* on-power colour band — latched separately
                                 * from level_wave, see level_base_color() */
static uint8_t level_pct;       /* latched, so the wave cannot change colour
                                 * band mid-animation if a poll lands */
static uint8_t wave_phase;

/* Repaint led_color[] for the current phase. The notification layer's
 * layer_color is left at {0,0,0} while the readout is up, so the compositor
 * takes each pixel's colour from led_color[] and we can vary it per column. */
static void level_paint(void)
{
    struct led_rgb base = level_base_color(level_pct, level_onp);

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

/* Take the readout down cleanly. Called whenever ui.c's page timeout fires,
 * a quick right-press dismisses it, something else navigates away, or
 * unplugging ends a promoted view — all need the identical teardown.
 * Dismiss *timing* is entirely ui.c's job now (see ui_cancel_timeout() and
 * battery_work_fn()); this function only ever tears down, never schedules
 * anything. */
static void level_dismiss(void)
{
    if (!level_showing) {
        return;
    }
    level_showing = false;
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

/* Called by time_display_reveal() before it takes over the whole screen.
 * Without this, a button press or wrist-tilt during the battery percentage
 * peek — or worse, during the persistent charging view — left level_showing/
 * charging_mode/wave_timer all still live while the reveal wiped the mask out
 * from under them: wave_timer kept firing every 60ms, repainting led_color[]
 * over whatever the reveal had just drawn, and time_display's `paused` flag
 * (only ever cleared by level_dismiss()'s time_display_resume() call) stayed
 * stuck true for the rest of the charging session, freezing the clock. Safe
 * to call unconditionally from workqueue context: every caller of
 * time_display_reveal() (button ISR -> reveal_work, wrist-tilt trigger, both
 * via ui_goto()) runs on the system workqueue, the same thread every other
 * battery.c state mutation runs on. */
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

static void level_show_for(void)
{
    uint8_t pct  = battery_percent();
    bool    chrg = battery_charging();
    bool    onp  = chrg || battery_cable_present();

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

    /* Latch all three, so a poll landing mid-animation cannot switch colour
     * band or start/stop the wave under the repaint. */
    level_pct  = pct;
    level_wave = chrg;
    level_onp  = onp;

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
    display_on();
    led_commit();

    if (level_wave) {
        k_timer_start(&wave_timer, K_MSEC(WAVE_STEP_MS), K_MSEC(WAVE_STEP_MS));
    } else {
        k_timer_stop(&wave_timer);
    }

    LOG_INF("Battery level: %u%% %umV %s",
            pct, battery_voltage_mv(),
            chrg ? "charging" : onp ? "on power" : "discharging");
}

/* Runs the actual draw on the system workqueue — the same context that owns
 * charging_mode/level_pct/level_wave/wave_phase everywhere else
 * (battery_work_fn, wave_work_fn). battery_show_level() is only ever called
 * from ui.c's battery_enter(), itself only reached via ui_goto(UI_PAGE_
 * BATTERY) — and every one of those call sites (ui_handle_button()'s
 * BTN_EV_R_HOLD, battery_work_fn()'s poll-driven entry) already checks that
 * the battery page isn't already current before going there, so by the time
 * this runs it is always a genuine fresh entry that wants a draw — including
 * the persistent on-power view's very first one, where charging_mode is
 * already true (battery_work_fn sets it just before calling ui_goto()). An
 * earlier version of this function skipped drawing whenever charging_mode
 * was already set, meant to stop a manual hold from re-timing an
 * already-showing view — which, now that ui.c's own current-page check
 * guards manual re-entry, only ever fired on that very first draw and
 * silently swallowed it. */
static void show_level_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    level_show_for();
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

    uint8_t  pct_raw = (uint8_t)soc.val1;
    uint32_t mv      = (uint32_t)(volt.val1 * 1000 + volt.val2 / 1000);

    /*
     * Smooth the readout the same way light.c smooths lux -> brightness.
     * SENSOR_CHAN_GAUGE_STATE_OF_CHARGE is already TI's own filtered
     * estimate (StateOfCharge(), not the unfiltered command — see the
     * driver, bq274xx.c), but it still steps a few points either way poll
     * to poll: Impedance Track recomputes against instantaneous voltage,
     * and does so against a 400 mAh design-capacity that is still a guess
     * the gauge hasn't learned through a full charge/discharge cycle (see
     * the devicetree comment on bq27441). Nothing here needs a fast
     * response — battery level changes over minutes, not seconds, unlike
     * brightness — so smooth everything except a jump large enough to be a
     * real event (unplugging, the first reading after boot) and take that
     * whole.
     */
    static uint8_t pct_filt;
    static bool    pct_filt_seeded;
    int pct_delta = (int)pct_raw - (int)pct_filt;

    if (!pct_filt_seeded) {
        pct_filt_seeded = true;
        pct_filt = pct_raw;
    } else if (pct_delta > PCT_SNAP_THRESHOLD || pct_delta < -PCT_SNAP_THRESHOLD) {
        pct_filt = pct_raw;
    } else {
        pct_filt = (uint8_t)((pct_filt * 3 + pct_raw) / 4);
    }

    uint8_t pct = pct_filt;

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

    LOG_INF("Battery: %u%% (raw %u%%) %umV %s",
            pct, pct_raw, mv, chrg ? "charging" : "discharging");

    log_gauge_detail(current);

    /*
     * "On power" is current flowing OR a cable physically attached, not
     * current alone. Avg current tapers to ~0 once the cell tops off, which
     * on its own reads identically to "unplugged" — that used to dismiss the
     * persistent view (and hand the display back to whatever ui.c considered
     * idle) while the watch was still sitting on the charger. VBUS doesn't
     * care about taper, so ORing it in keeps this correct through a full
     * charge cycle. See battery_cable_present().
     */
    bool on_power = chrg || battery_cable_present();

    /*
     * The battery page is never shown on plug-in by itself — only a manual
     * right-button hold opens it (see battery_show_level()). This used to
     * take the display over automatically once idle, which is exactly the
     * "can't tell if it's plugged in without checking, and can't get the
     * charging popup to leave you alone" complaint that changed it: the
     * watch should just sit there normally, on power or not, until asked.
     *
     * What plugging in still does is *promote* an already-open peek: if the
     * page happens to be up (because someone held the button) and the watch
     * turns out to be on power, this poll cancels its timeout so it stays
     * open instead of fading out mid-charge, and keeps refreshing it every
     * poll so the percentage climbs live. ui_cancel_timeout() rather than
     * ui_goto() again — the page is already showing the right thing, no
     * need to blank and redraw it.
     *
     * Dismissal is a quick press of the same button (ui.c's BTN_EV_R_SINGLE)
     * or unplugging — see the prev_power branch below.
     *
     * first_poll suppresses acting at boot: battery_init() runs before
     * main() blanks the display and drops to idle, so anything drawn here
     * would be wiped a moment later and look like a glitch. */
    static bool prev_power;
    static bool first_poll = true;

    if (first_poll) {
        first_poll = false;
        prev_power = on_power;
    } else if (on_power) {
        if (!prev_power) {
            LOG_INF("Power connected");
        }
        prev_power = true;

        if (charging_mode) {
            /* Already promoted — refresh in place. */
            level_show_for();
        } else if (ui_current() == UI_PAGE_BATTERY) {
            /* Open as a plain peek and now on power: promote it. */
            charging_mode = true;
            ui_cancel_timeout();
            level_show_for();
        }
    } else if (prev_power) {
        prev_power = false;
        LOG_INF("Power disconnected");
        if (charging_mode) {
            charging_mode = false;
            ui_dismiss();
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
     * change is only recorded in low_shown. level_dismiss() applies it when
     * the screen comes down — a poll landing while it's up must not paint
     * over the readout. */
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

    /* Checked, because this is the last step and the one that actually
     * connects the handler. It was previously called bare, with the "armed"
     * message printed unconditionally underneath — so a failure here would
     * report success and leave an interrupt that could never fire. This
     * interrupt has in fact never been observed firing, which makes it worth
     * knowing rather than assuming. */
    rc = gpio_add_callback(chg_pin.port, &chg_cb);
    if (rc) {
        LOG_WRN("Charge indicator callback registration failed (%d) —"
                " falling back to poll", rc);
        return;
    }

    LOG_INF("Charge indicator interrupt armed");
}
#endif

/* --------------------------------------------------------------------------
 * VBUS detection — nRF52833 POWER peripheral
 *
 * A second, independent "cable attached" signal alongside the SGM41524
 * indicator above. Where that one is board wiring whose interrupt "has in
 * fact never been observed firing" (see chg_indicator_init()), this is the
 * SoC sensing its own D+/D- lines — no GPIO, no external part, nothing that
 * can be missing pull-ups or miswired. It answers a narrower question than
 * either the SGM41524 pin or the fuel gauge's current reading: not
 * "charging", just "a cable is physically there" — which is exactly what
 * avg current can't tell you once it tapers to ~0 near a full charge (see
 * the on_power comment in battery_work_fn()).
 *
 * nRF52833's USBDETECTED/USBREMOVED events ride the POWER peripheral, on
 * the same physical interrupt line the CLOCK peripheral uses (there is no
 * separate USBREGULATOR_IRQn on this SoC, unlike nRF5340). Zephyr's own
 * clock driver already connects that line unconditionally at boot
 * (clock_control_nrf.c, POWER_CLOCK IRQ), so registering a usbevt handler
 * here needs no IRQ wiring of our own — just nrfx_power_init() once and
 * nrfx_power_usbevt_init()/_enable() to hook in.
 * -------------------------------------------------------------------------- */

static atomic_int g_vbus_present = ATOMIC_INIT(0);

/* ISR context (shared POWER_CLOCK vector): no I2C here, same rule as
 * chg_isr() above — just latch the new state and kick a poll. */
static void vbus_evt_handler(nrfx_power_usb_evt_t event)
{
    switch (event) {
    case NRFX_POWER_USB_EVT_DETECTED:
        atomic_store(&g_vbus_present, 1);
        k_work_submit(&battery_work);
        break;
    case NRFX_POWER_USB_EVT_REMOVED:
        atomic_store(&g_vbus_present, 0);
        k_work_submit(&battery_work);
        break;
    case NRFX_POWER_USB_EVT_READY:
        /* Regulator ready to supply current — not new information here;
         * DETECTED already said a cable is present. */
        break;
    }
}

static void vbus_init(void)
{
    /*
     * Skip entirely on the USB-console build variant (bringup_usb.conf).
     * Zephyr's own nRF USB device driver (usb_dc_nrfx.c) registers itself as
     * nrfx_power's one and only usbevt handler at boot — nrfx_power supports
     * exactly one, so a second registration here would just race it, and
     * whichever loses stops seeing events. The normal/production image never
     * sets CONFIG_USB_DEVICE_STACK, so this runs unopposed there.
     */
    if (IS_ENABLED(CONFIG_USB_DEVICE_STACK)) {
        LOG_INF("VBUS detect skipped — USB device stack owns it on this build");
        return;
    }

    /* dcdcen left false (zero-initialised): this board has no inductors on
     * the DCC pins and runs off an external 3.3V LDO — see CLAUDE.md, "the
     * internal DC/DC is off deliberately". nrfx_power_init() also brings up
     * POF and sleep-event handling, none of which this app uses; the config
     * struct covers only what nrfx_power itself needs to start. */
    static const nrfx_power_config_t power_config = {0};

    /* NRFX_ERROR_ALREADY_INITIALIZED would mean something else already
     * called this — nothing else does on this build, but the check is free
     * and matches how Zephyr's own USB driver treats the same call. */
    (void)nrfx_power_init(&power_config);

    static const nrfx_power_usbevt_config_t usbevt_config = {
        .handler = vbus_evt_handler,
    };

    nrfx_power_usbevt_init(&usbevt_config);
    nrfx_power_usbevt_enable();

    /* Seed from the live register rather than waiting for the first edge —
     * if the watch boots already plugged in, there is no edge coming. */
    atomic_store(&g_vbus_present,
                 nrf_power_usbregstatus_vbusdet_get(NRF_POWER) ? 1 : 0);

    LOG_INF("VBUS detect armed (cable %s)",
            atomic_load(&g_vbus_present) ? "present" : "absent");
}

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

    vbus_init();

    LOG_INF("Battery monitor started (poll every %ds)", POLL_INTERVAL_S);
}

uint8_t  battery_percent(void)    { return (uint8_t)atomic_load(&g_percent); }
uint32_t battery_voltage_mv(void) { return (uint32_t)atomic_load(&g_volt_mv); }
bool     battery_charging(void)   { return atomic_load(&g_charging) != 0; }
bool     battery_cable_present(void) { return atomic_load(&g_vbus_present) != 0; }
bool     battery_is_low(void)     { return battery_voltage_mv() < LOW_BATTERY_MV; }
