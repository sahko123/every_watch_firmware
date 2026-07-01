#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Gravity vector: unit direction the simulation treats as "down".
 * Components are fixed-point Q8 signed integers (-256 = -1.0, 256 = +1.0).
 * Call sand_set_gravity() on each accelerometer reading.
 * Default at init: straight down (col_step=0, row_step=+1).
 */
struct sand_gravity {
	int16_t col; /* +col = right */
	int16_t row; /* +row = down  */
};

/*
 * Initialise the simulation. Call after led_matrix_init().
 * Spawns the 30 Hz tick thread and seeds particles in the top row.
 */
void sand_init(void);

/* Update gravity from accelerometer. Thread-safe. */
void sand_set_gravity(struct sand_gravity g);

/* Add N particles at random positions in the top row (or gravity-entry edge).
 * Clamped to the grid capacity (140 cells). Thread-safe. */
void sand_add_particles(int n);

/* Remove all particles. Thread-safe. */
void sand_clear(void);

/* Number of live particles currently in the simulation. */
int sand_count(void);

/* Suspend / resume the simulation thread (called by display state machine). */
void sand_suspend(void);
void sand_resume(void);
