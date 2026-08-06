# Every Watch — firmware

Zephyr firmware for the Every Watch: an nRF52833 wearable with a 20×7 WS2812B
matrix, a falling-sand simulation that reveals the time as particles clear, and
a BLE proximity-encounter system.

Open by design — build it, modify it, flash it. The BLE protocol is documented
so anyone can write a companion app, and images genuinely are unsigned — the
bootloader verifies a SHA-256 hash and nothing else, so there are no keys to
need. That is integrity without authenticity, chosen deliberately: see
`child_image/mcuboot.conf`.

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

**RSSI monitor** — console-only build (RTT, so a J-Link on SWD, no USB) that
prints a table of every BLE device currently *visible* — address, name,
latest and best RSSI, packet count — refreshed every 3 s. Anything not heard
again since the last print drops out of the table on its own rather than
sitting there with a growing age. Doubles as an antenna health check and a
plain signal-strength monitor; see `CONFIG_EW_BLE_SCAN_DEBUG`'s help in
`Kconfig`:

```bash
west build -b every_watch/nrf52833 every_watch_firmware -p always -d build_rssi \
    -- -DEXTRA_CONF_FILE=rssi_monitor.conf
```

**Channel survey** — same console setup, but prints raw RF energy (dBm)
measured directly off the radio for all 40 BLE channels, roughly twice a
second, instead of advertisements from other devices. No reference
transmitter or known distance needed — a working antenna shows clear energy
humps at the three Wi-Fi channels (1/6/11), a disconnected or badly-matched
one shows flat thermal noise across the whole sweep. See
`CONFIG_EW_BLE_CHANNEL_SURVEY`'s help in `Kconfig` and `survey_report()` in
`src/ble/ble.c`:

```bash
west build -b every_watch/nrf52833 every_watch_firmware -p always -d build_survey \
    -- -DEXTRA_CONF_FILE=channel_survey.conf
```

For this one, `tools/channel_survey_plot.py` turns the raw text into a live
bar graph instead of reading numbers off the RTT log by eye — handy for
watching an antenna change in real time while tuning it:

```bash
python -m pip install matplotlib   # once
sh /c/ewdev/rtt.sh                 # in one terminal, leave running
python tools/channel_survey_plot.py  # in another
```

Sizes — the app slot is 450,048 B and RAM is 128 KB:

| Build | Flash | RAM |
|---|---:|---:|
| Production | 294,628 B (65%) | 64,028 B (49%) |
| MCUboot (48 KB partition) | 45,172 B (92%) | 21,352 B (16%) |

The console variants all land higher — RTT's buffers and the USB stack are the
two expensive additions — but the figures previously tabulated here had drifted
badly, so rather than quote numbers that are wrong again in a month, measure
whichever you care about: every build prints its own report at the end of the
link step, and

```bash
west build -d build_bringup 2>&1 | grep -E "FLASH:|RAM:"
```

reprints it. Note each build emits **two** reports, MCUboot first and the
application second.

> This app builds through the legacy child-image path
> (`child_image/mcuboot.conf`), not sysbuild. The stale `sysbuild/` config
> that used to disagree with it on single-slot/USB DFU settings has been
> removed — there's no `--sysbuild` flag to avoid anymore, it just isn't
> wired up.

## Flashing

**The first flash must be over SWD.** USB DFU is provided *by* MCUboot, so
MCUboot has to be on the chip before USB flashing can work at all. The same
applies to any later change to MCUboot itself — a bootloader cannot replace
itself over the update path it provides.

**A bad app image no longer needs SWD.** MCUboot arms a 30 s watchdog as the
very first thing it does (`MCUBOOT_WATCHDOG_SETUP()` at the top of its `main()`,
supplied by `patches/mcuboot-watchdog-setup.patch`), and `FIH_PANIC` on a failed
image validation comes much later. So a corrupt app now produces a ~30 s reset
loop rather than a permanent halt — and because the DFU button is sampled
*before* validation, holding the left button through any cycle drops you into
USB DFU. This follows from the code ordering; it has not been exercised by
deliberately corrupting an image.

SWD is still required for the first flash on a new board, for any MCUboot
change, and if the bootloader itself is damaged.

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

**After that, updates go over USB.** Hold **both buttons for 2 seconds** — the
app reboots into MCUboot — and **keep the left button held through the reset**.
The two gestures are different on purpose: both buttons is the *application's*
binding for "reboot me into the bootloader", while the **left** button is what
*MCUboot itself* samples at boot to decide whether to stay in DFU. It then waits
in USB DFU indefinitely, so there is no window to race. Release once it
enumerates.

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

## The shell

### On the production image, over USB

The normal image carries a **shell over USB CDC** — no special build, no
debugger. Plug the watch in and open the virtual COM port with any terminal at
any baud rate (CDC ignores it). This is the shell only: logging and `printk`
still go nowhere on this image, so it is for issuing commands, not watching
boot.

```
settime <unix-epoch-seconds>    set the RTC
led show                        current brightness settings and the resulting duty
led ambient <0-255>             force a brightness level
led auto <0|1>                  hand the level back to the light sensor, or take it
led gamma <1-3>                 perceptual curve on ambient; 2 is the default
led max <0-255>                 per-pixel ceiling (255 = off)
led budget <units>              total current budget
sand fill [percent]             seed random coloured grains
sand clear / sand count
i2c read_byte i2c@40003000 <addr> <reg>
```

