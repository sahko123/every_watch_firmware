#include "imu.h"
#include "sand/sand.h"
#include "display/display.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

static const struct device *bmi = DEVICE_DT_GET(DT_NODELABEL(bmi260));
static bool imu_ready;

/* -------------------------------------------------------------------------
 * Wrist-tilt wake — DOES NOT WORK ON THIS HARDWARE. Kept, disarmed, because
 * the code is correct for a BMI270 and is the starting point for whatever
 * replaces it.
 *
 * Tested on hardware 2026-08-02: with the display off and the board being
 * actively moved and shaken for 50 s, the trigger fired exactly zero times.
 * A control run with the board stationary was likewise zero. sensor_attr_set()
 * and sensor_trigger_set() both return success, so the driver does write the
 * registers — there is simply nothing behind them on this part.
 *
 * Either the BMI260's feature-engine microcode does not implement any-motion,
 * or it implements it at different feature-register addresses than the
 * BMI270's page 1 0x3C/0x3E that this inherited. Bosch treats the per-part
 * feature list as NDA material (their own community forum says so), so the
 * two cannot be told apart from outside — and it does not matter, because
 * both mean this cannot be made to work as written.
 *
 * Replacements, in rough order of preference:
 *   - tap-to-wake. Bosch's own bmi2_defs.h documents a wake-up feature
 *     specifically "for bmi260" (single/double/triple tap), so unlike
 *     any-motion it is known to exist on this part. Different gesture, but
 *     hardware-accelerated and it still wakes the CPU from sleep.
 *   - detect the raise in software from raw accel. Works for certain, but
 *     needs the accelerometer polling while the display is off — imu_suspend()
 *     currently stops it entirely — so it costs idle current against the
 *     <10 uA target.
 * ------------------------------------------------------------------------- */

static void motion_trigger_handler(const struct device *dev,
				    const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev); ARG_UNUSED(trig);

	/* Logged unconditionally, and before the display check, so the trigger
	 * can be observed firing regardless of display state. This is the only
	 * way to confirm the BMI260's microcode actually implements any-motion:
	 * sensor_trigger_set() succeeding only proves the driver wrote the
	 * feature registers, not that anything is behind them on this part —
	 * the anymo_1/anymo_2 addresses are inherited from the BMI270 and Bosch
	 * treats the BMI260 feature list as NDA material. If this never appears
	 * while the board is being moved, any-motion is not implemented here
	 * and wrist-tilt wake needs doing in software instead. */
	LOG_INF("any-motion trigger fired (display %s)",
		display_is_on() ? "on" : "off");

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
		LOG_ERR("BMI260 any-motion attr config failed: %d", rc);
		return rc;
	}

	struct sensor_trigger trig = {
		.type = SENSOR_TRIG_MOTION,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	rc = sensor_trigger_set(bmi, &trig, motion_trigger_handler);
	if (rc) {
		LOG_ERR("BMI260 any-motion trigger set failed: %d", rc);
		return rc;
	}

	return 0;
}

/*
 * Convert BMI260 sensor_value accelerometer reading to a Q8 sand gravity vector.
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
			LOG_WRN("BMI260 fetch failed: %d", err);
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
		LOG_ERR("BMI260 suspend failed: %d", rc);
	}
}

void imu_resume(void)
{
	if (!imu_ready) {
		return;
	}
	int rc = pm_device_action_run(bmi, PM_DEVICE_ACTION_RESUME);
	if (rc) {
		LOG_ERR("BMI260 resume failed: %d", rc);
	}
	k_thread_resume(&imu_thread_data);
}

/*
 * Power up and configure the accelerometer.
 *
 * This is not optional setup — the driver deliberately leaves init with the
 * sensor in advanced power save and the accelerometer OFF, and the only thing
 * that switches it on is setting the sampling frequency (set_accel_odr_osr()
 * is what writes PWR_CTRL_ACC_EN). Without this the driver reports ready,
 * sample_fetch and channel_get all succeed, and every axis reads exactly
 * 0.00 — which is precisely how this presented before it was found.
 *
 * Order matters and is prescribed by the driver: full scale and oversampling
 * first, sampling frequency LAST, because that call also selects the power
 * mode. Zephyr's own bmi270 sample carries the same warning.
 *
 * The gyroscope is deliberately left powered down. Nothing in this firmware
 * uses it — sand gravity needs the accelerometer only — and on a coin cell
 * the gyro is by far the more expensive of the two to run.
 */
static int accel_config(void)
{
	/* 2g: this only ever measures gravity to derive a tilt direction, so
	 * the smallest range gives the most resolution where it matters. */
	struct sensor_value full_scale  = {.val1 = 2,  .val2 = 0};
	struct sensor_value oversampling = {.val1 = 1, .val2 = 0}; /* normal */
	/* 50 Hz against a 30 Hz consumer: comfortably above the poll rate
	 * without paying for bandwidth nothing reads. */
	struct sensor_value sampling_freq = {.val1 = 50, .val2 = 0};
	int rc;

	rc  = sensor_attr_set(bmi, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_FULL_SCALE, &full_scale);
	rc |= sensor_attr_set(bmi, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_OVERSAMPLING, &oversampling);
	rc |= sensor_attr_set(bmi, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);

	if (rc) {
		LOG_ERR("accel config failed: %d — axes will read zero", rc);
	}

	return rc;
}

void imu_init(void)
{
	if (!device_is_ready(bmi)) {
		LOG_ERR("BMI260 not ready");
		return;
	}

	/* Before the trigger: any-motion watches the accelerometer, so it has
	 * to be running first. */
	(void)accel_config();

	/* Not called: verified non-functional on the BMI260 — see the block
	 * comment above motion_trigger_handler(). Arming it configured a GPIO
	 * interrupt and wrote feature registers for a trigger that can never
	 * fire, which is worse than not trying: it looks like a working wake
	 * path in the code and in the boot log. Left compiled (referenced
	 * here) so it does not rot, and so it is ready if the correct BMI260
	 * feature-register addresses ever turn up.
	 */
	if (IS_ENABLED(CONFIG_EW_IMU_ANYMOTION_WAKE)) {
		motion_trigger_init();
	}

	k_thread_create(&imu_thread_data, imu_stack,
			K_THREAD_STACK_SIZEOF(imu_stack),
			imu_thread, NULL, NULL, NULL,
			IMU_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&imu_thread_data, "imu");
	imu_ready = true;

	LOG_INF("IMU started (30 Hz)");
}
