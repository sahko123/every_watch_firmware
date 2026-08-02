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
 * Digit cells are NOT obstacles: sand flows freely over LED_LAYER_DIGITS and
 * the digits are revealed underneath as particles clear, because the
 * compositor draws the sand layer above the digit layer.
 */

static uint8_t grid[LED_ROWS][LED_COLS]; /* 1 = particle, 0 = empty */

/*
 * Per-grain hue, parallel to grid[] and moved with the particle.
 *
 * led_color[] is indexed by cell, not by particle, so writing colours there
 * directly would leave a fixed colour field with grains sliding through it —
 * a grain would change colour as it fell. Carrying the hue alongside the
 * particle and only converting to RGB at publish time keeps each grain its
 * own colour for its whole life. 140 bytes.
 *
 * Only meaningful where grid[][] is set; stale entries in empty cells are
 * harmless and get overwritten when a particle next lands there.
 */
static uint8_t grain_hue[LED_ROWS][LED_COLS];

/* True once sand_fill_random() has seeded coloured grains, so publish_mask()
 * knows to paint per-grain colour rather than leaving led_color[] to whatever
 * the rest of the app last set (normally the flat amber from main.c). */
static bool grain_colored;

/*
 * Simulation mode.
 *
 *   SAND_FREE        normal falling sand, gravity from the accelerometer
 *   SAND_CONSTRAINED particles lock into target[] — fills a shape bottom-up
 *   SAND_RAIN        a curtain sweeping top to bottom, density ramping up to
 *                    full and back down, used for the time reveal
 *
 * Both reveal modes ignore the gravity vector and run straight down, so they
 * look identical however the watch is held.
 */
enum sand_mode {
	SAND_FREE = 0,
	SAND_CONSTRAINED,
	SAND_RAIN,
};

static enum sand_mode mode;

/* SAND_CONSTRAINED: the shape being filled. */
static uint8_t target[LED_ROWS][LED_COLS];

/* SAND_RAIN: density envelope, in ticks at TICK_MS.
 *
 * Particles descend one row every RAIN_FALL_DIVIDER ticks, so the screen takes
 * (LED_ROWS * RAIN_FALL_DIVIDER) ticks to traverse. Each tick every column is
 * offered a new particle with probability rain_density/255, so the density the
 * eye sees at any height is the envelope value from when that row was spawned —
 * the ramp is literally travelling down the display.
 *
 * The hold is longer than one traversal so the screen reaches complete cover,
 * which is the moment the digits are put in place underneath. */
#define RAIN_FALL_DIVIDER  2     /* 15 rows/s at 30 Hz -> ~470 ms to cross */
#define RAIN_RAMP_TICKS   21     /* ~0.7 s fading in, and again fading out */
#define RAIN_HOLD_TICKS   16     /* ~0.5 s at full density */

static uint16_t rain_tick;
static uint8_t  rain_density;
static bool     rain_peak_fired;
static bool     rain_peak_pending;
static bool     rain_done_pending;

static void (*rain_on_peak)(void);
static void (*rain_on_done)(void);

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
				/* Hue travels with the grain — see grain_hue[]. */
				grain_hue[nr][nc] = grain_hue[row][col];
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
			grain_hue[chosen_row][chosen_col] = grain_hue[row][col];
		}
	}
}

/* -------------------------------------------------------------------------
 * Constrained tick — the time reveal
 * -------------------------------------------------------------------------*/

/* Lowest cell of the target still empty in this column, or -1 if the column
 * has nothing left to fill. Particles home to this row and lock there, so the
 * shape fills from the bottom up. */
static int landing_row(int col)
{
	for (int row = LED_ROWS - 1; row >= 0; row--) {
		if (target[row][col] && !grid[row][col]) {
			return row;
		}
	}
	return -1;
}

/* Keep the waterfall flowing: top up row 0 for any column still being filled.
 * The random gate staggers the columns — dropping every column on every tick
 * looks like a descending bar rather than falling sand. */
static void spawn_stream(void)
{
	for (int col = 0; col < LED_COLS; col++) {
		if (grid[0][col] || landing_row(col) < 0) {
			continue;
		}
		if ((rand32() & 3) == 0) {
			grid[0][col] = 1;
		}
	}
}

