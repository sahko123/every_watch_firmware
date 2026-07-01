#pragma once

#include <stdint.h>
#include <string.h>

/* Grid dimensions */
#define LED_COLS 20
#define LED_ROWS  7

/* Compositor layer IDs — lower index = higher priority (drawn on top) */
#define LED_LAYER_NOTIFICATION 0
#define LED_LAYER_DIGITS       1
#define LED_LAYER_SAND         2
#define LED_LAYER_BG           3
#define LED_LAYER_COUNT        4

struct led_rgb {
	uint8_t r, g, b;
};

/* Color layer: one RGB value per grid cell, shared across all mask layers.
 * Animated independently of the masks (e.g. sweeping hue shifts). */
extern struct led_rgb led_color[LED_ROWS][LED_COLS];

/* Mask layers: non-zero = particle present at this cell.
 * Compositor picks the highest-priority non-zero mask and reads led_color. */
extern uint8_t led_mask[LED_LAYER_COUNT][LED_ROWS][LED_COLS];

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
