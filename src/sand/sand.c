#include "sand.h"
#include "led_matrix/led_matrix.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(sand, LOG_LEVEL_INF);

/* XOR-shift PRNG — fast, no entropy hardware needed */
static uint32_t rng_state = 0xDEADBEEF;

static uint32_t rand32(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* -------------------------------------------------------------------------
 * Grid
 * -------------------------------------------------------------------------
 * The simulation runs on the same 7×20 logical grid as the LED matrix.
 * LED_LAYER_DIGITS cells are treated as immovable obstacles — sand cannot
 * enter them but can rest against them.
 */

static uint8_t grid[LED_ROWS][LED_COLS]; /* 1 = particle, 0 = empty */

/* Default gravity: straight down. GRAVITY_Q8_1G defined in sand.h. */
static struct sand_gravity gravity = {.col = 0, .row = GRAVITY_Q8_1G};
static K_MUTEX_DEFINE(sand_mutex);

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

static bool in_bounds(int col, int row)
{
	return col >= 0 && col < LED_COLS && row >= 0 && row < LED_ROWS;
}

/* A cell is passable if it's in bounds and not already occupied by a particle.
 * Digit cells are NOT obstacles — sand flows over them and reveals them beneath. */
static bool passable(int col, int row)
{
	return in_bounds(col, row) && !grid[row][col];
}

/* Convert Q8 gravity to primary step direction (±1 or 0 per axis). */
static void gravity_step(int *dcol, int *drow)
{
	/* Quantise: pick the dominant axis first, allow diagonal fall */
	*dcol = (gravity.col > 64) ? 1 : (gravity.col < -64) ? -1 : 0;
	*drow = (gravity.row > 64) ? 1 : (gravity.row < -64) ? -1 : 0;
}

/* -------------------------------------------------------------------------
 * Simulation tick
 * -------------------------------------------------------------------------
 *
 * Classic falling-sand rules, applied in gravity-direction order so fast
 * particles don't teleport through each other:
 *
 *   1. Try primary gravity step (dcol, drow).
 *   2. If blocked, try the two diagonal alternatives:
 *        (dcol ± perp, drow) or (dcol, drow ± perp) depending on gravity axis.
 *      Pick one randomly if both are free to avoid directional bias.
 *   3. If still blocked, particle stays put.
 *
 * Scan order is reversed along the gravity axis so particles cascade
 * without needing a second buffer: when gravity is downward we scan
 * rows bottom-to-top, so a falling particle doesn't immediately move
 * the cell we're about to process.
 */
static void tick(void)
{
	int dcol, drow;

	gravity_step(&dcol, &drow);

	/*
	 * Choose scan direction: iterate against gravity so we process the
	 * destination cells before the source cells, preventing double-moves.
	 */
	int row_start = (drow >= 0) ? LED_ROWS - 1 : 0;
	int row_end   = (drow >= 0) ? -1 : LED_ROWS;
	int row_inc   = (drow >= 0) ? -1 : 1;

	int col_start = (dcol >= 0) ? LED_COLS - 1 : 0;
	int col_end   = (dcol >= 0) ? -1 : LED_COLS;
	int col_inc   = (dcol >= 0) ? -1 : 1;

	for (int row = row_start; row != row_end; row += row_inc) {
		for (int col = col_start; col != col_end; col += col_inc) {
			if (!grid[row][col]) {
				continue;
			}

			int nr = row + drow;
			int nc = col + dcol;

			if (passable(nc, nr)) {
				grid[row][col] = 0;
				grid[nr][nc]   = 1;
				continue;
			}

			/*
			 * Primary path blocked — try the two perpendicular diagonals.
			 * For downward gravity: left-down and right-down.
			 * For sideways gravity: the two vertical diagonals.
			 *
			 * "perp" is the axis perpendicular to gravity.
			 */
			int perp_col = (drow != 0) ? 1 : 0;
			int perp_row = (dcol != 0) ? 1 : 0;

			bool left_free  = passable(nc - perp_col, nr - perp_row);
			bool right_free = passable(nc + perp_col, nr + perp_row);

			int chosen_col = nc;
			int chosen_row = nr;

			if (left_free && right_free) {
				/* Both free — pick randomly to avoid bias */
				if (rand32() & 1) {
					chosen_col -= perp_col;
					chosen_row -= perp_row;
				} else {
					chosen_col += perp_col;
					chosen_row += perp_row;
				}
			} else if (left_free) {
				chosen_col -= perp_col;
				chosen_row -= perp_row;
			} else if (right_free) {
				chosen_col += perp_col;
				chosen_row += perp_row;
			} else {
				/* Fully blocked — stay */
				continue;
			}

			grid[row][col]             = 0;
			grid[chosen_row][chosen_col] = 1;
		}
	}
}

/* Push simulation state into LED_LAYER_SAND and call led_commit(). */
static void render(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);
	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			led_mask[LED_LAYER_SAND][row][col] = grid[row][col];
		}
	}
	k_mutex_unlock(&led_mask_mutex);
	led_commit();
}

