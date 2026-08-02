/*
 * Stage 0 bring-up image.
 *
 * The smallest thing that can tell you whether a fresh board is alive. No
 * sensor drivers, no RTC driver, no SPI, no BLE, no LED matrix — nothing that
 * can hang during driver initialisation and leave you staring at a dead board
 * with no idea why.
 *
 * What it proves:
 *   - the chip is running and the clock is configured
 *   - SWD and the RTT console work
 *   - the I2C bus comes up and is not held low
 *   - which of the four expected devices actually ACK
 *   - the buttons and charge-detect GPIOs read sensibly
 *
 * Everything beyond that is stage 1 (bringup.conf + the self-test).
 *
 * Build:
 *   west build -b every_watch/nrf52833 every_watch_firmware -p always \
 *       -d build_stage0 -- -DCONF_FILE=stage0.conf
 *
 * Note this replaces prj.conf rather than overlaying it, and builds without
 * MCUboot, so the image links at 0x0 and runs directly.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/printk.h>

static const struct device *const i2c0_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));

static const struct gpio_dt_spec btn_l = GPIO_DT_SPEC_GET(DT_ALIAS(btn_left),  gpios);
static const struct gpio_dt_spec btn_r = GPIO_DT_SPEC_GET(DT_ALIAS(btn_right), gpios);
static const struct gpio_dt_spec chg   = GPIO_DT_SPEC_GET(DT_NODELABEL(chg_indicator), gpios);

struct i2c_expect {
	uint8_t     addr;
	const char *name;
};

static const struct i2c_expect expected[] = {
	{0x23, "BH1750   ambient light"},
	{0x32, "FRTC8900 real-time clock"},
	{0x55, "BQ27441  fuel gauge"},
	{0x68, "BMI260   IMU"},
};

/* Zero-length write. ACK means something is holding SDA low for that address. */
static bool i2c_ping(uint8_t addr)
{
	uint8_t dummy = 0;
	struct i2c_msg msg = {
		.buf   = &dummy,
		.len   = 0,
		.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
	};

	return i2c_transfer(i2c0_dev, &msg, 1, addr) == 0;
}

static void report_reset_cause(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		printk("reset cause  : unavailable\n");
		return;
	}

	printk("reset cause  : 0x%08x ", cause);
	if (cause & RESET_POR)          printk("power-on ");
	if (cause & RESET_PIN)          printk("pin ");
	if (cause & RESET_WATCHDOG)     printk("watchdog ");
	if (cause & RESET_SOFTWARE)     printk("software ");
	if (cause & RESET_DEBUG)        printk("debug ");
	if (cause & RESET_BROWNOUT)     printk("brownout ");
	if (cause & RESET_LOW_POWER_WAKE) printk("lpwake ");
	printk("\n");

	hwinfo_clear_reset_cause();
}

static void scan_i2c(void)
{
	printk("\n-- I2C bus (SCL P0.04 / SDA P0.01) ------------------------\n");

	if (!device_is_ready(i2c0_dev)) {
		printk("  I2C CONTROLLER NOT READY - nothing else here will work\n");
		return;
	}

	int found = 0;

	printk("  full scan 0x08-0x77:");
	for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
		if (i2c_ping(addr)) {
			printk(" 0x%02x", addr);
			found++;
		}
	}
	printk("   (%d device%s)\n\n", found, found == 1 ? "" : "s");

	for (int i = 0; i < (int)ARRAY_SIZE(expected); i++) {
		bool ack = i2c_ping(expected[i].addr);

		printk("  0x%02x  %-26s %s\n",
		       expected[i].addr, expected[i].name,
		       ack ? "ACK" : "-- no response --");
	}

	if (found == 0) {
		printk("\n  Nothing on the bus at all. Check the pull-up resistors on\n");
		printk("  SDA/SCL, and that the sensors have power. A bus stuck low\n");
		printk("  reads as zero devices too - measure SDA and SCL idle high.\n");
	}
}

static void read_gpios(void)
{
	printk("\n-- GPIO --------------------------------------------------\n");

	if (gpio_is_ready_dt(&btn_l) && gpio_is_ready_dt(&btn_r)) {
		gpio_pin_configure_dt(&btn_l, GPIO_INPUT);
		gpio_pin_configure_dt(&btn_r, GPIO_INPUT);
		/* Active-low in the devicetree, so 1 means pressed. */
		printk("  left  button P0.17 : %s\n",
		       gpio_pin_get_dt(&btn_l) ? "PRESSED" : "released");
		printk("  right button P0.15 : %s\n",
		       gpio_pin_get_dt(&btn_r) ? "PRESSED" : "released");
	} else {
		printk("  button GPIO not ready\n");
	}

	if (gpio_is_ready_dt(&chg)) {
		gpio_pin_configure_dt(&chg, GPIO_INPUT);
		printk("  charge detect P0.05 : %s\n",
		       gpio_pin_get_dt(&chg) ? "CHARGING" : "not charging");
	} else {
		printk("  charge detect GPIO not ready\n");
	}
}

int main(void)
{
	/* Give RTT a moment to attach so the banner is not lost if the viewer
	 * was opened after power-up. */
	k_msleep(200);

	printk("\n\n");
	printk("==========================================================\n");
	printk(" Every Watch - STAGE 0 bring-up\n");
	printk(" no sensors, no RTC, no SPI, no BLE, no LEDs\n");
	printk("==========================================================\n");
	report_reset_cause();

	scan_i2c();
	read_gpios();

	printk("\n----------------------------------------------------------\n");
	printk(" Shell is live on RTT buffer 1. Useful commands:\n");
	printk("   i2c scan i2c@40003000\n");
	printk("   i2c read i2c@40003000 0x68 0x00 1     (BMI260 chip id, expect 0x27)\n");
	printk("   i2c read i2c@40003000 0x23 0x00 1     (BH1750)\n");
	printk("   device list\n");
	printk("   kernel uptime\n");
	printk("----------------------------------------------------------\n");
	printk("\nPress a button now to check it registers:\n");

	/* Idle loop reporting button edges, so the operator can verify both
	 * buttons without needing the shell. */
	int last_l = -1, last_r = -1;

	while (true) {
		if (gpio_is_ready_dt(&btn_l) && gpio_is_ready_dt(&btn_r)) {
			int l = gpio_pin_get_dt(&btn_l);
			int r = gpio_pin_get_dt(&btn_r);

			if (l != last_l) {
				printk("  LEFT  %s\n", l ? "pressed" : "released");
				last_l = l;
			}
			if (r != last_r) {
				printk("  RIGHT %s\n", r ? "pressed" : "released");
				last_r = r;
			}
		}
		k_msleep(25);
	}
}