static void tick_constrained(void)
{
	spawn_stream();

	/* Bottom-up so a particle cannot be moved twice in one tick: once it
	 * steps into row+1 that row has already been processed. */
	for (int row = LED_ROWS - 1; row >= 0; row--) {
		for (int col = 0; col < LED_COLS; col++) {
			if (!grid[row][col]) {
				continue;
			}

			/* Look for the landing slot with this particle taken out
			 * of the grid, otherwise one already sitting on its own
			 * target would see the slot as occupied and drift off. */
			grid[row][col] = 0;
			int land = landing_row(col);

			grid[row][col] = 1;

			if (land == row) {
				continue;              /* locked into the shape */
			}

			if (row + 1 >= LED_ROWS) {
				grid[row][col] = 0;    /* nothing to fill: drains away */
				continue;
			}

			if (!grid[row + 1][col]) {
				grid[row][col] = 0;
				grid[row + 1][col] = 1;
			}
			/* else blocked — wait for the one below to settle */
		}
	}
}

/* -------------------------------------------------------------------------
 * Rain tick — the curtain reveal
 * -------------------------------------------------------------------------*/

/* Density envelope: fade in, hold at full, fade out, then let the last of it
 * fall clear. Returns false once the whole animation is finished. */
static bool rain_step_envelope(void)
{
	uint16_t t = rain_tick++;

	if (t < RAIN_RAMP_TICKS) {
		rain_density = (uint8_t)((t * 255u) / RAIN_RAMP_TICKS);
		return true;
	}
	t -= RAIN_RAMP_TICKS;

	if (t < RAIN_HOLD_TICKS) {
		rain_density = 255;

		/* Full cover: hand the digits over to be placed underneath the
		 * sand, so they are already there when the curtain thins out. */
		if (!rain_peak_fired) {
			rain_peak_fired   = true;
			rain_peak_pending = true;
		}
		return true;
	}
	t -= RAIN_HOLD_TICKS;

	if (t < RAIN_RAMP_TICKS) {
		rain_density = (uint8_t)(255u - (t * 255u) / RAIN_RAMP_TICKS);
		return true;
	}

	/* Envelope over. Keep falling until the last particles have left. */
	rain_density = 0;

	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			if (grid[row][col]) {
				return true;
			}
		}
	}
	return false;
}

static void tick_rain(void)
{
	if (!rain_step_envelope()) {
		mode = SAND_FREE;
		rain_done_pending = true;
		return;
	}

	/* Advance the curtain on every Nth tick so it descends at a readable
	 * speed rather than crossing the display in a fifth of a second. */
	if ((rain_tick % RAIN_FALL_DIVIDER) != 0) {
		return;
	}

	/* Everything moves down one row in lockstep — this is a curtain, not a
	 * pile, so the bottom row leaves the display rather than accumulating. */
	for (int col = 0; col < LED_COLS; col++) {
		grid[LED_ROWS - 1][col] = 0;
	}
	for (int row = LED_ROWS - 2; row >= 0; row--) {
		for (int col = 0; col < LED_COLS; col++) {
			grid[row + 1][col] = grid[row][col];
			grid[row][col] = 0;
		}
	}

	/* Feed the top row at the current density. */
	for (int col = 0; col < LED_COLS; col++) {
		grid[0][col] = ((rand32() & 0xFF) < rain_density) ? 1 : 0;
	}
}

/* Copy simulation state into LED_LAYER_SAND.
 * Caller holds sand_mutex; this takes led_mask_mutex for the copy.
 * Does NOT commit — see sand_thread() for why that is kept separate. */
/* Colour the curtain. Hue runs down the display and drifts sideways and with
 * time, so the falling sheet is a moving rainbow rather than a flat wash.
 * Writes led_color[], so the caller must hold led_mask_mutex — the compositor
 * reads it under that lock. */
static void paint_rainbow(void)
{
	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			uint8_t hue = (uint8_t)(row * 26 + col * 5 + rain_tick * 3);

			led_color[row][col] = led_color_wheel(hue);
		}
	}
}

/* Paint each occupied cell from its grain's own hue. Same locking rule as
 * paint_rainbow() — writes led_color[], so the caller must hold
 * led_mask_mutex. Empty cells are left alone: the sand mask is zero there so
 * the compositor never reads them. */
static void paint_grains(void)
{
	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			if (grid[row][col]) {
				led_color[row][col] =
					led_color_wheel(grain_hue[row][col]);
			}
		}
	}
}

