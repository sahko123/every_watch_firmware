# Every Watch — Firmware Plan

## Hardware Summary

**MCU:** nRF52833-QDAA-R7 (ARM Cortex-M4F @ 64MHz, 512KB flash, 128KB RAM)

### I2C Bus — SCL: P0.04, SDA: P0.01

| Device | Address | Role | Interrupt pin |
|---|---|---|---|
| BMI270 | 0x68 | 6-axis IMU, step counter | INT1: P0.30, INT2: P0.31 |
| FRTC8900 | 0x32 | Real-time clock | P0.00 |
| BH1750 | 0x23 | Ambient light sensor | — |
| BQ27441 | 0x55 | Battery fuel gauge | BIN: P0.11, GPOUT: P1.09 |

### LED Matrix — WS2812B, 20×7, 140 LEDs total

| Data line | Pin | Rows | LED count | Wiring pattern |
|---|---|---|---|---|
| Line 1 | P0.29 | 0–1 | 40 | Row 0 left→right (idx 0–19), Row 1 right→left (idx 20–39) |
| Line 2 | P0.28 | 2–3 | 40 | Row 2 left→right (idx 0–19), Row 3 right→left (idx 20–39) |
| Line 3 | P0.02 | 4–5 | 40 | Row 4 left→right (idx 0–19), Row 5 right→left (idx 20–39) |
| Line 4 | P0.03 | 6 | 20 | Row 6 left→right (idx 0–19) |

### Other GPIO

| Pin | Role |
|---|---|
| P0.05 | SGM41524 charge indicator |
| P0.15 | Right button |
| P0.17 | Left button |
| P1.09 | BQ27441 GPOUT (battery threshold alert) |

---

## Development Environment

**SDK:** nRF Connect SDK v2.7.x (Zephyr RTOS)
**Toolchain:** Zephyr SDK (ARM GCC), managed via nRF Connect for Desktop → Toolchain Manager
**IDE:** VS Code + nRF Connect extension pack (configures West, CMake, Ninja automatically)
**Programmer:** J-Link or nRF52840 DK used as programmer over SWD

### Setup Steps
1. Install nRF Connect for Desktop → Toolchain Manager → install nRF Connect SDK v2.7.0
2. Install VS Code + nRF Connect extension pack
3. Install nRF Command Line Tools (nrfjprog)
4. Verify: `west --version` and `arm-none-eabi-gcc --version` both respond

---

## Product Intent

**Context:** Commercial product, sub-100 unit run, explicitly designed to be hackable.

**Hackability principles:**
- Firmware is open source — published source, community can build and modify
- BLE protocol is fully documented so anyone can write a companion app
- No enforced secure boot — MCUboot is present but signing is optional / key is published
- SWD debug pads exposed on hardware for direct flash access
- Companion app: custom app planned but protocol is open — third-party apps (including open source alternatives) are supported by design

**Manufacturing:** A dedicated test build target exercises all peripherals (I2C scan, LED matrix, buttons, BLE advertising) to validate each unit before shipping.

---

## Project Structure

```
every_watch_firmware/
├── boards/
│   └── arm/
│       └── every_watch/
│           ├── every_watch.yaml
│           ├── every_watch_defconfig     ← Kconfig defaults for this board
│           ├── every_watch.dts           ← pin assignments and peripheral nodes
│           └── board.cmake               ← flash/debug runner (J-Link)
├── app/
│   ├── CMakeLists.txt
│   ├── prj.conf                          ← Kconfig options
│   └── src/
│       ├── main.c
│       ├── drivers/
│       │   └── frtc8900/                 ← custom RTC driver (not in Zephyr upstream)
│       ├── led_matrix/
│       │   ├── led_matrix.c              ← unified SPI buffer, parallel DMA commit
│       │   └── led_matrix.h
│       ├── particle_sim/
│       │   ├── particle_sim.c            ← unified physics engine
│       │   └── particle_sim.h
│       ├── sensors/
│       │   ├── imu.c                     ← BMI270 wrapper, gravity projection
│       │   ├── light.c                   ← BH1750 wrapper
│       │   └── battery.c                 ← BQ27441 wrapper
│       ├── ble/
│       │   ├── ble_services.c            ← custom sensor service + DFU
│       │   └── ble_services.h
│       └── ui/
│           ├── buttons.c                 ← debounce, short/long press detection
│           ├── display_sm.c              ← display state machine + notification overlay
│           ├── notification.c            ← notification overlay logic
│           ├── mode_clock.c              ← time reveal mode
│           ├── mode_sand.c               ← sand simulation mode
│           ├── mode_steps.c              ← step counter display
│           ├── mode_battery.c            ← battery status display
│           └── font.c                    ← pixel digit font
└── CMakeLists.txt
```

