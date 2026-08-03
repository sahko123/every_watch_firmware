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

Other build variants (production, USB-CDC console, stage0) are in README.md.
This project builds through the legacy child_image path
(`child_image/mcuboot.conf`), not sysbuild — the stale `sysbuild/` config
that used to warn about this has been removed entirely, so there's nothing
left to accidentally invoke.

**Two files outside this repo, at `C:\ewdev\zephyr` and
`C:\ewdev\bootloader\mcuboot`, are hand-patched** to make MCUboot's watchdog
actually work on this SoC (see `patches/README.md`). A `west update` or a
fresh workspace checkout loses them silently — run `sh patches/apply.sh
/c/ewdev` afterward, or the watchdog silently goes back to being inert
plumbing with no error to notice by.

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
(or the in-app 3 s hold) and MCUboot waits indefinitely in USB DFU
(`CONFIG_BOOT_USB_DFU_GPIO`) until a transfer completes — this was deliberately
changed from a 5-second window so there's time to sort out drivers/Zadig
without racing a timeout.

**First flash on new hardware, or any MCUboot change, must be over SWD** — USB
DFU is provided *by* MCUboot, so it can't bootstrap itself. SWD is also the
only recovery path if the app is invalid: no reset pin, no watchdog armed yet,
so a bad image halts at `FIH_PANIC` permanently. Fixing that (arming a
watchdog in MCUboot) is blocked on bootloader flash space — it's at 97% of its
48 KB partition, full plan in RECOVERY.md.

Verify a flash landed by checking the `2FE3` USB device disappeared (MCUboot
validated the SHA-256 and chainloaded):

```powershell
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_2FE3*" }
```

No device present = success.

## Architecture (src/)

- `main.c` — boot sequence, boot-blink indicator, the two button-hold polling
  loops (left = DFU reboot, right = battery readout)
- `display/` — on/off state machine, 10 s auto-off timer, left-button press
  interrupt (wakes display + starts time reveal)
- `time_display/` — 3×5 digit font (shared via `led_matrix.h`), HH:MM curtain
  reveal. Gated by an `active` flag: time only renders as the result of an
  explicit reveal, never just because the display woke up for something else
  (see "Recent changes" below)
- `led_matrix/` — WS2812B encoding, 4-layer compositor (notification > sand >
  digits > bg), parallel DMA commit, ambient-brightness + current-budget
  scaling, shared glyph stamping (`led_stamp_digit`/`led_stamp_percent`)
- `sand/` — falling-sand cellular automaton, 30 Hz, free mode + two reveal
  modes (curtain rain, column fill)
- `battery/` — BQ27441 fuel gauge polling, low-battery indicator, the
  percentage readout screen (see "Recent changes")
- `light/` — BH1750 ambient light → `led_brightness`, sampled on button press
  (see "Recent changes")
- `imu/`, `identity/`, `ble/`, `selftest/` — not touched recently; see
  FIRMWARE_PLAN.md

## Hardware quirks (PCB rev 1)

- **No 32.768 kHz crystal, no I2C pull-ups** — both will hang or damage the
  board without the firmware workarounds already in place. Don't "fix" the
  devicetree to assume normal crystal/pull-up hardware.
- **No reset pin.** This is why DFU entry has no software escape and why
  `FIH_PANIC` is currently unrecoverable without SWD.
- **VBUS *is* connected** to USB 5 V, contrary to what this file said until
  2026-08-03. The nRF52833 senses it through the POWER peripheral —
  `USBREGSTATUS.VBUSDETECT` plus the `USBDETECTED`/`USBREMOVED` events — so no
  GPIO is involved and nothing needs adding to the devicetree to read it.

  Nothing currently uses it, and two decisions were made on the assumption it
  was absent and are worth revisiting:

  - The USB CDC console has to be a **separate build variant**
    (`bringup_usb.conf`) because an always-on USB stack costs idle current. With
    VBUS detectable, USB can be brought up only while a cable is present, which
    would put the `settime` shell command in the normal image instead of a
    variant that has to be flashed specially.
  - Charging detection leans on the SGM41524 indicator on P0.05, whose
    interrupt path is still unverified. VBUS is a more direct signal for
    "a cable is attached" — it does not say the cell is charging, which remains
    the fuel gauge's job.
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

## Recent changes (uncommitted as of this writing — see below)

- **Battery percentage screen**, right button 3 s hold (`battery.c`,
  `battery_show_level()`). Colour: blue discharging, green (with a travelling
  hue shimmer) while charging, red under 20% regardless.
- **Persistent charging view**: plugging in enters a mode that stays up and
  refreshes every poll for the whole charging session (not a timed peek),
  drops instantly on unplug. A manual hold is a no-op while this is active.
- **Time-display holdover fix**: previously, waking the display for any
  reason (a bare right-press, the battery screen dismissing) could show time
  instantly with no reveal, because the digit layer was kept live in the
  background continuously. Fixed with the `active` flag in `time_display.c` —
  see that file's comments before changing display-wake paths.
- **Ambient light**: sampled on button press (`light_sample_now()`, called
  from `display.c`'s button ISR and `main.c`'s right-button hold loop) instead
  of a fixed 2 s timer. Darkness floor and room-light band are tuned by feel
  on the actual (custom, non-standard) LEDs, not measured — expect to retune
  with `led ambient`/`led max` in the shell rather than trusting the lux
  breakpoints in `light.c` as calibrated.

**None of this is committed yet.** `git status` in `C:\ewdev\every_watch_firmware`
will show it. If you're picking this up in a fresh context, check there before
assuming a clean tree — this checkout (if you're reading this file from the
non-`C:\ewdev` copy) won't have it until it's pushed and pulled.
