#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/logging/log.h>

#include "led_matrix/led_matrix.h"
#include "sand/sand.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct device *const rtc = DEVICE_DT_GET(DT_ALIAS(rtc0));

int main(void)
{
	if (!device_is_ready(rtc)) {
		LOG_ERR("RTC device not ready");
		return -ENODEV;
	}

	led_matrix_init();

	/* Sand colour: warm amber */
	led_color_fill(255, 160, 20);

	sand_init();
	sand_add_particles(60);

	LOG_INF("Every Watch starting");
	return 0;
}
