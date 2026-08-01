#include "led_matrix.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>
#include <nrfx_pwm.h>
#include <string.h>

LOG_MODULE_REGISTER(led_matrix, LOG_LEVEL_INF);

/*
 * WS2812B via PWM + EasyDMA
 * ------------------------
 * WS2812B is self-clocked: the value of each bit is encoded in the width of a
 * high pulse within a fixed 1.25 us period. There is no clock line, so the job
 * is purely to generate an accurately timed waveform on one GPIO per data line.
 *
 * The nRF52833 PWM peripheral does exactly that. Each instance plays a sequence
 * of duty-cycle values out of RAM by DMA, one value per period, with no CPU
 * involvement. There are four instances (PWM0-3) and four data lines, so every
 * line gets its own and all four transmit simultaneously.
 *
 * Timing, base clock 16 MHz (62.5 ns/tick), TOP = 20 ticks = 1.25 us period:
 *
 *   bit 0 :  6 ticks high = 375.0 ns   (datasheet T0H 400 ns +/-150 -> 250-550)
 *   bit 1 : 13 ticks high = 812.5 ns   (datasheet T1H 800 ns +/-150 -> 650-950)
 *
 * Both sit comfortably mid-range. The previous SPI approach could only produce
 * T1H = 1000 ns, outside the 950 ns maximum, which genuine parts tolerate and
 * clones frequently do not.
 *
 * In each 16-bit sequence value, bit 15 selects polarity and bits 14-0 are the
 * compare value. With bit 15 set the output starts high and falls at compare,
 * so the value is simply the high time — which is what WS2812B encodes.
 *
 * Why not SPI: driving WS2812B from SPIM needs SCK assigned to a physical pin
 * because the shift register is clocked from it, and this board has no pin to
 * spare for a signal that goes nowhere. On hardware, SPIM1 and SPIM2 returned
 * -ETIMEDOUT on every transfer with SCK unassigned. PWM needs no such stub, and
 * it also removes the interrupt-locked bitbang that row 6 previously required.
 */

#define PWM_TOP        20              /* 20 ticks @ 16 MHz = 1.25 us */
#define PWM_BIT_0      (0x8000u | 6)   /* 375.0 ns high */
#define PWM_BIT_1      (0x8000u | 13)  /* 812.5 ns high */
#define PWM_LINE_LOW   (0x8000u | 0)   /* compare 0 -> low for the whole period */

/* WS2812B latches a frame after the data line is held low for >280 us.
 * 280 us / 1.25 us = 224 periods; 240 gives margin. This costs no memory:
 * end_delay repeats the final sequence value, which is why every sequence is
 * terminated with one PWM_LINE_LOW word. */
#define LATCH_PERIODS  240

#define BITS_PER_LED   24              /* GRB, 8 bits each */

#define LEDS_L1        40  /* rows 0-1, P0.29 */
#define LEDS_L2        40  /* rows 2-3, P0.28 */
#define LEDS_L3        40  /* rows 4-5, P0.02 */
#define LEDS_L4        20  /* row 6,    P0.03 */

/* Upper bound on how long a frame may take before we give up on it. A full
 * line is 40 x 24 x 1.25 us = 1.2 ms plus a 300 us latch, so 50 ms only trips
 * on something genuinely stuck. */
#define COMMIT_TIMEOUT_MS 50

/* Sequence buffers. One 16-bit duty value per WS2812B bit, plus one trailing
 * low word for the latch. EasyDMA requires these in RAM. */
static uint16_t seq_l1[LEDS_L1 * BITS_PER_LED + 1];
static uint16_t seq_l2[LEDS_L2 * BITS_PER_LED + 1];
static uint16_t seq_l3[LEDS_L3 * BITS_PER_LED + 1];
static uint16_t seq_l4[LEDS_L4 * BITS_PER_LED + 1];

static nrfx_pwm_t pwm[4] = {
	NRFX_PWM_INSTANCE(0),
	NRFX_PWM_INSTANCE(1),
	NRFX_PWM_INSTANCE(2),
	NRFX_PWM_INSTANCE(3),
};