---

## Phase 1 — Custom Board Definition

Devicetree key nodes:

```dts
&i2c0 {
    compatible = "nordic,nrf-twim";
    sda-pin = <1>;    /* P0.01 */
    scl-pin = <4>;    /* P0.04 */
    clock-frequency = <I2C_BITRATE_FAST>;

    bmi270:  bmi270@68  { compatible = "bosch,bmi270";         reg = <0x68>; };
    frtc8900: frtc8900@32 { compatible = "every-watch,frtc8900"; reg = <0x32>; };
    bh1750:  bh1750@23  { compatible = "rohm,bh1750";          reg = <0x23>; };
    bq27441: bq27441@55 { compatible = "ti,bq274xx";           reg = <0x55>; };
};

/* Four independent SPI masters, one per WS2812B data line */
/* All fired simultaneously via async DMA — see LED matrix section */
&spi0 { compatible = "nordic,nrf-spim"; mosi-pin = <29>; }; /* P0.29 line 1 */
&spi1 { compatible = "nordic,nrf-spim"; mosi-pin = <28>; }; /* P0.28 line 2 */
&spi2 { compatible = "nordic,nrf-spim"; mosi-pin = <2>;  }; /* P0.02 line 3 */
&spi3 { compatible = "nordic,nrf-spim"; mosi-pin = <3>;  }; /* P0.03 line 4 */
```

---

## Phase 2 — Peripheral Bring-up

Each peripheral is verified in isolation via the Zephyr shell before application code is built on top.
Enable `CONFIG_SHELL=y` and `CONFIG_I2C_SHELL=y` for interactive bring-up.

### BMI270 (IMU)
- Zephyr upstream driver: `CONFIG_BMI270=y`
- Bring-up: read device ID, stream accel + gyro via shell
- Configure INT1 (P0.30) → hardware step counter
- Configure INT2 (P0.31) → wrist-tilt wake interrupt

### FRTC8900 (RTC)
- **Not in Zephyr upstream — requires a custom driver**
- Implements the Zephyr `rtc` driver API (`drivers/rtc.h`)
- Bring-up: set time, read back, set 1-minute alarm, verify interrupt on P0.00

### BH1750 (Ambient light)
- Zephyr upstream driver: `CONFIG_BH1750=y`
- Bring-up: read lux value, verify it changes with light/dark
- Used for automatic LED brightness scaling

### BQ27441 (Fuel gauge)
- Zephyr upstream driver: `CONFIG_BQ274XX=y`
- Bring-up: read battery %, voltage (mV), current (mA)
- BIN interrupt (P0.11) → low battery event
- GPOUT (P1.09) → state-of-charge threshold alert

### SGM41524 (Charge detect)
- P0.05 GPIO input, active-low
- Bring-up: plug/unplug USB, verify pin state changes

### Buttons
- P0.15 (right), P0.17 (left) — GPIO inputs with internal pull-ups
- Interrupt-driven with 5ms software debounce
- Short press: released within 400ms
- Long press: held 500ms+ — fires on the hold, not on release
- Use Zephyr `gpio_keys` devicetree binding

### WS2812B LED matrix
- Four SPI peripherals, one per data line
- Bring-up: all LEDs red → green → blue → off, verify each line independently

---

## Phase 3 — LED Matrix Driver

### WS2812B colour depth

