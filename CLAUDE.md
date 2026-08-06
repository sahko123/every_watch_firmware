# Every Watch firmware — working notes for Claude

nRF52833 wearable: 20×7 WS2812B matrix, falling-sand physics that reveals the
time as particles clear, BLE proximity encounters, BQ27441 fuel gauge, BH1750
ambient light. Zephyr / nRF Connect SDK v2.7.0.

Full detail lives in [README.md](README.md) (build/flash/bring-up),
[FIRMWARE_PLAN.md](FIRMWARE_PLAN.md) (hardware + architecture),
[RECOVERY.md](RECOVERY.md) (DFU/recovery design), and
[hardwarer_spec.md](hardwarer_spec.md) (pin map). This file is the
gotchas-and-current-state summary — read the others for anything this doesn't
cover.

## Build from C:\ewdev, not here

**This checkout is for editing. It is not a west workspace.** The real
workspace — `zephyr/`, `nrf/`, toolchain, the lot — lives at `C:\ewdev`, with
this repo checked out inside it at `C:\ewdev\every_watch_firmware`. They're
two separate git clones of the same GitHub repo (`sahko123/every_watch_firmware`),
kept in sync by commit + push from `C:\ewdev`, not by editing both. **GitHub
is the source of truth** — if you edit files here, either mirror the edit into
`C:\ewdev\every_watch_firmware` or just edit there directly, then build/flash
from `C:\ewdev`.

Toolchain env (Git Bash):

```bash
export PATH="/c/ncs/toolchains/ce3b5ff664/opt/bin:/c/ncs/toolchains/ce3b5ff664/opt/bin/Scripts:/c/ncs/toolchains/ce3b5ff664/mingw64/bin:/c/ncs/toolchains/ce3b5ff664/bin:/c/ncs/toolchains/ce3b5ff664/usr/bin:$PATH"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/c/ncs/toolchains/ce3b5ff664/opt/zephyr-sdk
export ZEPHYR_BASE=/c/ewdev/zephyr
```

Board target is **`every_watch/nrf52833`** (HWMv2 format) — not
`every_watch_nrf52833`. Build (bring-up image, from `/c/ewdev`):

```bash
west build -b every_watch/nrf52833 every_watch_firmware -d build_bringup -- -DEXTRA_CONF_FILE=bringup.conf
```

Other build variants (production, USB-CDC console, RSSI monitor, channel
survey) are in README.md.
This project builds through the legacy child_image path
(`child_image/mcuboot.conf`), not sysbuild — the stale `sysbuild/` config
that used to warn about this has been removed entirely, so there's nothing
left to accidentally invoke.

**Three files outside this repo, under `C:\ewdev\zephyr`,
`C:\ewdev\bootloader\mcuboot` and `C:\ewdev\nrf`, are hand-patched** (see
`patches/README.md`). Two make MCUboot's watchdog actually work on this SoC;
the third lets `CONFIG_BOOT_SIGNATURE_TYPE_NONE` build at all. A `west update`
or a fresh workspace checkout loses them silently — run `sh patches/apply.sh
/c/ewdev` afterward. The signing one fails the build loudly if it is missing;
the watchdog ones just revert to inert plumbing with no error to notice by.

## Flashing over USB DFU

`/c/ewdev/flash_usb.sh` handles the runtime→DFU PID switch automatically
(Zephyr's DFU class re-enumerates as a different USB ID —
`2FE3:0100` runtime vs `2FE3:FFFF` DFU mode — so plain `dfu-util -d 2fe3:0100`
loses the device mid-transfer). Windows needs Zadig/WinUSB bound to **both**
PIDs separately; see README.md's "two Zadig installs" section if flashing
fails with `LIBUSB_ERROR_NOT_SUPPORTED`.

```bash
sh /c/ewdev/flash_usb.sh                                    # default image
sh /c/ewdev/flash_usb.sh /c/ewdev/build_bringup/zephyr/app_update.bin
```

DFU entry is **persistent, not timed**: hold the left button through a reset
(or hold both buttons in-app, which reboots into it) and MCUboot waits
indefinitely in USB DFU
(`CONFIG_BOOT_USB_DFU_GPIO`) until a transfer completes — this was deliberately
changed from a 5-second window so there's time to sort out drivers/Zadig
without racing a timeout.

**First flash on new hardware, or any MCUboot change, must be over SWD** — USB
DFU is provided *by* MCUboot, so it can't bootstrap itself.