static nrf_pwm_sequence_t pwm_seq[4];
static bool pwm_ready;

/* Protects the full led_commit() sequence.
 * display_off() acquires this before suspending the sand thread so it cannot
 * suspend mid-transfer. */
K_MUTEX_DEFINE(led_commit_mutex);

/* Protects all writes to led_mask[] from concurrent contexts
 * (time_display on workqueue, BLE on BT thread, display_off on workqueue)
 * against reads in build_buffers() on the sand thread. */
K_MUTEX_DEFINE(led_mask_mutex);

/* Public state */
struct led_rgb led_color[LED_ROWS][LED_COLS];
struct led_rgb led_layer_color[LED_LAYER_COUNT]; /* zero = use led_color per cell */
uint8_t        led_mask[LED_LAYER_COUNT][LED_ROWS][LED_COLS];
uint8_t        led_brightness     = 255;  /* ambient scaling 0-255; set by light sensor */
uint8_t        led_max_brightness = 32;   /* per-pixel ceiling, ~12% */
uint32_t       led_current_budget = 1900; /* ~150 mA total — see the maths below */

/* --------------------------------------------------------------------------
 * Shared 3x5 digit font
 *
 * Lives here rather than in the clock so anything that needs to put a number on
 * screen uses the same glyphs — the clock and the battery readout at least.
 * -------------------------------------------------------------------------- */

const uint8_t led_font_3x5[10][5] = {
	{0b111, 0b101, 0b101, 0b101, 0b111}, /* 0 */
	{0b010, 0b110, 0b010, 0b010, 0b111}, /* 1 */
	{0b111, 0b001, 0b111, 0b100, 0b111}, /* 2 */
	{0b111, 0b001, 0b111, 0b001, 0b111}, /* 3 */
	{0b101, 0b101, 0b111, 0b001, 0b001}, /* 4 */
	{0b111, 0b100, 0b111, 0b001, 0b111}, /* 5 */
	{0b111, 0b100, 0b111, 0b101, 0b111}, /* 6 */
	{0b111, 0b001, 0b001, 0b001, 0b001}, /* 7 */
	{0b111, 0b101, 0b111, 0b101, 0b111}, /* 8 */
	{0b111, 0b101, 0b111, 0b001, 0b111}, /* 9 */
};

/* Percent sign, same 3x5 cell as the digits:
 *
 *   #.#
 *   ..#
 *   .#.
 *   #..
 *   #.#
 */
static const uint8_t glyph_percent[5] = {0b101, 0b001, 0b010, 0b100, 0b101};

static void stamp_bits(uint8_t out[LED_ROWS][LED_COLS], const uint8_t bits[5],
		       int row, int col)
{
	for (int r = 0; r < 5; r++) {
		for (int c = 0; c < 3; c++) {
			int rr = row + r;
			int cc = col + c;

			/* Clipped rather than asserted: callers centre text by
			 * arithmetic, and a glyph running off the edge should lose a
			 * column, not corrupt the frame buffer. */
			if ((bits[r] & (0x4 >> c)) &&
			    rr >= 0 && rr < LED_ROWS && cc >= 0 && cc < LED_COLS) {
				out[rr][cc] = 1;
			}
		}
	}
}

void led_stamp_percent(uint8_t out[LED_ROWS][LED_COLS], int row, int col)
{
	stamp_bits(out, glyph_percent, row, col);
}

void led_stamp_digit(uint8_t out[LED_ROWS][LED_COLS], int digit, int row, int col)
{
	if (digit < 0 || digit > 9) {
		return;
	}

	stamp_bits(out, led_font_3x5[digit], row, col);
}

/* --------------------------------------------------------------------------
 * WS2812B encoding
 * -------------------------------------------------------------------------- */

struct led_rgb led_color_wheel(uint8_t pos)
{
	pos = 255 - pos;