WS2812B LEDs are 8 bits per channel (R, G, B), 24 bits per LED total. Data is sent on the wire in GRB order. The natural in-memory representation is a packed struct of three `uint8_t` values:

```c
struct led_rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};
```

### Layered compositing architecture

Display state is split into two kinds of buffer:

**Mask layers** — `uint8_t [7][20]` = 140 bytes each. Stores 0 (empty) or non-zero (particle present). One layer per logical display element. Cheap to have multiple.

**Color layer** — `struct led_rgb [7][20]` = 420 bytes. One packed RGB value per grid position, shared across all mask layers. Defines what color a pixel emits when any mask layer has a particle there.

```
mask_sand[7][20]    (140B) ─┐
mask_digits[7][20]  (140B) ─┤── compositor ──► ws2812_encode() ──► SPI DMA buffers (1,260B)
mask_bg[7][20]      (140B) ─┘       ▲
                                     │
color_layer[7][20]  (420B) ──────────┘
```

The compositor scans every pixel, walks layers in priority order, and on the first non-zero mask reads the RGB value from the color layer at that position:

```c
struct led_rgb composite_pixel(int col, int row) {
    if (mask_digits[row][col]) return color_layer[row][col];
    if (mask_sand[row][col])   return color_layer[row][col];
    if (mask_bg[row][col])     return color_layer[row][col];
    return (struct led_rgb){0, 0, 0};
}
```

The result is passed directly to `ws2812_encode()` which expands the 3 RGB bytes into the 9-byte SPI sequence and writes it into the correct SPI DMA buffer position via `fb_pixel_to_physical()`.

**Color layer variants:** The color layer does not have to be a full 420-byte buffer. A gradient between two or three RGB stops can be computed per-pixel at render time from row/column position — near-zero memory at the cost of trivial CPU. Modes choose whichever fits:

```c
// Computed gradient — zero extra RAM
struct led_rgb gradient_color(int col, int row) {
    float t = (float)row / 6.0f;
    return led_rgb_lerp(&color_top, &color_bottom, t);
}

// Full per-pixel buffer — maximum flexibility
struct led_rgb color_layer[7][20];  // 420 bytes, set freely per pixel
```

**Color layer is animatable independently.** Sweeping a hue shift or gradient across the color layer each frame changes all visible particle colors without touching any mask. Sand grains flowing through a shifting gradient, digit colors pulsing over the hold period — all done by updating only the 420-byte color layer.

**Parallel async DMA:** All four SPIM peripherals are started in rapid succession via `spi_write_async()`. The nRF52833 EasyDMA runs all four in hardware simultaneously. Wall-clock LED update time = longest chain (~1.2ms for 40 LEDs) rather than the sum of all chains (~4.2ms). Do not use Zephyr's blocking `led_strip_update_rgb()`.

### Logical-to-physical pixel mapping

```c
// col: 0–19 left to right, row: 0–6 top to bottom
void fb_pixel_to_physical(int col, int row, int *strip, int *pixel) {
    if (row <= 5) {
        *strip = row / 2;
        *pixel = (row % 2 == 0) ? col : (39 - col);  // snake: odd rows reverse
    } else {
        *strip = 3;
        *pixel = col;  // row 6: single row, straight left→right
    }
}
```

### RAM budget for LED subsystem

| Buffer | Size |
|---|---|
| SPI DMA buf × 3 (40 LEDs × 9 bytes) | 1,080 bytes |
| SPI DMA buf × 1 (20 LEDs × 9 bytes) | 180 bytes |
| Color layer (140 pixels × 3 bytes) | 420 bytes |
| Mask layers × 3 at 140 bytes each | 420 bytes |
| **Total** | **2,100 bytes** |

Each additional mask layer costs 140 bytes. Switching from a full color layer buffer to a computed gradient saves 420 bytes.

WS2812B SPI encoding: each bit → 3 SPI bits at ~3.2MHz, so each LED = 9 bytes. Wire order is GRB — swap R and G at encode time.

### `fb_commit()` — parallel DMA

