# Every Watch — firmware

Zephyr firmware for the Every Watch: an nRF52833 wearable with a 20×7 WS2812B
matrix, a falling-sand simulation that reveals the time as particles clear, and
a BLE proximity-encounter system.

Open by design — build it, modify it, flash it. The BLE protocol is documented
so anyone can write a companion app, and images are unsigned so you don't need
our keys.

See [FIRMWARE_PLAN.md](FIRMWARE_PLAN.md) for hardware detail and architecture.

---

## Toolchain

nRF Connect SDK **v2.7.0**. The Nordic toolchain bundle ships its own Python,
CMake, Ninja and ARM GCC, so nothing else needs installing.

```bash
# 1. nRF Util (~6 MB) — put it somewhere on PATH
curl -L -o nrfutil.exe https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/x86_64-pc-windows-msvc/nrfutil.exe

# 2. Toolchain bundle (~2 GB)
nrfutil install toolchain-manager
nrfutil toolchain-manager install --ncs-version v2.7.0
```

## Workspace

This repository *is* the west manifest repo, so it has to live inside the
workspace rather than the other way round:

```bash
west init -m https://github.com/sahko123/every_watch_firmware --mr master C:/ewdev
cd C:/ewdev
west update            # ~5 GB, 30-60 min
```

> **Use a short, space-free path.** Zephyr's CMake does not survive paths
> containing spaces or `!`. On Windows, `C:\ewdev` plus a junction
> (`mklink /J W:\ew C:\ewdev`) works well if you want it visible elsewhere.

Every `west` command below runs inside the toolchain environment:

```bash
nrfutil toolchain-manager launch --ncs-version v2.7.0 -- west build ...
```

## Building

Run these from the workspace root (`C:/ewdev`).

**Production image** — no console, logging at ERROR, `-Os`:

```bash
west build -b every_watch/nrf52833 every_watch_firmware -p always
```

**Bring-up image, RTT console** — use this for first power-on. Adds the
hardware self-test, full logging and a Zephyr shell, over SEGGER RTT on the SWD
pads. RTT is alive from the first instruction, so it catches failures that
happen long before USB could enumerate:

```bash
west build -b every_watch/nrf52833 every_watch_firmware -p always -d build_bringup \
    -- -DEXTRA_CONF_FILE=bringup.conf
```

**Bring-up image, USB CDC console** — same thing with the console on the USB
port as a virtual COM port, for once the board is known good and you no longer
want a debugger attached:

```bash
west build -b every_watch/nrf52833 every_watch_firmware -p always -d build_usb \
    -- -DEXTRA_CONF_FILE=bringup_usb.conf -DEXTRA_DTC_OVERLAY_FILE=bringup_usb.overlay
```

Approximate sizes (slot is 450,048 B):

| Build | Flash | RAM |
|---|---:|---:|
| Production | 229 KB (51%) | 38 KB (29%) |
| Bring-up, RTT | 353 KB (78%) | 52 KB (40%) |
| Bring-up, USB CDC | 373 KB (83%) | 59 KB (45%) |

> Do **not** pass `--sysbuild`. This app builds through the legacy child-image
> path, and `sysbuild/mcuboot.conf` is stale — it is missing the single-slot and
> USB DFU settings that `pm_static.yml` and the in-app DFU trigger depend on.

**Stage 0** — the minimal first-power-on image. No sensor drivers, no RTC
driver, no SPI, no BLE, no LED matrix: nothing that can hang during driver init
on an unproven board. Scans the I2C bus, reads the GPIOs, and drops you at a
shell. Flash this first on a board that has never been powered:

```bash
west build -b every_watch/nrf52833 every_watch_firmware -p always -d build_stage0 \
    -- -DCONF_FILE=stage0.conf -DEXTRA_DTC_OVERLAY_FILE=stage0.overlay
```

## Flashing

**The first flash must be over SWD.** USB DFU is provided *by* MCUboot, so
MCUboot has to be on the chip before USB flashing can work at all. The same
applies to any later change to MCUboot itself — a bootloader cannot replace
itself over the update path it provides.

**SWD is still required for recovery.** There is no reset pin and no
user-accessible power cycle, so "hold the button and apply power" needs the
battery disconnected. If the app is invalid, MCUboot hits `FIH_PANIC` and halts
with no watchdog to restart it. Closing this needs MCUboot to start the
watchdog before chainloading, which does not fit until the RSA-2048 test-key
signature comes out — the bootloader is at 97% of its 48 KB partition.