	if (pos < 85) {
		return (struct led_rgb){255 - pos * 3, 0, pos * 3};
	}
	if (pos < 170) {
		pos -= 85;
		return (struct led_rgb){0, pos * 3, 255 - pos * 3};
	}
	pos -= 170;
	return (struct led_rgb){pos * 3, 255 - pos * 3, 0};
}

/* Expand one pixel into 24 PWM duty values. Wire order is GRB, MSB first. */
static void encode_led(const struct led_rgb *c, uint16_t *out)
{
	uint32_t grb = ((uint32_t)c->g << 16) | ((uint32_t)c->r << 8) | c->b;

	for (int bit = BITS_PER_LED - 1; bit >= 0; bit--) {
		*out++ = (grb & BIT(bit)) ? PWM_BIT_1 : PWM_BIT_0;
	}
}

/* --------------------------------------------------------------------------
 * Logical → physical pixel mapping
 * --------------------------------------------------------------------------
 *
 * col: 0-19 left→right, row: 0-6 top→bottom.
 * Returns the strip index (0-3) and the pixel offset within that strip.
 *
 * Wiring is snake: even rows run L→R (index 0-19),
 * odd rows run R→L (index 20-39 within the same strip).
 *
 *   Strip 0 (PWM0, P0.29): row 0 → pixels  0-19  (L→R)
 *                          row 1 → pixels 20-39  (R→L, col 0 → pixel 39)
 *   Strip 1 (PWM1, P0.28): row 2 → pixels  0-19
 *                          row 3 → pixels 20-39
 *   Strip 2 (PWM2, P0.02): row 4 → pixels  0-19
 *                          row 5 → pixels 20-39
 *   Strip 3 (PWM3, P0.03): row 6 → pixels  0-19  (L→R, single row)
 */
static void pixel_to_physical(int col, int row, int *strip, int *pixel)
{
	if (row <= 5) {
		*strip = row / 2;
		*pixel = (row % 2 == 0) ? col : (39 - col);
	} else {
		*strip = 3;
		*pixel = col;
	}
}

/* --------------------------------------------------------------------------
 * Compositor
 * -------------------------------------------------------------------------- */

static struct led_rgb composite(int col, int row)
{
	for (int layer = 0; layer < LED_LAYER_COUNT; layer++) {
		if (!led_mask[layer][row][col]) {
			continue;
		}
		struct led_rgb lc = led_layer_color[layer];

		return (lc.r || lc.g || lc.b) ? lc : led_color[row][col];
	}
	return (struct led_rgb){0, 0, 0};
}

/* Build all four PWM sequences from the current mask + color state.
 *
 * Two-pass approach:
 *   Pass 1 — composite + clamp to effective brightness → accumulate channel sum
 *   Pass 2 — apply current-limit scale factor → encode into the sequences
 *
 * Effective brightness = led_brightness * led_max_brightness / 255.
 * Current scale        = MIN(1.0, led_current_budget / total_sum).
 *
 * Caller (led_commit) holds led_commit_mutex; this function additionally
 * acquires led_mask_mutex for the duration of its led_mask[] reads.
 */