```c
void fb_commit(void) {
    // All four transfers start in rapid succession; hardware runs them in parallel
    spi_write_async(spi_dev[0], &cfg, &tx[0], cb, (void *)0);
    spi_write_async(spi_dev[1], &cfg, &tx[1], cb, (void *)1);
    spi_write_async(spi_dev[2], &cfg, &tx[2], cb, (void *)2);
    spi_write_async(spi_dev[3], &cfg, &tx[3], cb, (void *)3);
    k_sem_take(&done[0], K_FOREVER);
    k_sem_take(&done[1], K_FOREVER);
    k_sem_take(&done[2], K_FOREVER);
    k_sem_take(&done[3], K_FOREVER);
}
```

---

## Phase 4 — Particle Simulation Engine

### Core principle

One physics engine. Gravity is a swappable 2D vector parameter. The calling mode decides the gravity source — the engine has no knowledge of modes.

```c
struct vec2f {
    float x;  // horizontal component (positive = toward right edge)
    float y;  // vertical component   (positive = toward bottom)
};

// Time reveal: constant, straight down
static const struct vec2f GRAVITY_CONSTANT = { 0.0f, 1.0f };

// Sand mode: live from IMU each frame
struct vec2f gravity_from_imu(void);  // projects BMI270 accel X/Y onto display plane

// Exit animation: strong pull to drain particles off-screen quickly
static const struct vec2f GRAVITY_DRAIN = { 0.0f, 3.0f };
```

### Simulation state

```c
struct particle_sim {
    uint8_t grid[7][20];   // 0 = empty, non-zero = particle present
                           // color is read from the shared color layer at render time
    uint8_t target[7][20]; // digit bitmap — only used in constrained mode
    bool constrained;      // true: particles settle into target shape (time reveal)
                           // false: free physics (sand mode, exit animations)
};
```

**Constrained mode (time reveal):** A falling particle in column `x` stops when it reaches the lowest row where `target[row][x]` is set. Particles in columns with no target pixel fall off the bottom and are removed. This produces the waterfall-settling-into-digits visual.

**Free mode (sand / exit):** Standard cellular automaton — each particle tries to move in the gravity direction, falls to a diagonal neighbour if blocked, stays put if all neighbours in that direction are full. Particles that leave the grid boundary are removed.

### Per-tick flow

```
particle_sim_tick(sim, gravity):
  for each particle in grid (bottom-up traversal to avoid double-move):
    compute desired move direction from gravity vector
    try primary direction → try diagonal left → try diagonal right → stay
    if constrained and reached target position → lock particle in place
    if particle exits grid boundary → remove it
```

---

## Phase 5 — Display State Machine

### Button mapping

| Input | Action |
|---|---|
| Left short press | Trigger time reveal |
| Right short press | Enter sand simulation |
| Left long press | Configurable via BLE app (stored in NVS) |
| Right long press | Configurable via BLE app (stored in NVS) |
| Both buttons held | (reserved) |
| Wrist tilt (BMI270 INT2) | Trigger time reveal (same as left short press) |

Long press actions are stored as action IDs in NVS (default values on first boot). The BLE app writes a new ID to change the behaviour without a firmware update.

### Notification overlay

Notifications are not a display state — they are a compositor overlay that sits on top of whatever state is currently active.

**Display is ON (any active state):** A small colored square appears in a corner of the display (exact corner and color TBD). It is drawn into `mask_notification` and cleared after a short timeout. The underlying mode continues running uninterrupted.

**Display is IDLE (LEDs off):** The notification wakes the display, shows the corner flash, then returns to IDLE when the notification timeout expires. This uses a lightweight `NOTIFICATION_WAKE` path that bypasses the full mode initialisation.

The notification color and size are simple and fixed for now — one color, one size (approximately 2×2 pixels). Per-type color coding is deferred until the companion app design is clearer.

BLE: phone writes to the notification characteristic → firmware sets `mask_notification` pixels and starts the timeout timer.

### Display states

