#pragma once

void imu_init(void);

/* Suspend/resume the 50 Hz polling thread.
 * Call from display_off/on — no I2C traffic while screen is off. */
void imu_suspend(void);
void imu_resume(void);