static void build_buffers(void)
{
	/*
	 * Ambient level scales *within* the ceiling rather than being MIN'd
	 * against it. With MIN, every ambient value above led_max_brightness
	 * collapsed to the same output, so the light sensor did nothing at all
	 * once the room was brighter than a dim office — the display sat at the
	 * ceiling almost all the time. Multiplying keeps the full range usable:
	 * led_max_brightness is the brightest the watch ever gets, and
	 * led_brightness picks a point below it.
	 */
	uint8_t br = (uint8_t)(((uint16_t)led_brightness * led_max_brightness) / 255);

	k_mutex_lock(&led_mask_mutex, K_FOREVER);

	/* Pass 1: composite + brightness → stash + accumulate sum */
	static struct led_rgb composed[LED_ROWS][LED_COLS];
	uint32_t total = 0;

	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			struct led_rgb c = composite(col, row);

			if (br < 255) {
				c.r = (uint8_t)(((uint16_t)c.r * br) >> 8);
				c.g = (uint8_t)(((uint16_t)c.g * br) >> 8);
				c.b = (uint8_t)(((uint16_t)c.b * br) >> 8);
			}

			total += c.r + c.g + c.b;
			composed[row][col] = c;
		}
	}

	k_mutex_unlock(&led_mask_mutex);

	/* Current-limit scale in Q8 fixed-point (256 = 1.0, no reduction) */
	uint32_t scale = 256;
	uint32_t budget = led_current_budget;

	if (budget > 0 && total > budget) {
		scale = (budget << 8) / total;
	}

	/* Pass 2: apply current scale → encode → PWM sequences */
	uint16_t *strips[4] = {seq_l1, seq_l2, seq_l3, seq_l4};

	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			struct led_rgb c = composed[row][col];

			if (scale < 256) {
				c.r = (uint8_t)(((uint16_t)c.r * scale) >> 8);
				c.g = (uint8_t)(((uint16_t)c.g * scale) >> 8);
				c.b = (uint8_t)(((uint16_t)c.b * scale) >> 8);
			}

			int strip, pixel;

			pixel_to_physical(col, row, &strip, &pixel);
			encode_led(&c, strips[strip] + pixel * BITS_PER_LED);
		}
	}
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void led_matrix_init(void)
{
	static const uint32_t pins[4] = {
		NRF_GPIO_PIN_MAP(0, 29),   /* line 1, rows 0-1 */
		NRF_GPIO_PIN_MAP(0, 28),   /* line 2, rows 2-3 */
		NRF_GPIO_PIN_MAP(0, 2),    /* line 3, rows 4-5 */
		NRF_GPIO_PIN_MAP(0, 3),    /* line 4, row 6    */
	};
	static uint16_t *const seqs[4] = {seq_l1, seq_l2, seq_l3, seq_l4};
	static const uint16_t lens[4] = {
		ARRAY_SIZE(seq_l1), ARRAY_SIZE(seq_l2),
		ARRAY_SIZE(seq_l3), ARRAY_SIZE(seq_l4),
	};

	for (int i = 0; i < 4; i++) {
		nrfx_pwm_config_t cfg = NRFX_PWM_DEFAULT_CONFIG(
			pins[i],
			NRF_PWM_PIN_NOT_CONNECTED,
			NRF_PWM_PIN_NOT_CONNECTED,
			NRF_PWM_PIN_NOT_CONNECTED);

		cfg.base_clock = NRF_PWM_CLK_16MHz;
		cfg.count_mode = NRF_PWM_MODE_UP;
		cfg.top_value  = PWM_TOP;
		cfg.load_mode  = NRF_PWM_LOAD_COMMON;
		cfg.step_mode  = NRF_PWM_STEP_AUTO;

		/* NULL handler: playback is polled rather than interrupt-driven, so
		 * no PWM IRQ has to be routed through nrfx_glue and no ISR runs
		 * during a frame. */
		nrfx_err_t err = nrfx_pwm_init(&pwm[i], &cfg, NULL, NULL);

		if (err != NRFX_SUCCESS) {
			LOG_ERR("PWM%d init failed: 0x%08x", i, (unsigned int)err);
			return;
		}

		/* Terminate with a low period; end_delay then holds the line low
		 * long enough for the WS2812B latch without extra memory. */
		seqs[i][lens[i] - 1] = PWM_LINE_LOW;

		pwm_seq[i].values.p_common = seqs[i];
		pwm_seq[i].length          = lens[i];
		pwm_seq[i].repeats         = 0;
		pwm_seq[i].end_delay       = LATCH_PERIODS;
	}

	memset(led_color, 0, sizeof(led_color));
	memset(led_mask,  0, sizeof(led_mask));
	pwm_ready = true;

	LOG_INF("LED matrix ready (4x PWM + EasyDMA, all lines parallel)");
}

