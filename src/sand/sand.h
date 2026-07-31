#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "led_matrix/led_matrix.h"

/* Q8 fixed-point scale: GRAVITY_Q8_1G = 1.0 g. Scale factor for sensor_value
 * (m/s²): GRAVITY_Q8_1G / 9.8 ≈ 26 counts per m/s². */
#define GRAVITY_Q8_1G  256

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

/*
 * Constrained mode — the time reveal.
 *
 * Hand it a 7x20 bitmap of cells to fill (the digit shape). The simulation
 * then switches from free physics to the waterfall:
 *
 *   - gravity is fixed straight down and the accelerometer is ignored, so the
 *     reveal looks the same however the watch is being held
 *   - particles are streamed into the top of any column that still has cells
 *     to fill, staggered so the columns do not drop in lockstep
 *   - a falling particle locks in place when it reaches the lowest cell of the
 *     target still empty in its column, so the shape fills from the bottom up
 *   - particles in columns with nothing left to fill keep going and are
 *     removed at the bottom edge
 *
 * Pass NULL to drop back to free physics.
 */
void sand_set_target(const uint8_t target[LED_ROWS][LED_COLS]);

/* True once every target cell is occupied. Always false in free mode. */
bool sand_target_complete(void);
