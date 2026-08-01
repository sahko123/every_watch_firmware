#pragma once

#include <stdint.h>
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

/* Per-cell color, used by layers that don't have a layer_color override (e.g. sand). */
extern struct led_rgb led_color[LED_ROWS][LED_COLS];

/* Per-layer solid color override. When non-zero, all lit pixels in that layer
 * use this color instead of led_color[row][col]. Set to {0,0,0} to disable. */
extern struct led_rgb led_layer_color[LED_LAYER_COUNT];

/* Mask layers: non-zero = pixel active at this cell.
 * Compositor walks layers highest-to-lowest priority, returns first active hit.
 *
 * LOCKING: all writes to led_mask[] from any context must hold led_mask_mutex.
 * Writers: render() in sand.c, render_time() in time_display.c, display_off()
 * in display.c, show_notification() in ble.c, show_low_battery_indicator() in
 * battery.c. build_buffers() acquires the mutex for its reads. */
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

/* The brightest any single pixel is ever driven, 0-255. Default 64 (~25%).
 * This is a perceptual ceiling; led_current_budget below is the one that
 * protects the battery. */
extern uint8_t led_max_brightness;

/* Total-current limit, expressed as a budget on the sum of every channel value
 * in the composited frame. When a frame exceeds it, all pixels are scaled down
 * together — so a handful of LEDs can be bright, but lighting the whole display
 * dims it automatically instead of drawing amps.
 *
 * Converting budget to current:
 *   140 LEDs at full white = 140 × 3 × 255 = 107,100 sum, and draws ~8.4 A.
 *   So 1 unit of sum ≈ 8400 / 107100 ≈ 0.078 mA.
 *
 *   budget 1900  ≈ 150 mA   (default: safe on USB, ~0.4C on a 400 mAh cell)
 *   budget 45000 ≈ 3.5 A    (the old default — never engaged, so a full-screen
 *                            frame at the old 78% ceiling pulled about 2.2 A)
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
	for (int row = 0; row < LED_ROWS; row++)
		for (int col = 0; col < LED_COLS; col++)
			led_color[row][col] = (struct led_rgb){r, g, b};
}