void led_commit(void)
{
	if (!pwm_ready) {
		return;
	}

	k_mutex_lock(&led_commit_mutex, K_FOREVER);

	build_buffers();

	/* Start all four. They run concurrently in hardware, so a frame costs
	 * the length of one line (~1.2 ms) rather than the sum of all four. */
	for (int i = 0; i < 4; i++) {
		(void)nrfx_pwm_simple_playback(&pwm[i], &pwm_seq[i], 1,
					       NRFX_PWM_FLAG_STOP);
	}

	/* Wait for playback to finish before releasing the buffers to the next
	 * frame. Bounded: a stuck instance costs one frame, not the display. */
	int64_t deadline = k_uptime_get() + COMMIT_TIMEOUT_MS;

	for (int i = 0; i < 4; i++) {
		while (!nrfx_pwm_stopped_check(&pwm[i])) {
			if (k_uptime_get() > deadline) {
				LOG_ERR("PWM%d did not stop within %d ms",
					i, COMMIT_TIMEOUT_MS);
				break;
			}
			k_msleep(1);
		}
	}

	k_mutex_unlock(&led_commit_mutex);
}

/* --------------------------------------------------------------------------
 * Shell commands — live brightness tuning
 * --------------------------------------------------------------------------
 *
 * Brightness is a thing you judge by looking at it, and reflashing this board
 * means reconnecting a debug probe. These let it be dialled in over RTT while
 * the watch is running, then written back into the defaults above once the
 * numbers are right.
 *
 * Note `led ambient` only sticks while the display is on: the light sensor
 * rewrites led_brightness every 2 s whenever the display is off.
 * -------------------------------------------------------------------------- */

#ifdef CONFIG_SHELL

#include <zephyr/shell/shell.h>
#include <stdlib.h>

/* Sum-of-channels to milliamps: 107,100 sum ≈ 8.4 A, so ~0.078 mA per unit. */
static unsigned int budget_to_ma(uint32_t budget)
{
	return (unsigned int)((budget * 78u) / 1000u);
}

static int cmd_led_max(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		led_max_brightness = (uint8_t)strtoul(argv[1], NULL, 0);
		led_commit();
	}
	shell_print(sh, "max_brightness = %u / 255  (%u%%)",
		    led_max_brightness, (led_max_brightness * 100u) / 255u);
	return 0;
}

static int cmd_led_budget(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		led_current_budget = (uint32_t)strtoul(argv[1], NULL, 0);
		led_commit();
	}
	shell_print(sh, "current_budget = %u  (~%u mA driven)",
		    led_current_budget, budget_to_ma(led_current_budget));
	return 0;
}

static int cmd_led_ambient(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 2) {
		led_brightness = (uint8_t)strtoul(argv[1], NULL, 0);
		led_commit();
	}
	shell_print(sh, "ambient = %u / 255  (overwritten by the light sensor "
			"while the display is off)", led_brightness);
	return 0;
}

static int cmd_led_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);

	uint8_t eff = (uint8_t)(((uint16_t)led_brightness * led_max_brightness) / 255);

	shell_print(sh, "ambient        %u / 255", led_brightness);
	shell_print(sh, "max_brightness %u / 255", led_max_brightness);
	shell_print(sh, "effective      %u / 255  (%u%%)", eff, (eff * 100u) / 255u);
	shell_print(sh, "current_budget %u  (~%u mA driven)",
		    led_current_budget, budget_to_ma(led_current_budget));
	shell_print(sh, "note: 140 WS2812B also draw ~100 mA just being powered");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(led_sub,
	SHELL_CMD_ARG(show,    NULL, "Print current brightness settings",   cmd_led_show,    1, 0),
	SHELL_CMD_ARG(max,     NULL, "Per-pixel ceiling, 0-255",            cmd_led_max,     1, 1),
	SHELL_CMD_ARG(budget,  NULL, "Total current budget (sum units)",    cmd_led_budget,  1, 1),
	SHELL_CMD_ARG(ambient, NULL, "Ambient level, 0-255",                cmd_led_ambient, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(led, &led_sub, "LED brightness tuning", NULL);

#endif /* CONFIG_SHELL */