static void publish_mask(void)
{
	k_mutex_lock(&led_mask_mutex, K_FOREVER);

	if (mode == SAND_RAIN) {
		paint_rainbow();
	} else if (grain_colored) {
		paint_grains();
	}

	for (int row = 0; row < LED_ROWS; row++) {
		for (int col = 0; col < LED_COLS; col++) {
			led_mask[LED_LAYER_SAND][row][col] = grid[row][col];
		}
	}
	k_mutex_unlock(&led_mask_mutex);
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
		switch (mode) {
		case SAND_CONSTRAINED:
			tick_constrained();
			break;
		case SAND_RAIN:
			tick_rain();
			break;
		default:
			tick();
			break;
		}
		publish_mask();

		/* Latch the callbacks and fire them after the mutex is released:
		 * they reach into the LED layers, and holding two locks across a
		 * caller-supplied function is how deadlocks get written. */
		bool peak = rain_peak_pending;
		bool done = rain_done_pending;

		rain_peak_pending = false;
		rain_done_pending = false;
		void (*cb_peak)(void) = rain_on_peak;
		void (*cb_done)(void) = rain_on_done;

		k_mutex_unlock(&sand_mutex);

		if (peak && cb_peak) {
			cb_peak();
		}
		if (done && cb_done) {
			cb_done();
		}

		/* led_commit() blocks ~3 ms in DMA and is deliberately called
		 * with sand_mutex released. display_off() suspends this thread,
		 * and being suspended while holding sand_mutex would block every
		 * caller of sand_set_gravity(), sand_count() and sand_clear()
		 * until the display came back on. */
		led_commit();

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

	int dcol, drow;

	gravity_step(&dcol, &drow);

	/* Direction to walk when looking for somewhere to put a particle. With
	 * no meaningful gravity, fall back to downward so the search always
	 * makes progress and cannot spin. */
	int step_col = (dcol > 0) ? 1 : (dcol < 0) ? -1 : 0;
	int step_row = (drow > 0) ? 1 : (drow < 0) ? -1 : 0;

	if (step_col == 0 && step_row == 0) {
		step_row = 1;
	}

	int added = 0;

	for (int attempt = 0; attempt < n * 4 && added < n; attempt++) {
		/*
		 * Spawn on the edge that particles enter from — the face
		 * opposite to the gravity direction.
		 */
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

		/*
		 * Walk along the gravity direction to the first free cell.
		 *
		 * Placing only on the edge itself caps the total at the width of
		 * that edge — 20 cells for vertical gravity — because once the
		 * edge fills, every later attempt fails and the request silently
		 * under-delivers. sand_add_particles(60) used to add exactly 20,
		 * which then settled into precisely one full row.
		 */
		while (in_bounds(col, row) && grid[row][col]) {
			col += step_col;
			row += step_row;
		}

		if (in_bounds(col, row)) {
			grid[row][col] = 1;
			added++;
		}
	}

	if (added < n) {
		LOG_WRN("sand: asked for %d particles, grid had room for %d",
			n, added);
	}

	k_mutex_unlock(&sand_mutex);
}

void sand_set_target(const uint8_t t[LED_ROWS][LED_COLS])
{
	k_mutex_lock(&sand_mutex, K_FOREVER);

	if (t != NULL) {
		memcpy(target, t, sizeof(target));
		mode = SAND_CONSTRAINED;
	} else {
		memset(target, 0, sizeof(target));
		mode = SAND_FREE;
	}

	k_mutex_unlock(&sand_mutex);
}

void sand_rain_start(void (*on_peak)(void), void (*on_done)(void))
{
	k_mutex_lock(&sand_mutex, K_FOREVER);

	memset(grid, 0, sizeof(grid));
	rain_tick         = 0;
	rain_density      = 0;
	rain_peak_fired   = false;
	rain_peak_pending = false;
	rain_done_pending = false;
	rain_on_peak      = on_peak;
	rain_on_done      = on_done;
	mode              = SAND_RAIN;

	k_mutex_unlock(&sand_mutex);
}

bool sand_rain_active(void)
{
	bool active;

	k_mutex_lock(&sand_mutex, K_FOREVER);
	active = (mode == SAND_RAIN);
	k_mutex_unlock(&sand_mutex);

	return active;
}

bool sand_target_complete(void)
{
	bool done = true;

	k_mutex_lock(&sand_mutex, K_FOREVER);

	if (mode != SAND_CONSTRAINED) {
		done = false;
	} else {
		for (int row = 0; row < LED_ROWS && done; row++) {
			for (int col = 0; col < LED_COLS; col++) {
				if (target[row][col] && !grid[row][col]) {
					done = false;
					break;
				}
			}
		}
	}

	k_mutex_unlock(&sand_mutex);
	return done;
}

