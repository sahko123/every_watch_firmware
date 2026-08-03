#include "imu.h"
#include "sand/sand.h"
#include "display/display.h"
#include "ui/ui.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

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

	/* Only wake from rest: once a page is up, ordinary wrist movement
	 * during use must not keep yanking the display back to the clock. */
	if (!display_is_on()) {
		ui_goto(UI_PAGE_CLOCK);
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

/* -------------------------------------------------------------------------
 * Accelerometer → sand gravity
 *
 * AXIS MAPPING. Which physical axis points where on the PCB is not derivable
 * from this source, and hardwarer_spec.md records only the I2C address and the
 * two interrupt pins. These four constants are the entire mapping — establish
 * them on hardware with the procedure below and change them here; nothing else
 * needs touching.
 *
 *   Build with the imu module at LOG_LEVEL_DBG, put the sand toy up, and watch
 *   the "accel" line while holding the watch in each orientation:
 *
 *     display upright, top of the screen up   → row should read +1 (sand down)
 *     display upright, rotated 90° clockwise  → col should read -1 or +1
 *     display flat on the bench, face up      → both should read 0
 *
 *   If an axis moves the sand the wrong way, flip its SIGN. If tilting left/
 *   right moves sand vertically instead, swap COL_AXIS and ROW_AXIS.
 * ------------------------------------------------------------------------- */

enum accel_axis { AX_X = 0, AX_Y = 1, AX_Z = 2 };

/*
 * Established on hardware 2026-08-03, holding the board in three known
 * orientations and reading the vector back:
 *
 *   flat, display up      X +0.04  Y +0.22  Z -9.82   in-plane pull ~zero
 *   upright, reading it   X -9.61  Y +0.13  Z +0.37   gravity toward digit feet
 *   right edge to floor   X +0.02  Y +9.70  Z +0.07   gravity toward screen right
 *
 * So X is the display's VERTICAL axis and Y its horizontal one — swapped from
 * what this code originally assumed, which had tilting the watch upright
 * sliding the sand sideways. X additionally runs negative toward the bottom of
 * the screen, hence the inversion; Y already increases to the right.
 *
 * Z is the face normal and reads negative with the display upwards, meaning
 * the sensor's +Z points back through the board. Nothing uses that sign — Z
 * only contributes its magnitude to the normalisation — but it is the fact
 * that identifies the part's mounting if this ever needs revisiting.
 */
#define IMU_COL_AXIS  AX_Y
#define IMU_COL_SIGN  (+1)   /* +col = right */
#define IMU_ROW_AXIS  AX_X
#define IMU_ROW_SIGN  (-1)   /* +row = down  */

/*
 * Q8 counts per m/s². GRAVITY_Q8_1G (256) = 1 g, so 256/9.8 ≈ 26.
 *
 * sensor_value is val1 = whole m/s², val2 = millionths. Only a direction is
 * wanted, so the fractional term is approximated:
 * val2 / 38462 ≈ val2 * GRAVITY_Q8_1G / 9800000.
 */
static int accel_to_q8(const struct sensor_value *v)
{
	return v->val1 * 26 + (int)(v->val2 / 38462);
}

/* Integer square root, bit-by-bit. No FPU on this part and this runs 30 times
 * a second, so the libm call is not worth it for a value used to one part in
 * 256. */
static uint32_t isqrt32(uint32_t x)
{
	uint32_t res = 0;
	uint32_t bit = 1u << 30;

	while (bit > x) {
		bit >>= 2;
	}
	while (bit) {
		if (x >= res + bit) {
			x -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

/*
 * Below this the reading carries no usable direction: free-fall, or the sensor
 * saturating on an impact. ~0.25 g. Returning zero gravity parks the sand
 * rather than sending it somewhere arbitrary.
 */
#define ACCEL_MIN_MAG_Q8 64

/*
 * All three axes, projected onto the display plane and normalised.
 *
 * Reading Z is what makes the other two mean anything. The sand lives in the
 * display plane, so only the in-plane components can push it — but their
 * *magnitude* is only interpretable relative to the whole vector. Dividing by
 * |a| turns each into a true direction cosine: full tilt gives ±256 with the
 * display vertical, and falls to zero as it lays flat, which is the geometry
 * actually being modelled.
 *
 * Without it the raw values stood in for direction, and that breaks the moment
 * the watch is doing anything but sitting still. A shake adds linear
 * acceleration on top of gravity, so |a| runs to 2-3 g, the raw axis saturates
 * against the clamp, and the sand slams to one edge and stays pinned there
 * until the movement stops. Normalised, a shake changes the direction the sand
 * is pulled without ever exceeding one gravity of pull.
 */
static struct sand_gravity accel_to_gravity(const struct sensor_value *a)
{
	int q8[3] = {
		accel_to_q8(&a[AX_X]),
		accel_to_q8(&a[AX_Y]),
		accel_to_q8(&a[AX_Z]),
	};

	uint32_t mag2 = (uint32_t)(q8[0] * q8[0]) +
			(uint32_t)(q8[1] * q8[1]) +
			(uint32_t)(q8[2] * q8[2]);
	uint32_t mag = isqrt32(mag2);

	/*
	 * Shake: how far the total departs from 1 g in either direction.
	 *
	 * A watch held still reads exactly one gravity however it is turned, so
	 * anything else in |a| is the hand moving it. Taking the absolute
	 * deviation catches both halves of a shake — the throw and the
	 * turnaround — where reading the raw magnitude would only see one.
	 *
	 * Smoothed over roughly four samples (~130 ms at 30 Hz). A shake is an
	 * oscillation that crosses 1 g twice a cycle, so the instantaneous value
	 * keeps returning to zero mid-shake; without the filter the grains would
	 * be kicked in stutters rather than continuously.
	 */
	static uint16_t shake_filt;
	int excess = abs((int)mag - GRAVITY_Q8_1G);

	shake_filt = (uint16_t)((shake_filt * 3 + excess) / 4);

	if (mag < ACCEL_MIN_MAG_Q8) {
		return (struct sand_gravity){
			.col = 0, .row = 0, .shake = shake_filt
		};
	}

	int col = IMU_COL_SIGN * q8[IMU_COL_AXIS] * GRAVITY_Q8_1G / (int)mag;
	int row = IMU_ROW_SIGN * q8[IMU_ROW_AXIS] * GRAVITY_Q8_1G / (int)mag;

	col = CLAMP(col, -GRAVITY_Q8_1G, GRAVITY_Q8_1G);
	row = CLAMP(row, -GRAVITY_Q8_1G, GRAVITY_Q8_1G);

	return (struct sand_gravity){
		.col = col, .row = row, .shake = shake_filt
	};
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

	int dbg_div = 0;

	while (true) {
		int err = sensor_sample_fetch(bmi);

		if (err) {
			LOG_WRN("BMI260 fetch failed: %d", err);
			k_msleep(IMU_PERIOD_MS);
			continue;
		}

		struct sensor_value a[3];

		if (sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_X, &a[AX_X]) ||
		    sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_Y, &a[AX_Y]) ||
		    sensor_channel_get(bmi, SENSOR_CHAN_ACCEL_Z, &a[AX_Z])) {
			k_msleep(IMU_PERIOD_MS);
			continue;
		}

		struct sand_gravity g = accel_to_gravity(a);

		sand_set_gravity(g);

		/*
		 * Alignment readout — the procedure described above
		 * accel_to_gravity(). Rate-limited to ~2 Hz because 30 Hz of
		 * this floods RTT badly enough to stall the log backend, and a
		 * hand tilting a watch is nowhere near that fast anyway.
		 *
		 * INFO, not DEBUG: bringup.conf runs the whole build at INFO,
		 * so a DEBUG line here compiles away and the reading silently
		 * returns nothing.
		 */
		if (IS_ENABLED(CONFIG_EW_IMU_AXIS_DEBUG) && (++dbg_div >= 15)) {
			dbg_div = 0;
			LOG_INF("accel % 3d.%02d % 3d.%02d % 3d.%02d m/s2 "
				"-> col %+4d row %+4d  step %+d,%+d  shake %u",
				a[AX_X].val1, abs(a[AX_X].val2) / 10000,
				a[AX_Y].val1, abs(a[AX_Y].val2) / 10000,
				a[AX_Z].val1, abs(a[AX_Z].val2) / 10000,
				g.col, g.row,
				(g.col > 64) - (g.col < -64),
				(g.row > 64) - (g.row < -64),
				g.shake);
		}

		k_msleep(IMU_PERIOD_MS);
	}
}

void imu_suspend(void)
{
	if (!imu_ready) {
		return;
	}

	/* Axis bring-up needs a reading on demand, without first getting a page
	 * up to keep the display awake — that would put button handling and the
	 * display state machine in the path of what is meant to be a direct
	 * measurement. Costs continuous accelerometer current, hence bring-up
	 * only; see CONFIG_EW_IMU_AXIS_DEBUG. */
	if (IS_ENABLED(CONFIG_EW_IMU_AXIS_DEBUG)) {
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