/* -------------------------------------------------------------------------
 * 30 Hz thread
 * -------------------------------------------------------------------------*/

#define SAND_STACK_SIZE 1024
#define SAND_PRIORITY   5
#define TICK_MS         33  /* ~30 Hz */

static K_THREAD_STACK_DEFINE(sand_stack, SAND_STACK_SIZE);
static struct k_thread sand_thread_data;

static void sand_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (true) {
		k_mutex_lock(&sand_mutex, K_FOREVER);
		tick();
		render();
		k_mutex_unlock(&sand_mutex);

		k_msleep(TICK_MS);
	}
}

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

void sand_init(void)
{
	memset(grid, 0, sizeof(grid));
	led_mask_clear(LED_LAYER_SAND);

	k_thread_create(&sand_thread_data, sand_stack,
			K_THREAD_STACK_SIZEOF(sand_stack),
			sand_thread, NULL, NULL, NULL,
			SAND_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&sand_thread_data, "sand");

	LOG_INF("Sand simulation started (30 Hz)");
}

void sand_set_gravity(struct sand_gravity g)
{
	k_mutex_lock(&sand_mutex, K_FOREVER);
	gravity = g;
	k_mutex_unlock(&sand_mutex);
}

void sand_add_particles(int n)
{
	k_mutex_lock(&sand_mutex, K_FOREVER);

	int added = 0;

	for (int attempt = 0; attempt < n * 4 && added < n; attempt++) {
		/*
		 * Spawn on the edge that particles enter from — the face
		 * opposite to the gravity direction.
		 */
		int dcol, drow;

		gravity_step(&dcol, &drow);

		int col, row;

		if (abs(drow) >= abs(dcol)) {
			/* Primarily vertical gravity: spawn on top or bottom row */
			col = rand32() % LED_COLS;
			row = (drow > 0) ? 0 : LED_ROWS - 1;
		} else {
			/* Primarily horizontal gravity: spawn on left or right col */
			col = (dcol > 0) ? 0 : LED_COLS - 1;
			row = rand32() % LED_ROWS;
		}

		if (passable(col, row)) {
			grid[row][col] = 1;
			added++;
		}
	}

	k_mutex_unlock(&sand_mutex);
}

void sand_clear(void)
{
	k_mutex_lock(&sand_mutex, K_FOREVER);
	memset(grid, 0, sizeof(grid));
	k_mutex_unlock(&sand_mutex);
}

int sand_count(void)
{
	int count = 0;

	k_mutex_lock(&sand_mutex, K_FOREVER);
	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			count += grid[row][col];
		}
	}
	k_mutex_unlock(&sand_mutex);
	return count;
}

void sand_suspend(void)
{
	k_thread_suspend(&sand_thread_data);
}

void sand_resume(void)
{
	k_thread_resume(&sand_thread_data);
}