### With a Raspberry Pi Pico (no commercial debugger needed)

Flash the Pico with Raspberry Pi's `debugprobe` firmware — note the file
differs by board, and `debugprobe.uf2` is for the dedicated Debug Probe
hardware, *not* a plain Pico:

| Board | Asset |
|---|---|
| Pico / Pico W (RP2040) | `debugprobe_on_pico.uf2` |
| Pico 2 / Pico 2 W (RP2350) | `debugprobe_on_pico2.uf2` |
| RPi Debug Probe | `debugprobe.uf2` |

From <https://github.com/raspberrypi/debugprobe/releases>. Hold BOOTSEL while
plugging the Pico in, then drag the `.uf2` onto the `RPI-RP2` drive.

Wiring — the three pins are adjacent on the Pico header:

| Pico pin | GPIO | Watch |
|---|---|---|
| 3 | GND | GND |
| 4 | GP2 | SWCLK |
| 5 | GP3 | SWDIO |

Do **not** power the watch from the Pico — let it run from its own battery or
USB and share only ground. Keep the SWD leads short; long flying leads are the
usual cause of intermittent `cannot read IDR`.

Check the probe can reach the chip before writing anything to it:

```bash
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init; targets; exit"
```

You want `nrf52.cpu` listed and `halted`.

Install OpenOCD (0.11 or newer), then:

```bash
west flash                      # openocd is the default runner for this board
```

If the chip refuses to connect, it may have readback protection enabled from
the factory. Recover it once with:

```bash
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "init; nrf52_recover; exit"
```

### With a J-Link or nRF52840 DK

```bash
west flash -r jlink
```

### Once MCUboot is on the chip

```bash
west flash -r dfu-util
```

**After that, updates go over USB.** Hold the **left button for 3 seconds** —
the app reboots into MCUboot — and **keep holding through the reset**. MCUboot
samples the button before booting the app and stays in USB DFU indefinitely, so
there is no window to race. Release once it enumerates.

```bash
dfu-util -d 2fe3:ffff -a 0 -D build/zephyr/app_update.bin
```

This works on battery power too: plug USB in first, then hold the button.

To recover a watch whose app will not boot, hold the left button and *then*
apply power. The pin is sampled before the image is validated, so a corrupt app
cannot lock you out of the bootloader.

### Windows: two Zadig installs, not one

The USB DFU class has no built-in Windows driver (`CONFIG_USB_DEVICE_OS_DESC`
is off and Zephyr's `usb_dfu.c` has no MS OS descriptor support, so WinUSB
cannot auto-bind). Run Zadig as administrator, tick *Options -> List All
Devices*, and bind **WinUSB** to **both** of these:

| USB ID        | When it appears                                    |
|---------------|----------------------------------------------------|
| `2FE3:0100`   | Runtime mode — how MCUboot first enumerates        |
| `2FE3:FFFF`   | DFU mode — after dfu-util sends its detach request |

Zephyr changes the product ID when it switches into DFU mode, so the second is
a hardware ID Windows has never seen and needs its own driver. Miss it and
dfu-util reports `LIBUSB_ERROR_NOT_SUPPORTED` / `Lost device after RESET?`
immediately after `Device will detach and reattach...`.

Target `2fe3:ffff` directly when the device is already in DFU mode. Passing
`-d 2fe3:0100` makes dfu-util issue a detach and then lose the device across
the re-enumeration, because it does not follow the PID change.

## Watching the console

Logs are on RTT buffer 0, the interactive shell on buffer 1.

With OpenOCD (Pico), start an RTT server and telnet to it:

```bash
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
        -c "init; rtt setup 0x20000000 0x20000 \"SEGGER RTT\"; rtt start; rtt server start 9090 0; rtt server start 9091 1"
# then, in another terminal
telnet localhost 9090     # logs
telnet localhost 9091     # shell
```

With a J-Link:

```bash
JLinkRTTViewer            # or: JLinkRTTClient
```

USB CDC build: open the virtual COM port with any terminal at any baud rate
(CDC ignores it).

## First bring-up

