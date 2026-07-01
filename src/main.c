#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>

#include "led_matrix/led_matrix.h"
#include "sand/sand.h"
#include "time_display/time_display.h"
#include "display/display.h"
#include "imu/imu.h"
#include "identity/identity.h"
#include "ble/ble.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc0));

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

	LOG_INF("Every Watch starting");
	return 0;
}
