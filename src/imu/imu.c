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

static struct imu_accel latest;
static K_MUTEX_DEFINE(accel_mutex);

/*
 * Convert BMI270 sensor_value accelerometer reading to a Q8 sand gravity vector.
 * Q8 format: 256 = 1g (≈ 9.8 m/s²). Scale factor: 256 / 9.8 ≈ 26.
 *
 * sensor_value: val1 = integer m/s², val2 = fractional µm/s² (millionths).
 * For gravity direction we only need ~4% precision so the val2 term is
 * approximated (val2 / 38462 ≈ val2 * 256 / 9800000).
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

	col = CLAMP(col, -256, 256);
	row = CLAMP(row, -256, 256);

	return (struct sand_gravity){.col = col, .row = row};
}

/* -------------------------------------------------------------------------
 * 50 Hz polling thread
 * -------------------------------------------------------------------------
 * Reads accelerometer at 50 Hz — fast enough for responsive sand physics
 * without hammering I2C. The sand thread runs at 30 Hz so every tick has
 * a fresh gravity vector.
 */

#define IMU_STACK_SIZE 1024
#define IMU_PRIORITY   4
#define IMU_PERIOD_MS  20  /* 50 Hz */

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

		struct sensor_value ax, ay, az;

		sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_X, &ax);
		sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_Y, &ay);
		sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_Z, &az);

		/* Hot path: integer Q8 gravity for sand — no FPU needed */
		sand_set_gravity(accel_to_gravity(&ax, &ay));

		/* Float conversion only for external callers of imu_get_accel() */
		k_mutex_lock(&accel_mutex, K_FOREVER);
		latest.x = sensor_value_to_float(&ax);
		latest.y = sensor_value_to_float(&ay);
		latest.z = sensor_value_to_float(&az);
		k_mutex_unlock(&accel_mutex);

		k_msleep(IMU_PERIOD_MS);
	}
}

void imu_get_accel(struct imu_accel *out)
{
	k_mutex_lock(&accel_mutex, K_FOREVER);
	*out = latest;
	k_mutex_unlock(&accel_mutex);
}

void imu_suspend(void)
{
	k_thread_suspend(&imu_thread_data);
	/* Put the BMI270 hardware into suspend mode (~3 µA vs ~950 µA normal) */
	pm_device_action_run(bmi, PM_DEVICE_ACTION_SUSPEND);
}

void imu_resume(void)
{
	/* Restore normal mode before resuming the poll thread */
	pm_device_action_run(bmi, PM_DEVICE_ACTION_RESUME);
	k_thread_resume(&imu_thread_data);
}

void imu_init(void)
{
	if (!device_is_ready(bmi)) {
		LOG_ERR("BMI270 not ready");
		return;
	}

	k_thread_create(&imu_thread_data, imu_stack,
			K_THREAD_STACK_SIZEOF(imu_stack),
			imu_thread, NULL, NULL, NULL,
			IMU_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&imu_thread_data, "imu");

	LOG_INF("IMU started (50 Hz)");
}
