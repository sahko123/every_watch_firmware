#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>

/* Grid dimensions */
#define LED_COLS 20
#define LED_ROWS  7

/* Compositor layer IDs — lower index = higher priority (drawn on top).
 * Sand sits above digits so it covers them; as particles clear a cell
 * the digit underneath is revealed. */
#define LED_LAYER_NOTIFICATION 0
#define LED_LAYER_SAND         1
#define LED_LAYER_DIGITS       2
#define LED_LAYER_BG           3
#define LED_LAYER_COUNT        4

struct led_rgb {
	uint8_t r, g, b;
};

/* Per-cell color, used by layers that don't have a layer_color override (e.g. sand).
 *
 * LOCKING: same contract as led_mask[] below — all writes must hold
 * led_mask_mutex. build_buffers() reads led_mask[], led_color[] and
 * led_layer_color[] together under one lock acquisition, so a writer that
 * only guards led_mask[] and not this array is still racing the compositor.
 * Use led_color_fill() below for a solid fill; it takes the lock internally
 * (safe to call whether or not the caller already holds led_mask_mutex —
 * Zephyr's k_mutex is safely re-entrant for the owning thread).
 *
 * OWNERSHIP: exactly one thing may animate this at a time, and the UI page is
 * what decides which. Two animators exist and they must never overlap —
 * sand.c's hue wave rewrites the whole field every 33 ms, and battery.c's
 * readout shimmer rewrites it every 60 ms. Nothing arbitrates between them at
 * this level; they stay apart because the battery page declares needs_sand =
 * false, which parks the sand thread, and time_display_deactivate() drops the
 * wave whenever the clock goes away. Adding a third animator, or giving the
 * battery page sand, breaks that without any warning louder than flicker. */
extern struct led_rgb led_color[LED_ROWS][LED_COLS];

/* Per-layer solid color override. When non-zero, all lit pixels in that layer
 * use this color instead of led_color[row][col]. Set to {0,0,0} to disable.
 * Same locking contract as led_color[] above. */
extern struct led_rgb led_layer_color[LED_LAYER_COUNT];

/* Mask layers: non-zero = pixel active at this cell.
 * Compositor walks layers highest-to-lowest priority, returns first active hit.
 *
 * LOCKING: all writes to led_mask[] from any context must hold led_mask_mutex.
 * Writers: render() in sand.c, render_time() in time_display.c, display_off()
 * in display.c, show_notification() in ble.c, show_low_battery_indicator() in
 * battery.c. build_buffers() acquires the mutex for its reads — of this array
 * and of led_color[]/led_layer_color[] together, see above. */
extern uint8_t led_mask[LED_LAYER_COUNT][LED_ROWS][LED_COLS];

/* Mutex protecting all led_mask[] accesses (writes from workqueue/BT thread,
 * reads from sand thread via build_buffers()). */
extern struct k_mutex led_mask_mutex;

/* Mutex protecting the full led_commit() sequence (DMA transfers + bitbang).
 * display_off() acquires this before suspending the sand thread to ensure the
 * thread cannot be suspended mid-DMA leaving semaphores stuck at zero. */
extern struct k_mutex led_commit_mutex;

/* Ambient level 0-255 set by the light sensor. Scales within
 * led_max_brightness rather than overriding it, so 255 means "as bright as
 * this watch ever gets", not "full power".
 * Only updated while the display is off, to avoid the LEDs feeding back into
 * the sensor and driving themselves brighter. */
extern uint8_t led_brightness;

/*
 * Gamma exponent applied to led_brightness, 1-3. Default 2.
 *
 * Everything downstream of here is linear in PWM duty, but the eye is not:
 * perceived brightness goes roughly as duty^(1/2.2). Without this, the ambient
 * curve was linear in duty and so far brighter than its numbers suggested —
 * led_brightness 24, the pitch-dark floor, is 9.4% duty and reads as about a
 * THIRD of full brightness; 60 (indoor evening) is 23.5% duty and reads as
 * about half. That is why the watch was uncomfortable in a dark room while the
 * curve looked like it was already down at a tenth.
 *
 * Applied only to the ambient factor, and deliberately not to the current
 * budget: that one is a physical limit in milliamps, and bending it through a
 * perceptual curve would mean it no longer limits what it claims to.
 *
 * Because x^g is anchored at both ends (0 stays 0, 255 stays 255), raising this
 * costs nothing at the top — direct sunlight is still full power — and only
 * pulls the dark end down. That asymmetry is the whole point; a flat multiplier
 * would have dimmed daylight too, where the watch already struggles.
 *
 * Integer exponents only, so this is two multiplies rather than a powf() and a
 * 256-entry table. 2.0 vs the textbook 2.2 is a difference of a few percent in
 * the middle of the range and nothing at either end — well inside what these
 * (custom, unmeasured) LEDs and diffuser are worth tuning to. 1 restores the
 * old linear behaviour for comparison; 3 is aggressive, and at 3 the useful
 * ambient range compresses into roughly 40-255 because everything below that
 * rounds to the same one-LSB floor.
 *
 * Note this changes what led_brightness *means*, so the lux breakpoints in
 * light.c's lux_to_brightness() are calibrated against a particular value of
 * this. Change one, re-check the other.
 */
extern uint8_t led_gamma;

/*
 * Whether the light sensor owns led_brightness. Default true.
 *
 * Set false and light.c keeps sampling and logging but stops writing, so a
 * value forced with `led ambient` stays put. Purely a bench-tuning aid: the
 * sensor only writes while the display is off, so a forced level survives the
 * frame you are looking at and is then silently replaced the moment the watch
 * blanks — which makes comparing two levels by eye almost impossible, since the
 * second look is never at the value you set.
 */