`led ambient` will not stick on its own — the light sensor rewrites it on every
poll taken while the display is off. Use `led auto 0` first to hold a level
still, which is the only way to compare two settings by eye.

> USB is enabled from a `SYS_INIT` hook rather than `main()`, and it has to be:
> the shell backend checks the CDC device exactly once during its own init and
> never retries, so enabling USB from `main()` enumerates a port that never
> responds. See `usb_enable_early()` in `src/main.c`.

### On the bring-up images, over RTT

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

## First bring-up

Flash the RTT bring-up image over SWD and watch the console. `CONFIG_EW_SELFTEST`
runs before any application thread starts and reports PASS/FAIL for:

- **I2C** — full bus scan, then a targeted probe of all four expected devices
  (0x23 BH1750, 0x32 FRTC8900, 0x55 BQ27441, 0x68 BMI260)
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

- **~~WS2812B T1H marginally out of spec~~ — resolved.** The LED matrix moved
  from SPI to PWM0-3 + EasyDMA (see `led_matrix.c`); T1H is now 812.5 ns,
  comfortably mid-spec (datasheet 800 ns ±150). Left here as a record in case
  the driver ever regresses, not as an open item.
- **~~Row 6 locks interrupts for ~585 µs per frame~~ — resolved.** Row 6 now
  runs on PWM3 (P0.03) alongside the other three lines, same as the item
  above — no more bit-banging, no more interrupt lock.
- **~~IMU fails its chip-ID check~~ — resolved.** The part is a BMI2**60**, not
  the BMI270 the firmware originally assumed; chip ID 0x27 was the correct
  answer to a question being asked of the wrong driver. An in-repo BMI260
  driver module fixed it, and the accelerometer then needed explicitly powering
  up (the driver leaves it off until a sampling frequency is set). Sand gravity
  and the axis mapping both work.
- **Wrist-tilt wake does not work and is disarmed.** The BMI260's any-motion
  feature-register layout differs from the BMI270's and Bosch treat the map as
  NDA material, so the trigger can be armed but never fires — verified over 50 s
  of deliberate movement with the display off. The code is left compiled but
  unbound behind `CONFIG_EW_IMU_ANYMOTION_WAKE`. Tap-to-wake is documented for
  this part and is the most likely replacement; see the block comment above
  `motion_trigger_handler()` in `src/imu/imu.c`.
- **Fuel gauge capacity needs checking against the fitted cell.**
  `design-capacity = <400>` (mAh) in the DTS — state-of-charge readings are
  wrong if this does not match.
- **Idle current is unmeasured.** The figures in FIRMWARE_PLAN.md are targets.
  BLE scans and advertises continuously, and `CONFIG_PM` is deliberately not
  set — see `prj.conf`, where the reasoning is written out: it silently did
  nothing on this SoC in NCS 2.7 for a long time. `CONFIG_PM_DEVICE` is on but
  buys nothing for the sensors, because none of their drivers implement PM
  actions.
- **Colour resolution runs out at the bottom of the brightness range.** At the
  darkness floor a whole colour spans 2-3 of the WS2812B's 256 levels, so hue
  degrades as it dims. Both ordered and temporal dithering were implemented and
  removed — see `src/light/light.c`'s `lux_to_brightness()` comment for the
  measurements and why raising the floor is the real lever.

## Buttons

Two buttons, all gestures recognised by `src/buttons/` and bound in one
`switch` in `src/ui/ui.c`. A hold is 2 s.

| Gesture | Effect |
|---|---|
| Left, press | Clock page — falling-sand time reveal. Pressing again restarts it |
| Right, press | Toggles the sand toy on and off; dismisses the battery page if it is up |
| Right, hold | Battery percentage |
| Both, hold | Reboots into the MCUboot USB DFU bootloader |
| Left hold, and either double-press | Recognised, deliberately unbound |

Note that **both** buttons trigger DFU, not the left one. That is separate from
MCUboot's own DFU entry, which is the *left* button held through a reset and is
a bootloader feature the application has no part in — see Flashing above.

The display sleeps 10 s after the last interaction.

## Layout

```
boards/arm/every_watch/   board definition, pinctrl, flash partitions
child_image/mcuboot.conf  MCUboot config (single-slot + USB DFU) — the live one
pm_static.yml             static flash layout
patches/                  patches to the SDK itself, outside this repo — read this
modules/bmi260/           in-repo IMU driver (nothing upstream supports this part)
src/main.c                boot sequence, USB enable, watchdog liveness loop
src/buttons/              debounce and gesture recognition, own work queue
src/ui/                   page arbitration — every button binding lives here
src/display/              on/off state, auto-off timer, thread suspend/resume
src/led_matrix/           WS2812B encoding, compositor, parallel DMA commit
src/sand/                 falling-sand cellular automaton, 30 Hz; drives commits
src/time_display/         3×5 digit font, HH:MM onto the digit layer
src/imu/                  BMI260 → gravity vector
src/identity/             watch identity hash, encounter counters in NVS
src/ble/                  advertising, encounter scanning, GATT, notifications
src/battery/ src/light/   fuel gauge and ambient light
src/time_sync/            the settime shell command
src/watchdog/             feeds the watchdog MCUboot armed
src/selftest/             bring-up hardware self-test (CONFIG_EW_SELFTEST)
```

The FRTC8900 RTC driver is a separate west module:
[sahko123/frtc8900-zephyr](https://github.com/sahko123/frtc8900-zephyr).
