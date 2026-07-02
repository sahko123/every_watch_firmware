#include "led_matrix.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>
#include <string.h>

LOG_MODULE_REGISTER(led_matrix, LOG_LEVEL_INF);

/*
 * WS2812B SPI encoding
 * --------------------
 * Each WS2812B bit → 3 SPI bits: T1=110, T0=100
 * At 2 MHz: 3 × 500 ns = 1.5 µs/bit  (spec: 1.25 µs ±600 ns)
 *   T1H = 2×500 = 1000 ns  (datasheet max 950 ns, but WS2812B tolerates ~1050 ns)
 *   T1L = 1×500 =  500 ns  (spec 300–600 ns ✓)
 *   T0H = 1×500 =  500 ns  (spec 250–550 ns, borderline — verify at bring-up)
 *   T0L = 2×500 = 1000 ns  (spec 700–1000 ns ✓)
 *
 * Note: nRF52833 SPIM supports only discrete rates (1/2/4/8 MHz etc.).
 * 2 MHz is the closest option to the ideal ~2.4 MHz; 4 MHz gives T1H = 500 ns
 * which is below the 550 ns minimum and is worse. Verify with oscilloscope.
 *
 * 24 bits/LED × 3 SPI bits = 72 SPI bits = 9 bytes/LED.
 * Wire order is GRB; swap R↔G at encode time.
 *
 * SPI lines (SPIM1, SPIM2, SPIM3) fire in parallel for rows 0-5.
 * Row 6 (P0.03) is sent sequentially on SPIM3 after rows 4-5 complete.
 * SPIM0 is unavailable: it shares hardware with TWIM0 (I2C0 @ 0x40003000).
 */

/* Row 6 bitbang on P0.03
 * ---------------------
 * The nRF52833 has no PIO block. The closest equivalent is PWM + EasyDMA:
 * preload a buffer of 16-bit duty-cycle values (one per WS2812B bit) and the
 * PWM peripheral generates the waveform on any GPIO pin without CPU involvement.
 * PWM0-3 are all free (separate hardware blocks from SPIM/TWIM).
 *
 * For now, row 6 uses a simple bit-bang. The CPU disables interrupts and
 * toggles P0.03 via direct OUTSET/OUTCLR register writes with nop delays.
 * At 64 MHz: 1 nop ≈ 15.6 ns. Interrupt-off window: 20 LEDs × 24 bits × 1250 ns ≈ 600 µs.
 *
 * TODO: migrate row 6 to PWM0 + EasyDMA for zero CPU cost and deterministic
 *       timing without the ~585 µs interrupt-disable window that can cause BLE
 *       connection supervision timeouts at short connection intervals.
 */
#define ROW6_PORT  0
#define ROW6_PIN   3   /* P0.03 */
#define ROW6_MASK  BIT(3)

/*
 * Nop delay macros. Calibrated for 64 MHz with flash cache warm (1 nop ≈ 15.6 ns).
 * Peripheral register writes (OUTSET/OUTCLR) take ~2 APB cycles ≈ 31 ns each.
 * Verify high/low pulse widths with an oscilloscope on first board bring-up.
 *
 * Target: T1H ≈ 580 ns, T0H ≈ 220 ns, bit period ≈ 1250 ns.
 *   T1: OUTSET(2) + nop×35(35) + OUTCLR(2) = 39 cycles H ≈ 609 ns
 *       OUTSET(2) + nop×35(35) + OUTCLR(2) = 39 cycles L ≈ 609 ns  → period ≈ 1218 ns
 *   T0: OUTSET(2) + nop×12(12) + OUTCLR(2) = 16 cycles H ≈ 250 ns
 *       OUTSET(2) + nop×58(58) + OUTCLR(2) = 62 cycles L ≈ 969 ns  → period ≈ 1219 ns
 */
#define _NOP()  __asm__ volatile ("nop" ::: "memory")
#define NOP2    _NOP(); _NOP()
#define NOP4    NOP2; NOP2
#define NOP8    NOP4; NOP4
#define NOP12   NOP8; NOP4
#define NOP16   NOP8; NOP8
#define NOP35   NOP16; NOP16; NOP2; _NOP()
#define NOP58   NOP35; NOP16; NOP4; NOP2; _NOP()

#define BYTES_PER_LED 9   /* 24 bits × 3 SPI bits / 8 = 9 bytes */
#define LEDS_01       40  /* rows 0-1: SPIM1 (P0.29) */
#define LEDS_23       40  /* rows 2-3: SPIM2 (P0.28) */
#define LEDS_45       40  /* rows 4-5: SPIM3 (P0.02) */
#define LEDS_6        20  /* row 6:    P0.03 bitbang  */

/* DMA buffers — placed in RAM, naturally aligned for EasyDMA */
static uint8_t buf01[LEDS_01 * BYTES_PER_LED];
static uint8_t buf23[LEDS_23 * BYTES_PER_LED];
static uint8_t buf45[LEDS_45 * BYTES_PER_LED];
static uint8_t buf6 [LEDS_6  * BYTES_PER_LED];