A bad *app* image, though, no longer needs SWD. That claim was true before the
watchdog patches and outlived them: `MCUBOOT_WATCHDOG_SETUP()` is now the first
statement in MCUboot's `main()` (line 450) and `FIH_PANIC` is at line 569, so a
failed validation gets a 30 s watchdog reset rather than a permanent halt. The
DFU button is sampled at line 486 — also before validation — so holding left
through any cycle of that loop reaches USB DFU. Reasoned from the code ordering,
not yet exercised by deliberately corrupting an image.

Bootloader occupancy is 92% of its 48 KB partition (45,172 of 49,152 B); it has
been as high as 97%, so re-measure before concluding anything fits.

Verify a flash landed by checking the `2FE3` USB device disappeared (MCUboot
validated the SHA-256 and chainloaded):

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_2FE3*" }
```

No device present = success.

## Architecture (src/)

- `main.c` — boot sequence, boot-blink indicator, `usb_enable()` (as a
  `SYS_INIT` hook, not from `main()` — see below), and forwarding button
  gestures to the UI. It binds no gestures itself; that all lives in `ui.c`
- `buttons/` — debounce and gesture recognition (single / double / hold /
  both), on its own work queue at priority -2 so a busy USB stack cannot
  starve sampling. Emits `BTN_EV_*`; binds nothing
- `ui/` — page arbitration. One page owns the display at a time, each with its
  own enter/exit and timeout, and every gesture binding is one `case` in
  `ui_handle_button()`. Start here to change what a button does
- `display/` — on/off state machine and the 10 s auto-off timer. Resumes and
  suspends the sand and IMU threads with the panel, so nothing runs while dark
- `time_display/` — 3×5 digit font (shared via `led_matrix.h`), HH:MM curtain
  reveal. Gated by an `active` flag: time only renders as the result of an
  explicit reveal, never just because the display woke for something else.
  Also auto-recovers the RTC's voltage-low flag (see below)
- `led_matrix/` — WS2812B encoding, 4-layer compositor (notification > sand >
  digits > bg), parallel DMA commit, ambient-brightness + gamma +
  current-budget scaling, shared glyph stamping
  (`led_stamp_digit`/`led_stamp_percent`), and the `led` shell command group
- `sand/` — falling-sand cellular automaton, 30 Hz, free mode + two reveal
  modes (curtain rain, column fill). Its thread also drives the panel commit,
  so it is what refreshes the display on any page that needs it
- `battery/` — BQ27441 fuel gauge polling, VBUS cable detection, low-battery
  indicator, percentage readout screen
- `light/` — BH1750 ambient light → `led_brightness`, adaptively polled (1 s
  for a minute after any interaction, then 10 s), and only while the display
  is off so the LEDs cannot feed back into the sensor
- `time_sync/` — the `settime <unix-epoch>` shell command, reachable over USB
  CDC in the normal image
- `watchdog/` — feeds the watchdog MCUboot started, but only if something has
  called `watchdog_alive()` since the last feed
- `imu/` — BMI260 accelerometer. Feeds sand gravity while the display is on,
  and watches for motion to wake it while off (`CONFIG_EW_IMU_MOTION_WAKE`,
  software — the hardware any-motion trigger does not work on this part)
- `identity/`, `ble/`, `selftest/` — see FIRMWARE_PLAN.md

## Hardware quirks (PCB rev 1)

- **No 32.768 kHz crystal, no I2C pull-ups** — both will hang or damage the
  board without the firmware workarounds already in place. Don't "fix" the
  devicetree to assume normal crystal/pull-up hardware.
- **No reset pin.** This is why DFU entry has no software escape, and why
  unplugging USB restarts nothing — the watch keeps running off its own cell.
  The watchdog is what provides the reset that this pin would have.
- **VBUS *is* connected** to USB 5 V, contrary to what this file said until
  2026-08-03. The nRF52833 senses it through the POWER peripheral —
  `USBREGSTATUS.VBUSDETECT` plus the `USBDETECTED`/`USBREMOVED` events — so no
  GPIO is involved and nothing needs adding to the devicetree to read it.

  Used by `battery.c`'s `vbus_init()` as `battery_cable_present()`, OR'd with
  the fuel gauge's current reading to decide "on power" — current alone tapers
  to ~0 near a full charge, which reads identically to unplugged.

  Note `vbus_init()` steps aside at runtime whenever `CONFIG_USB_DEVICE_STACK`
  is compiled in, because Zephyr's USB driver wants the same POWER peripheral
  events. That is the normal image now, not just the console variants, so
  cable detection there falls back to fuel-gauge current alone.

  The normal image carries a USB CDC **shell** (not console/logging) so
  `settime` is reachable without a special build. It is enabled
  unconditionally rather than gated on VBUS: gating would mean driving
  Zephyr's own USB VBUS handling instead of the `nrfx_power` hookup above, and
  that is real unverified work rather than a config line.
- **The internal DC/DC is off deliberately — do not enable it.** VDD comes from
  an external 3.3 V regulator, so the SoC runs in normal LDO mode. Enabling
  `CONFIG_BOARD_ENABLE_DCDC` requires inductors on the `DCC` pins that this
  board does not have; without them the supply would sag under radio load,
  which presents as an RF fault rather than a power one. The usual "the DC/DC
  halves radio current" advice does not apply here.
- **The SGM41524 is not an I2C part.** It is a standalone charger and P0.05 is
  its entire interface — there is nothing to configure, no driver to write, and
  its absence from the boot I2C sweep is correct rather than a fault. (The
  programmable part on the bus is the BQ27441 fuel gauge at 0x55; the two are
  easy to conflate.) Charge current and termination are set by hardware, so
  anything wrong there is a BOM question, not a firmware one.
- Charge indicator: SGM41524 on P0.05, active-low, edge interrupt — used only
  as a hint to trigger an immediate fuel-gauge poll; the BQ27441's average
  current is the actual source of truth for "charging". The interrupt path is
  unverified on hardware; if it's not firing, plug-in detection still works
  via the 15 s poll, just slower.

## Buttons

All bindings live in `ui_handle_button()` (`ui.c`) — one `case` each. Current
map, with `HOLD_MS` at **2 s**:

| Gesture | Effect |
|---|---|
| Left, press | Clock page; re-pressing restarts the reveal |
| Right, press | Toggles the sand toy; dismisses the battery page if it's up |
| Right, hold | Battery percentage (no-op if already showing) |
| Both, hold | Reboots into MCUboot USB DFU |
| Left hold, either double | Recognised by `buttons.c`, deliberately unbound |

Note **both** buttons for DFU, not left — left-hold is free. That is separate
from MCUboot's own DFU entry, which is the *left* button held through a reset
and is a bootloader feature this firmware has no part in.

## Things that will bite

- **Ambient brightness is gamma-corrected** (`led_gamma`, default 2). The
  numbers in `light.c`'s `lux_to_brightness()` are perceptual, not duty cycle:
  the floor of 24 is 0.88% duty, not 9.4%. Change the gamma and that whole
  curve needs re-checking. `led show` prints the duty the knobs work out to.
- **The brightness floor is set by resolution, not taste.** At ~1% duty a
  whole colour spans 2-3 LSB and hue starts collapsing under rounding.
  Dithering was tried twice to get past this — ordered (spatial) and
  error-feedback (temporal), the latter at up to 125 Hz refresh — and both
  were removed: the error has to surface as either visible texture or visible
  flicker, and eight bits at 1% duty simply does not carry the information.
  Raise the floor rather than reaching for a filter. See `light.c`'s comment.
- **Sensor drivers here do not implement `PM_DEVICE`.** BMI260, BH1750 and the
  custom FRTC8900 all pass `NULL` as the PM device, so `pm_device_action_run()`
  returns `-ENOSYS` and does nothing — silently, if the result is discarded.
  Both `imu.c` and `light.c` were caught by this; the IMU one left the
  accelerometer converting the whole time the display was off. Power sensors
  down through the driver's own API instead (for the BMI260, an ODR of zero
  clears `PWR_CTRL_ACC_EN`).
- **The RTC sets its own voltage-low flag** on any supply dip below ~1.6 V, and
  once set, `rtc_get_time()` returns `-ENODATA` forever until something calls
  `rtc_set_time()`. `time_display.c`'s `read_time()` now detects that and
  reseeds a placeholder automatically, so a dip shows 00:00 rather than a
  permanently blank clock. Confirmed on hardware by reading FLAG register 0x0E.
- **`usb_enable()` must run from a `SYS_INIT` hook, not `main()`.** The shell
  backend checks `device_is_ready()` on the CDC UART exactly once, during its
  own `POST_KERNEL` init, and never retries — `main()` runs after all of that,
  so enabling USB there enumerates the port but leaves the shell dead. See the
  comment on `usb_enable_early()` in `main.c` before moving it.
- **Buttons run on their own work queue at priority -2.** The system and USB
  work queues are both -1 and `CONFIG_TIMESLICE_SIZE` is 0, so USB activity
  could occupy the system queue long enough for a whole press to fall between
  two 15 ms samples and vanish. Don't move button sampling back onto the
  system queue.

## Current state

Check `git status` in `C:\ewdev\every_watch_firmware` before assuming a clean
tree — this checkout (if you're reading this from the non-`C:\ewdev` copy) is
only current as far as the last push.
