#include "imu.h"
#include "sand/sand.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

static const struct device *bmi = DEVICE_DT_GET(DT_NODELABEL(bmi270));

static struct imu_accel latest;
static K_MUTEX_DEFINE(accel_mutex);

/*
 * Convert accelerometer axes to a sand gravity vector.
 *
 * The watch face is in the XY plane. Gravity pulls the real world downward,
 * so the accelerometer reads +1 g on whichever axis points up relative to
 * the chip. We negate to get the direction sand should fall.
 *
 * Axis mapping (verify against PCB orientation at bring-up):
 *   accel.x > 0  → watch tilted right  → sand falls right  (+col)
 *   accel.x < 0  → watch tilted left   → sand falls left   (-col)
 *   accel.y > 0  → watch face up        → sand falls down   (+row)
 *   accel.y < 0  → watch face down      → sand falls up     (-row)
 *
 * The Q8 vector components are clamped to [-256, 256].
 */
static struct sand_gravity accel_to_gravity(float ax, float ay)
{
	/* Scale g (≈9.8 m/s²) to Q8 range: 9.8 → 256 → scale = 26.1 */
	const float scale = 256.0f / 9.8f;
	int col = (int)(ax * scale);
	int row = (int)(ay * scale);

	col = col >  256 ?  256 : col < -256 ? -256 : col;
	row = row >  256 ?  256 : row < -256 ? -256 : row;

	return (struct sand_gravity){.col = col, .row = row};
}

/* -------------------------------------------------------------------------
 * 50 Hz polling thread
 * -------------------------------------------------------------------------
 * Reads accelerometer at 50 Hz — fast enough for responsive sand physics
 * without hammering I2C. The sand thread runs at 30 Hz so every tick has
 * a fresh gravity vector.
 */

#define IMU_STACK_SIZE 512
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

		float fax = sensor_value_to_float(&ax);
		float fay = sensor_value_to_float(&ay);
		float faz = sensor_value_to_float(&az);

		k_mutex_lock(&accel_mutex, K_FOREVER);
		latest = (struct imu_accel){.x = fax, .y = fay, .z = faz};
		k_mutex_unlock(&accel_mutex);

		sand_set_gravity(accel_to_gravity(fax, fay));

		k_msleep(IMU_PERIOD_MS);
	}
}

void imu_get_accel(struct imu_accel *out)
{
	k_mutex_lock(&accel_mutex, K_FOREVER);
	*out = latest;
	k_mutex_unlock(&accel_mutex);
}

void imu_suspend(void) { k_thread_suspend(&imu_thread_data); }
void imu_resume(void)  { k_thread_resume(&imu_thread_data); }

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