/* SPI devices from DTS */
static const struct device *spi1 = DEVICE_DT_GET(DT_NODELABEL(spi1));
static const struct device *spi2 = DEVICE_DT_GET(DT_NODELABEL(spi2));
static const struct device *spi3 = DEVICE_DT_GET(DT_NODELABEL(spi3));

static const struct spi_config spi_cfg = {
	.frequency = 2000000,
	.operation = SPI_WORD_SET(8) | SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB,
};

/* Semaphores signalled by the async SPI callbacks */
static struct k_sem done1, done2, done3;

/* Protects the full led_commit() sequence (DMA + bitbang).
 * display_off() acquires this before suspending the sand thread so it
 * cannot suspend mid-DMA leaving semaphores stuck at zero. */
K_MUTEX_DEFINE(led_commit_mutex);

/* Protects all writes to led_mask[] from concurrent contexts
 * (time_display on workqueue, BLE on BT thread, display_off on workqueue)
 * against reads in build_buffers() on the sand thread. */
K_MUTEX_DEFINE(led_mask_mutex);

/* Public state */
struct led_rgb led_color[LED_ROWS][LED_COLS];
struct led_rgb led_layer_color[LED_LAYER_COUNT]; /* zero = use led_color per cell */
uint8_t        led_mask[LED_LAYER_COUNT][LED_ROWS][LED_COLS];
uint8_t        led_brightness     = 255;   /* ambient scaling; set by light sensor */
uint8_t        led_max_brightness = 200;   /* hard cap ~78% — lower for battery saving */
uint32_t       led_current_budget = 45000; /* auto-dim when total channel sum exceeds this */

/* --------------------------------------------------------------------------
 * WS2812B encoding
 * -------------------------------------------------------------------------- */

/*
 * Encode one byte of LED data (one colour channel) into 3 SPI bytes.
 *
 * Each input bit maps to the 3-bit SPI pattern:
 *   bit=1 → 110  (0xC, 0x8 packed — see below)
 *   bit=0 → 100  (0x4, 0x0 packed — see below)
 *
 * 8 input bits × 3 SPI bits = 24 SPI bits = 3 SPI bytes, packed MSB-first.
 * The three output bytes share boundaries between encoded bits:
 *   out[0]: bits [23:16]  = encode(b7) | encode(b6)[2:1]
 *   out[1]: bits [15:8]   = encode(b6)[0] | encode(b5) | encode(b4)[2]
 *   out[2]: bits [7:0]    = encode(b4)[1:0] | encode(b3) | encode(b2) | ...
 *
 * Built via a 24-bit shift register then split to bytes.
 */
static void encode_byte(uint8_t val, uint8_t *out)
{
	uint32_t bits = 0;

	for (int i = 7; i >= 0; i--) {
		/* T1=110 (6), T0=100 (4) in the 3 LSBs */
		bits = (bits << 3) | ((val >> i & 1) ? 6u : 4u);
	}

	out[0] = (bits >> 16) & 0xFF;
	out[1] = (bits >>  8) & 0xFF;
	out[2] =  bits        & 0xFF;
}

static void encode_led(const struct led_rgb *c, uint8_t *out)
{
	/* WS2812B wire order: GRB */
	encode_byte(c->g, out);
	encode_byte(c->r, out + 3);
	encode_byte(c->b, out + 6);
}

/* --------------------------------------------------------------------------
 * Logical → physical pixel mapping
 * --------------------------------------------------------------------------
 *
 * col: 0-19 left→right, row: 0-6 top→bottom.
 * Returns the SPI strip index (0-3) and the pixel offset within that strip.
 *
 * Wiring is snake: even rows run L→R (index 0-19),
 * odd rows run R→L (index 20-39 within the same strip buffer).
 *
 *   Strip 0 (SPIM1): row 0 → pixels  0-19  (L→R)
 *                    row 1 → pixels 20-39  (R→L, i.e. col 0 → pixel 39)
 *   Strip 1 (SPIM2): row 2 → pixels  0-19
 *                    row 3 → pixels 20-39
 *   Strip 2 (SPIM3): row 4 → pixels  0-19
 *                    row 5 → pixels 20-39
 *   Strip 3 (row 6): row 6 → pixels  0-19  (L→R, single row)
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

/* Build all four SPI buffers from the current mask + color state.
 *
 * Two-pass approach:
 *   Pass 1 — composite + clamp to effective brightness → accumulate channel sum
 *   Pass 2 — apply current-limit scale factor → encode to SPI buffers
 *
 * Effective brightness = MIN(led_brightness, led_max_brightness).
 * Current scale        = MIN(1.0, led_current_budget / total_sum).
 *
 * Caller (led_commit) holds led_commit_mutex; this function additionally
 * acquires led_mask_mutex for the duration of its led_mask[] reads.
 */
