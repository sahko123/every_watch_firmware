#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "time_display/time_display.h"
#include "display/display.h"
#include "imu/imu.h"
#include "identity/identity.h"
#include "ble/ble.h"
#include "battery/battery.h"
#include "light/light.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc0));

/* Hold left button for 3 seconds while USB is plugged in to enter DFU mode.
 * sys_reboot() hands control back to MCUboot, which opens a 5-second USB DFU
 * window. Works from battery: plug in USB first, then hold the button. */
static const struct gpio_dt_spec btn_dfu = GPIO_DT_SPEC_GET(DT_ALIAS(btn_left), gpios);

int main(void)
{
	if (!device_is_ready(rtc)) {
		LOG_ERR("RTC device not ready");
		return -ENODEV;
	}

	led_matrix_init();

	/* Sand: warm amber (per-cell, no layer_color override) */
	led_color_fill(255, 160, 20);

	/* Digits: cool white — revealed beneath sand as particles clear */
	led_layer_color[LED_LAYER_DIGITS] = (struct led_rgb){220, 220, 255};

	time_display_init(rtc);

	sand_init();
	sand_add_particles(60);

	display_init();
	imu_init();
	identity_init();
	ble_init();
	battery_init();
	light_init();

	gpio_pin_configure_dt(&btn_dfu, GPIO_INPUT);

	LOG_INF("Every Watch starting");

	int32_t held_ms = 0;

	while (true) {
		k_sleep(K_MSEC(50));

		if (gpio_pin_get_dt(&btn_dfu)) {
			held_ms += 50;
			if (held_ms >= 3000) {
				LOG_INF("DFU: rebooting into bootloader");
				sys_reboot(SYS_REBOOT_COLD);
			}
		} else {
			held_ms = 0;
		}
	}
}
