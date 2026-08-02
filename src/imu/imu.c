#include "imu.h"
#include "sand/sand.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

static const struct device *bmi = DEVICE_DT_GET(DT_NODELABEL(bmi270));
static bool imu_ready;

/* -------------------------------------------------------------------------
 * Wrist-tilt wake — BMI270 any-motion feature on INT1 (hardware, independent
 * of the 50 Hz poll thread below; keeps firing even while that thread is
 * suspended with the display off). Mirrors the plan's "wrist tilt wakes the
 * display" transition, so it only acts while the display is off — once it's
 * on, motion from normal wrist movement during sand mode must not keep
 * restarting the reveal.
 * ------------------------------------------------------------------------- */

static void motion_trigger_handler(const struct device *dev,
				    const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev); ARG_UNUSED(trig);

	if (!display_is_on()) {
		display_wake_and_reveal();
	}
}

static int motion_trigger_init(void)
{
	/* Threshold/duration tuned by feel, not measured — see light.c's lux
	 * breakpoints for the same caveat on this hardware. 0.15g / 40ms is a
	 * deliberate wrist raise, not a false trigger from setting the watch
	 * down on a desk. */
	struct sensor_value thresh = {.val1 = 0, .val2 = 150000}; /* 0.15 g */
	struct sensor_value dur    = {.val1 = 40, .val2 = 0};     /* 40 ms */

	int rc = sensor_attr_set(bmi, SENSOR_CHAN_ACCEL_XYZ,
				  SENSOR_ATTR_SLOPE_TH, &thresh);
	rc |= sensor_attr_set(bmi, SENSOR_CHAN_ACCEL_XYZ,
			       SENSOR_ATTR_SLOPE_DUR, &dur);
	if (rc) {
		LOG_ERR("BMI270 any-motion attr config failed: %d", rc);
		return rc;
	}

	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_MOTION,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	rc = sensor_trigger_set(bmi, &trig, motion_trigger_handler);
	if (rc) {
		LOG_ERR("BMI270 any-motion trigger set failed: %d", rc);
		return rc;
	}

	return 0;
}

/*
 * Convert BMI270 sensor_value accelerometer reading to a Q8 sand gravity vector.
 * GRAVITY_Q8_1G (256) = 1g. Scale factor: GRAVITY_Q8_1G / 9.8 ≈ 26.
 *
 * sensor_value: val1 = integer m/s², val2 = fractional µm/s² (millionths).
 * For gravity direction we only need ~4% precision so the val2 term is
 * approximated (val2 / 38462 ≈ val2 * GRAVITY_Q8_1G / 9800000).
 *
 * Axis mapping (verify against PCB orientation at bring-up):
 *   accel.x > 0  → watch tilted right  → sand falls right  (+col)
 *   accel.x < 0  → watch tilted left   → sand falls left   (-col)
 *   accel.y > 0  → watch face up        → sand falls down   (+row)
 *   accel.y < 0  → watch face down      → sand falls up     (-row)
 */
static struct sand_gravity accel_to_gravity(const struct sensor_value *ax,
					    const struct sensor_value *ay)
{
	int col = ax->val1 * 26 + (int)(ax->val2 / 38462);
	int row = ay->val1 * 26 + (int)(ay->val2 / 38462);

	col = CLAMP(col, -GRAVITY_Q8_1G, GRAVITY_Q8_1G);
	row = CLAMP(row, -GRAVITY_Q8_1G, GRAVITY_Q8_1G);

	return (struct sand_gravity){.col = col, .row = row};
}

#define IMU_STACK_SIZE 1024
#define IMU_PRIORITY   4
/* Matches sand.c's 30 Hz tick — sand_set_gravity()'s only consumer. Was
 * 20 ms (50 Hz): over a third of those reads were overwritten before the
 * sand thread ever looked at them, pure wasted I2C traffic and wakeups on
 * a board targeting <10 uA idle. */
#define IMU_PERIOD_MS  33  /* ~30 Hz */

static K_THREAD_STACK_DEFINE(imu_stack, IMU_STACK_SIZE);
static struct k_thread imu_thread_data;

static void imu_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (true) {
		int err = sensor_sample_fetch(bmi);

		if (err) {
			LOG_WRN("BMI270 fetch failed: %d", err);
			k_msleep(IMU_PERIOD_MS);
			continue;
		}

		struct sensor_value ax, ay;

		if (sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_X, &ax) ||
		    sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_Y, &ay)) {
			k_msleep(IMU_PERIOD_MS);
			continue;
		}

		sand_set_gravity(accel_to_gravity(&ax, &ay));

		k_msleep(IMU_PERIOD_MS);
	}
}

void imu_suspend(void)
{
	if (!imu_ready) {
		return;
	}
	k_thread_suspend(&imu_thread_data);
	int rc = pm_device_action_run(bmi, PM_DEVICE_ACTION_SUSPEND);
	if (rc) {
		LOG_ERR("BMI270 suspend failed: %d", rc);
	}
}

void imu_resume(void)
{
	if (!imu_ready) {
		return;
	}
	int rc = pm_device_action_run(bmi, PM_DEVICE_ACTION_RESUME);
	if (rc) {
		LOG_ERR("BMI270 resume failed: %d", rc);
	}
	k_thread_resume(&imu_thread_data);
}

void imu_init(void)
{
	if (!device_is_ready(bmi)) {
		LOG_ERR("BMI270 not ready");
		return;
	}

	/* Non-fatal: worst case is losing wrist-tilt wake, gravity-driven sand
	 * mode still works off the poll thread below. */
	motion_trigger_init();

	k_thread_create(&imu_thread_data, imu_stack,
			K_THREAD_STACK_SIZEOF(imu_stack),
			imu_thread, NULL, NULL, NULL,
			IMU_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&imu_thread_data, "imu");
	imu_ready = true;

	LOG_INF("IMU started (30 Hz)");
}
