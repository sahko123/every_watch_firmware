#pragma once

#include <stdint.h>
#include <string.h>

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
 * Compositor walks layers highest-to-lowest priority, returns first active hit. */
extern uint8_t led_mask[LED_LAYER_COUNT][LED_ROWS][LED_COLS];

/* Global brightness scaler 0-255 (default 255 = full).
 * Set by the light sensor to adapt to ambient light. */
extern uint8_t led_brightness;

/* Initialise SPI devices. Call once before first led_commit(). */
void led_matrix_init(void);

/* Composite all layers + color layer → SPI DMA buffers → parallel DMA to strips.
 * Blocks until all transfers complete (~2–3 ms). Safe to call from any thread. */
void led_commit(void);

/* Convenience helpers */
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