extern bool led_ambient_auto;

/* The brightest any single pixel is ever driven, 0-255.
 *
 * Defaults to 255 — i.e. off — and you probably want to leave it there. It sat
 * at 32 (~12%) for a long time and that turned out to be the main thing eating
 * colour resolution at low brightness: it crushed every frame to a twelfth of
 * range *before* the current limiter looked at it, so on sparse content (a
 * clock face, a few sand grains) the limiter never engaged and the range was
 * given up for nothing. Sand amber (255,160,20) came out as (31,20,2) at full
 * ambient — two levels of blue left, and the hue visibly drifted orange as it
 * dimmed further.
 *
 * The other two knobs already cover both real constraints: led_brightness is
 * the perceptual one (how bright should this be in this room) and
 * led_current_budget is the physical one (how much may we draw). This is a
 * third limiter overlapping both. Reach for it only if the watch is still too
 * bright after led_brightness has bottomed out. */
extern uint8_t led_max_brightness;

/*
 * Transition fade, 0-255. 255 is normal; 0 is black.
 *
 * Folded into the same single scale factor as everything else, so a fade costs
 * no extra quantisation — it just moves the one multiply that was already
 * happening. Deliberately separate from led_brightness, which the light sensor
 * owns and rewrites on its own schedule: an animation borrowing that variable
 * would fight the next ambient reading, and whichever landed last would win.
 *
 * Whoever ramps this is responsible for putting it back to 255.
 */
extern uint8_t led_fade;

/* Total-current limit, expressed as a budget on the sum of every channel value
 * in the composited frame. When a frame exceeds it, all pixels are scaled down
 * together — so a handful of LEDs can be bright, but lighting the whole display
 * dims it automatically instead of drawing amps.
 *
 * Converting budget to current:
 *   140 LEDs at full white = 140 × 3 × 255 = 107,100 sum, and draws ~8.4 A.
 *   So 1 unit of sum ≈ 8400 / 107100 ≈ 0.078 mA.
 *
 *   budget 5128  ≈ 400 mA   (default)
 *   budget 1900  ≈ 150 mA   (the previous default — conservative, and dim in
 *                            daylight once the per-pixel ceiling was removed)
 *   budget 45000 ≈ 3.5 A    (the original — never engaged, so a full-screen
 *                            frame at the old 78% ceiling pulled about 2.2 A)
 *
 * Since build_buffers() multiplies ambient by the budget rather than MIN-ing
 * them, this value sets what full brightness *means* — so changing it scales
 * every ambient level, not just the bright end. Frame draw works out at
 * roughly budget * (led_brightness / 255), which at 400 mA is ~38 mA in the
 * dark and ~400 mA in direct sun. Raise this and the darkness floor in
 * light.c gets brighter in proportion; the two are tuned against each other.
 *
 * Set to 0 to disable limiting entirely. Do not, unless the watch is on a
 * bench supply.
 *
 * Note this governs the LEDs' *driven* current only. WS2812B also draw roughly
 * 0.5-1 mA each just being powered, so 140 of them idle at somewhere near
 * 100 mA regardless of what is displayed — that is a hardware property and no
 * firmware setting affects it. */
extern uint32_t led_current_budget;

/* Colour wheel: 0-255 walks once around the hue circle. Integer only — no
 * floats, no division — so it is cheap enough to call per pixel per frame. */
struct led_rgb led_color_wheel(uint8_t pos);

/* Shared 3x5 digit font. Each row is a bitmask: bit2 = left column, bit0 =
 * right. Five rows tall, so two spare rows on a 7-row display. */
extern const uint8_t led_font_3x5[10][5];

/* Stamp one digit into a 7x20 bitmap with its top-left corner at (row, col).
 * Pixels falling outside the grid are clipped, so callers may centre text by
 * arithmetic without bounds-checking first. Writes 1s only — never clears. */
void led_stamp_digit(uint8_t out[LED_ROWS][LED_COLS], int digit, int row, int col);

/* Same, for a percent sign — occupies the same 3x5 cell as a digit. */
void led_stamp_percent(uint8_t out[LED_ROWS][LED_COLS], int row, int col);

/* Initialise the LED matrix. Call once before first led_commit(). */
void led_matrix_init(void);

/* Composite all layers + color layer → SPI DMA buffers → parallel DMA to strips.
 * Blocks until all transfers complete (~2–3 ms). Safe to call from any thread.
 * Acquires led_commit_mutex internally. */
void led_commit(void);

/* Convenience helpers — callers must hold led_mask_mutex around calls that
 * modify led_mask[] if concurrent access from other threads is possible. */
static inline void led_mask_clear(int layer)
{
	memset(led_mask[layer], 0, sizeof(led_mask[layer]));
}

static inline void led_mask_clear_all(void)
{
	memset(led_mask, 0, sizeof(led_mask));
}

static inline void led_color_fill(uint8_t r, uint8_t g, uint8_t b)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	for (int row = 0; row < LED_ROWS; row++)
		for (int col = 0; col < LED_COLS; col++)
			led_color[row][col] = (struct led_rgb){r, g, b};
	k_mutex_unlock(&led_mask_mutex);
}

/*
 * The resting value of led_color[] — warm amber.
 *
 * Anything that finishes animating the field puts it back here rather than
 * leaving its own colours behind, so a layer that reads led_color[] without
 * setting it first gets something visible instead of whatever the last screen
 * happened to be using. Black would be the other obvious default and is worse:
 * it makes such a layer silently invisible rather than obviously wrong.
 *
 * This existed as a bare (255, 160, 20) at three separate call sites, which is
 * exactly the kind of constant that drifts apart one edit at a time.
 */
static inline void led_color_reset(void)
{
	led_color_fill(255, 160, 20);
}