void sand_fill_random(uint8_t percent)
{
	percent = MIN(percent, SAND_FILL_MAX_PCT);

	k_mutex_lock(&sand_mutex, K_FOREVER);

	memset(grid, 0, sizeof(grid));

	int target_count = (LED_ROWS * LED_COLS * percent) / 100;
	int placed = 0;

	/*
	 * Rejection sampling, but with a hard attempt cap rather than looping
	 * until the target is met. At the fill levels this is used for, the
	 * cap is never reached — but an uncapped "keep trying until placed ==
	 * target" loop would spin forever if the target ever exceeded grid
	 * capacity, and this runs with sand_mutex held on a system with a
	 * watchdog. Cheap insurance against a future caller passing something
	 * silly. Under-filling slightly is invisible; hanging is not.
	 */
	for (int attempt = 0; attempt < target_count * 8 && placed < target_count;
	     attempt++) {
		int row = rand32() % LED_ROWS;
		int col = rand32() % LED_COLS;

		if (!grid[row][col]) {
			grid[row][col]      = 1;
			grain_hue[row][col] = (uint8_t)rand32();
			placed++;
		}
	}

	grain_colored = true;
	mode          = SAND_FREE;

	k_mutex_unlock(&sand_mutex);

	LOG_INF("Sand: seeded %d/%d cells (%u%%), random hues",
		placed, LED_ROWS * LED_COLS, percent);
}

void sand_clear(void)
{
	k_mutex_lock(&sand_mutex, K_FOREVER);
	memset(grid, 0, sizeof(grid));
	/* Drop back to whatever colour the rest of the app is using; leaving
	 * this set would keep repainting led_color[] from stale hues. */
	grain_colored = false;
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
	/* Take sand_mutex first so the thread can only be frozen between
	 * ticks, never mid-critical-section holding it. Without this, a
	 * caller elsewhere (e.g. battery.c's level_show_for() -> sand_clear())
	 * that tries to lock sand_mutex while the suspended thread is still
	 * holding it would block forever — display_on()/sand_resume() being
	 * the only thing that could ever unblock it creates a real deadlock
	 * path, not just a lock-ordering nicety. Mirrors display.c's own
	 * led_commit_mutex handshake for the same reason. */
	k_mutex_lock(&sand_mutex, K_FOREVER);
	k_thread_suspend(&sand_thread_data);
	k_mutex_unlock(&sand_mutex);
}

void sand_resume(void)
{
	k_thread_resume(&sand_thread_data);
}

/* --------------------------------------------------------------------------
 * Shell commands — drive the sand simulation from the bench
 *
 * Free-mode sand has no trigger of its own in the application (the reveal
 * modes are what normally put particles on screen), so without these there is
 * no way to actually look at it on hardware.
 * -------------------------------------------------------------------------- */

#ifdef CONFIG_SHELL

#include <zephyr/shell/shell.h>
#include "display/display.h"

static int cmd_sand_fill(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t pct = 25;

	if (argc == 2) {
		pct = (uint8_t)strtoul(argv[1], NULL, 0);
	}

	/* Wake the display first: the sand thread is suspended while it is
	 * off, so seeding without this fills a grid nothing is ticking. */
	display_on();
	sand_fill_random(pct);

	shell_print(sh, "seeded ~%u%% of %d cells with random hues; "
			"tilt the watch to make it flow",
		    MIN(pct, SAND_FILL_MAX_PCT), LED_ROWS * LED_COLS);
	return 0;
}

static int cmd_sand_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);

	sand_clear();
	shell_print(sh, "grid cleared");
	return 0;
}

static int cmd_sand_count(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);

	int n = sand_count();

	shell_print(sh, "%d / %d cells occupied (%d%%)",
		    n, LED_ROWS * LED_COLS, (n * 100) / (LED_ROWS * LED_COLS));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sand_sub,
	SHELL_CMD_ARG(fill,  NULL, "Seed random coloured grains: sand fill [percent]",
		      cmd_sand_fill,  1, 1),
	SHELL_CMD_ARG(clear, NULL, "Remove all grains",  cmd_sand_clear, 1, 0),
	SHELL_CMD_ARG(count, NULL, "How many grains are live", cmd_sand_count, 1, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(sand, &sand_sub, "Sand simulation", NULL);

#endif /* CONFIG_SHELL */