```
[IDLE]
  LEDs off, CPU in deep sleep
  Woken by: button GPIO interrupt, BMI270 wrist-tilt interrupt (P0.31), BLE event

  left press / wrist tilt ──► TIME_REVEAL
  right press ──────────────► SAND_SIM
  BLE notification ─────────► NOTIFICATION_WAKE

[NOTIFICATION_WAKE]
  Briefly wakes display from IDLE to show the notification corner flash only
  ──► IDLE when notification timeout expires
  ──► TIME_REVEAL if left press / wrist tilt arrives while showing

[TIME_REVEAL]
  Particle sim: constrained=true, target=current time digits, gravity=GRAVITY_CONSTANT
  Particles spawn at top of each column that has a target pixel
  Notification overlay may appear simultaneously in corner without interrupting
  ──► TIME_HOLD when all target pixels are filled

[TIME_HOLD]
  Display frozen showing the time
  Countdown timer starts (duration configurable in NVS, default 5 seconds)
  ──► TIME_EXIT on timer expiry or left button press

[TIME_EXIT]
  Particle sim: constrained=false, gravity=GRAVITY_DRAIN
  Particles drain off the bottom of the display
  ──► IDLE when grid is empty

[SAND_SIM]
  Particle sim: constrained=false, gravity=gravity_from_imu() updated each frame
  Notification overlay may appear simultaneously in corner
  Runs until user exits
  ──► SAND_EXIT on right button press

[SAND_EXIT]
  Particle sim: constrained=false, gravity=GRAVITY_DRAIN
  Particles drain off the bottom
  ──► IDLE when grid is empty
```

### Per-frame tick (runs only when display is active)

```
1. Read BMI270 (accel for gravity, step count)
2. Update gravity source based on current state
3. particle_sim_tick()
4. Check state transition conditions
5. Composite all mask layers + color layer → SPI buffer
     - mask_notification checked last (highest priority, draws over everything)
6. fb_commit() — parallel DMA
7. Sleep until next frame tick (~33ms for 30fps)
```

When state returns to IDLE: stop frame timer, clear all masks, `fb_commit()` (all LEDs off), then allow CPU to deep-sleep.

---

## Phase 6 — BLE Services

**Advertising:** Device name `"EveryWatch"`. Slow advertising interval (~1s) when idle, fast (~200ms) for 30s after wrist tilt or button press.

**Open protocol:** The BLE service layout is publicly documented so third-party and open source apps can connect. No proprietary lock-in.

**Time sync:** The watch requests the current time automatically on every phone connection. No manual sync step.

### Custom sensor service (define project UUID)

| Characteristic | Properties | Payload |
|---|---|---|
| Current time | Read + Write | Unix timestamp — watch requests on connect, phone writes to sync FRTC8900 |
| Time format | Read + Write | uint8: 0 = 24hr (default), 1 = 12hr — persisted in NVS |
| Step count | Read + Notify | uint32 — daily total, reset at midnight |
| Battery level | Read + Notify | uint8 (%) |
| Ambient light | Read | uint16 (lux) |
| Time display duration | Read + Write | uint16 (seconds, default 5) — persisted in NVS |
| Left long press action | Read + Write | uint8 (action ID) — persisted in NVS |
| Right long press action | Read + Write | uint8 (action ID) — persisted in NVS |
| Notification trigger | Write | uint8 notification type (simple for now — single type, color TBD) |

All writable characteristics persist to NVS on change.

**Standard Battery Service:** `CONFIG_BT_BAS=y` — compatible with standard phone battery widgets.

### OTA DFU

MCUboot bootloader + MCUmgr SMP over BLE:
- `CONFIG_BOOTLOADER_MCUBOOT=y`
- `CONFIG_MCUMGR=y`
- `CONFIG_MCUMGR_TRANSPORT_BT=y`
- Phone app: **nRF Connect** (iOS/Android) — upload `.bin` file, or any MCUmgr-compatible client
- No enforced image signing — MCUboot present but signing is optional in line with the hackable product philosophy
- App must call `boot_write_img_confirmed()` on first successful boot to prevent automatic rollback

---

## Phase 7 — Power Management