static void build_buffers(void)
{
	/* Effective brightness cap (Q8: 0-255) */
	uint8_t br = (led_brightness < led_max_brightness)
	             ? led_brightness : led_max_brightness;

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

	/* Pass 2: apply current scale → encode → SPI buffers */
	uint8_t *strips[4] = {buf01, buf23, buf45, buf6};

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
			encode_led(&c, strips[strip] + pixel * BYTES_PER_LED);
		}
	}
}

/* --------------------------------------------------------------------------
 * Async SPI callbacks
 * -------------------------------------------------------------------------- */

static void cb1(const struct device *dev, int result, void *userdata)
{
	ARG_UNUSED(dev); ARG_UNUSED(result); ARG_UNUSED(userdata);
	k_sem_give(&done1);
}

static void cb2(const struct device *dev, int result, void *userdata)
{
	ARG_UNUSED(dev); ARG_UNUSED(result); ARG_UNUSED(userdata);
	k_sem_give(&done2);
}

static void cb3(const struct device *dev, int result, void *userdata)
{
	ARG_UNUSED(dev); ARG_UNUSED(result); ARG_UNUSED(userdata);
	k_sem_give(&done3);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/*
 * Bitbang WS2812B on P0.03 (row 6, 20 LEDs).
 *
 * Sends buf6 with interrupts locked. The nop counts give approximately:
 *   T1H ≈ 609 ns, T0H ≈ 250 ns, bit period ≈ 1219 ns
 * WS2812B tolerates ±150 ns on pulse widths, so these land safely in-spec.
 * Interrupt-off window: 20 × 24 × 1219 ns ≈ 585 µs.
 */
static void __attribute__((noinline, optimize("O2"))) ws2812_bitbang_row6(void)
{
	NRF_GPIO_Type *port = nrf_gpio_pin_port_decode(
				&(uint32_t){NRF_GPIO_PIN_MAP(ROW6_PORT, ROW6_PIN)});
	unsigned int key = irq_lock();

	for (int i = 0; i < (int)sizeof(buf6); i++) {
		uint8_t byte = buf6[i];

		for (int bit = 7; bit >= 0; bit--) {
			if (byte & BIT(bit)) {
				port->OUTSET = ROW6_MASK;
				NOP35;
				port->OUTCLR = ROW6_MASK;
				NOP35;
			} else {
				port->OUTSET = ROW6_MASK;
				NOP12;
				port->OUTCLR = ROW6_MASK;
				NOP58;
			}
		}
	}

	irq_unlock(key);

	/* WS2812B reset: hold data line low for >280 µs to latch the frame. */
	k_busy_wait(300);
}

void led_matrix_init(void)
{
	if (!device_is_ready(spi1) || !device_is_ready(spi2) || !device_is_ready(spi3)) {
		LOG_ERR("SPI device(s) not ready");
		return;
	}

	k_sem_init(&done1, 0, 1);
	k_sem_init(&done2, 0, 1);
	k_sem_init(&done3, 0, 1);

	/* Configure P0.03 as output low for row 6 bitbang */
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(ROW6_PORT, ROW6_PIN));
	nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(ROW6_PORT, ROW6_PIN));

	memset(led_color, 0, sizeof(led_color));
	memset(led_mask,  0, sizeof(led_mask));

	LOG_INF("LED matrix ready (rows 0-5 parallel DMA, row 6 bitbang)");
}

void led_commit(void)
{
	k_mutex_lock(&led_commit_mutex, K_FOREVER);

	build_buffers();

	/* Parallel async transfers for rows 0-5 */
	static struct spi_buf tx1_buf = {.buf = buf01, .len = sizeof(buf01)};
	static struct spi_buf tx2_buf = {.buf = buf23, .len = sizeof(buf23)};
	static struct spi_buf tx3_buf = {.buf = buf45, .len = sizeof(buf45)};
	static struct spi_buf_set tx1 = {.buffers = &tx1_buf, .count = 1};
	static struct spi_buf_set tx2 = {.buffers = &tx2_buf, .count = 1};
	static struct spi_buf_set tx3 = {.buffers = &tx3_buf, .count = 1};

	spi_transceive_cb(spi1, &spi_cfg, &tx1, NULL, cb1, NULL);
	spi_transceive_cb(spi2, &spi_cfg, &tx2, NULL, cb2, NULL);
	spi_transceive_cb(spi3, &spi_cfg, &tx3, NULL, cb3, NULL);

	k_sem_take(&done1, K_FOREVER);
	k_sem_take(&done2, K_FOREVER);
	k_sem_take(&done3, K_FOREVER);

	/* Row 6 (P0.03, 20 LEDs): bitbang after parallel DMA completes */
	ws2812_bitbang_row6();

	k_mutex_unlock(&led_commit_mutex);
}
