#pragma once

void imu_init(void);

/* Suspend/resume the 50 Hz polling thread.
 * Call from display_off/on — no I2C traffic while screen is off. */
void imu_suspend(void);
void imu_resume(void);

struct imu_accel { float x; float y; float z; };
void imu_get_accel(struct imu_accel *out);