Flash the RTT bring-up image over SWD and watch the console. `CONFIG_EW_SELFTEST`
runs before any application thread starts and reports PASS/FAIL for:

- **I2C** — full bus scan, then a targeted probe of all four expected devices
  (0x23 BH1750, 0x32 FRTC8900, 0x55 BQ27441, 0x68 BMI270)
- **RTC** — set a known time, read it back, confirm seconds advance. This is the
  riskiest part of the system: the FRTC8900 driver is custom, out-of-tree, and
  has never run on hardware
- **IMU** — fetch accel, print all three axes, sanity-check the gravity vector
  magnitude
- **Light / battery** — read lux, voltage and state of charge
- **GPIO** — charge indicator state
- **LEDs** — each data line in red/green/blue, then a single-pixel walk across
  the whole grid. *Watch this one.* A dark line means a bad solder joint on that
  pin; a row that runs backwards means the snake mapping in
  `pixel_to_physical()` is wrong; wrong colours mean the GRB swap is wrong
- **Buttons** — prompts for a left then a right press

The self-test **sets the RTC to a fixed date** as part of testing it. Re-sync
over BLE afterwards.

Set `CONFIG_EW_SELFTEST_HALT_ON_FAIL=y` to stop before starting the application
when any check fails — intended for an end-of-line manufacturing test rather
than bring-up.

### Known open items

Things to check with a scope or meter on first hardware, all flagged in the
source:

- **~~WS2812B T1H marginally out of spec~~ — resolved.** The LED matrix moved
  from SPI to PWM0-3 + EasyDMA (see `led_matrix.c`); T1H is now 812.5 ns,
  comfortably mid-spec (datasheet 800 ns ±150). Left here as a record in case
  the driver ever regresses, not as an open item.
- **~~Row 6 locks interrupts for ~585 µs per frame~~ — resolved.** Row 6 now
  runs on PWM3 (P0.03) alongside the other three lines, same as the item
  above — no more bit-banging, no more interrupt lock.
- **BMI270 IMU fails its chip-ID check on the current bench unit.** Consistent
  on repeated resets (reads chip ID 0x27, expects 0x24) — suspected signal
  integrity issue given this board has no external I2C pull-ups, but not
  confirmed. Wrist-tilt wake and sand-mode gravity are both non-functional
  until this is resolved. Not caused by firmware — the fault happens at
  driver-init time before any application code runs.
- **LFXO not confirmed.** The DTS assumes the internal RC oscillator. If a
  32.768 kHz crystal is populated, enable it (`&clock { lf-clk-src = <1>; }`)
  for better BLE timing and lower power
- **Fuel gauge capacity is a placeholder.** `design-capacity = <300>` in the DTS
  must match the real cell or state-of-charge readings will be wrong
- **IMU axis signs are unverified.** Tilting right should push sand right
  (moot until the chip-ID fault above is resolved)
- **No deep sleep yet.** `CONFIG_PM=y` is set but nothing drives it; BLE scans
  and advertises continuously. The power figures in FIRMWARE_PLAN.md are targets,
  not measurements
- **Button UX is partial.** Left button: press wakes the display and starts
  the time reveal; 3 s hold reboots into DFU. Right button: 3 s hold shows the
  battery percentage (a no-op if the charging view is already up); a bare
  press does nothing on its own. Short/long mapping beyond this does not exist
  yet

## Layout

```
boards/arm/every_watch/   board definition, pinctrl, flash partitions
child_image/mcuboot.conf  MCUboot config (single-slot + USB DFU) — the live one
sysbuild/                 stale, unused — see the --sysbuild warning above
pm_static.yml             static flash layout
src/led_matrix/           WS2812B encoding, compositor, parallel DMA commit
src/sand/                 falling-sand cellular automaton, 30 Hz
src/time_display/         3×5 digit font, HH:MM onto the digit layer
src/display/              on/off state, button interrupts, auto-off timer
src/imu/                  BMI270 → gravity vector
src/identity/             watch identity hash, encounter counters in NVS
src/ble/                  advertising, encounter scanning, GATT, notifications
src/battery/ src/light/   fuel gauge and ambient light
src/selftest/             bring-up hardware self-test (CONFIG_EW_SELFTEST)
```

The FRTC8900 RTC driver is a separate west module:
[sahko123/frtc8900-zephyr](https://github.com/sahko123/frtc8900-zephyr).