### Target behaviour
- CPU deep-sleeps between events
- Display (LEDs) only on during active states — never on in IDLE
- Frame loop timer only runs while display is active

### Sleep architecture

| State | CPU | LEDs | Approx. current |
|---|---|---|---|
| IDLE, no BLE | Deep sleep | Off | < 10µA |
| IDLE, BLE advertising (slow) | Deep sleep between adv | Off | ~50µA avg |
| Active display, no BLE | Awake at 30fps | On (brightness-capped) | ~5–30mA |
| Charging | Shallow sleep | Off (or charge anim) | USB-powered |

### Peripheral power strategy
- **BH1750:** sampled every 30s, suspended between reads via `PM_DEVICE_ACTION_SUSPEND`
- **BMI270:** low-power accelerometer mode between display events; wrist-tilt interrupt wakes system
- **WS2812B:** zero current when not transmitting (no hold line); driven only during `fb_commit()`
- **BLE:** advertising stops on long inactivity, restarts on button/tilt

### Brightness cap
WS2812B at full white = ~60mA per LED × 140 LEDs = 8.4A theoretical maximum.
Apply a global brightness scale (0–255) enforced in `fb_set_pixel()`. Default cap: ~15% (~100mA total). Configurable via BLE. BH1750 lux reading further scales brightness down in low-light conditions.

---

## Phase 8 — MCUboot & Flash Partitions

512KB flash layout (managed by Zephyr partition manager):

| Region | Size | Notes |
|---|---|---|
| MCUboot | ~32KB | Bootloader, validates and swaps images |
| App primary slot | ~220KB | Running image |
| App secondary slot | ~220KB | DFU upload target |
| NVS (settings) | ~8KB | Persistent config (time display duration, long press actions, etc.) |
| **Total** | **480KB / 512KB** | |

Sign firmware images with `imgtool` before flashing in production.

---

## RAM Budget

| Region | Size |
|---|---|
| Zephyr kernel + stacks | ~20KB |
| BLE stack | ~35KB |
| MCUmgr (DFU) | ~8KB |
| SPI DMA buffers (LED matrix) | ~1.3KB |
| Color layer (140 pixels × 3 bytes) | ~0.4KB |
| Mask layers × 4 (sand, digits, bg, notification) | ~0.6KB |
| Particle simulation grids (grid + target) | ~0.3KB |
| Sensor buffers, app state | ~10KB |
| **Total estimated** | **~76KB / 128KB** |

---

## Implementation Sequence

| Phase | Deliverable | Risk |
|---|---|---|
| 0 | Toolchain installed, board file boots, UART shell works | Low |
| 1 | I2C scan confirms all 4 devices ACK | Low |
| 2 | FRTC8900 custom driver — set/read time, alarm fires | **High** (no upstream driver) |
| 3 | BMI270: accel stream, step count, wrist-tilt interrupt | Medium |
| 4 | BH1750 + BQ27441 readable, buttons register short/long press | Low |
| 5 | LED matrix: all lines light up, correct pixel mapping verified | Medium |
| 6 | Layered compositor + parallel DMA commit, frame loop at 30fps | Medium |
| 7 | Particle engine: free sand mode working with constant gravity | Low |
| 8 | Constrained mode: time digits revealed by waterfall animation | Medium |
| 9 | IMU gravity fed into sand mode, tilt changes fall direction | Low |
| 10 | Display state machine: all transitions, wrist-tilt wake | Low |
| 11 | Step counter mode + battery status mode | Low |
| 12 | Notification overlay: corner flash from BLE write, wake from idle | Low |
| 13 | BLE: connect from phone, auto time sync, read sensor data, write config | Medium |
| 14 | NVS persistence for all writable characteristics | Low |
| 15 | Power profiling + optimization, sleep modes | Medium |
| 16 | MCUboot + OTA DFU tested end-to-end | Medium |
| 17 | Manufacturing test build: exercises all peripherals, reports pass/fail over UART | Low |

**Highest risk items:** FRTC8900 custom driver (no datasheet link yet), flash partition sizing with BLE + MCUmgr + MCUboot all enabled simultaneously.
