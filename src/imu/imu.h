#pragma once

/* Initialise BMI270 and start the gravity-feed loop. */
void imu_init(void);

/* Latest accelerometer reading in m/s², updated at 50 Hz. */
struct imu_accel {
	float x;
	float y;
	float z;
};

void imu_get_accel(struct imu_accel *out);
