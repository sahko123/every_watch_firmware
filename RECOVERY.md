# Firmware recovery

**Design goal: a USB cable is the only tool required.** Every failure mode that
can realistically happen to a watch in someone's hands must be recoverable with
the cable it shipped with. No debugger, no opening the case, no returning it.

There is exactly one exception, at the bottom of this document, and it cannot
occur without physically reflashing the bootloader.

> **Status: proposed, not yet implemented.** The current firmware uses MCUboot's
> USB DFU class with a 5-second window on every boot. This document describes
> the design that replaces it. See "Implementation" below for what changes.

---

## How it works

MCUboot runs before the application on every boot and decides whether to start
the app or wait for a new one. It enters recovery mode if **any** of these are
true, and all four checks happen *before* the app image is even validated:

| Trigger | Config | Covers |
|---|---|---|
| Left button held at power-on | `BOOT_SERIAL_ENTRANCE_GPIO` | Anything, including a working app |
| App asked for it before rebooting | `BOOT_SERIAL_BOOT_MODE` | Normal updates |
| No valid application present | `BOOT_SERIAL_NO_APPLICATION` | Corrupt or half-written image |
| Device was reset by the reset pin | `BOOT_SERIAL_PIN_RESET` | Bench work, if nRESET is on a pad |

Recovery mode presents itself over USB as a serial port and speaks the MCUmgr
SMP protocol. It waits indefinitely — there is no window to miss.

Because the button and boot-mode checks run before image validation, **a broken
app cannot lock you out.** MCUboot never trusts the application enough to let it
prevent recovery.

---

## The recovery ladder

Work down this list. Each step needs only the USB cable.

### 1. Normal update — the app is working

Hold the **left button for 3 seconds**. The watch reboots into recovery.

Equivalent triggers, all doing the same thing:
- A `bootloader` command on the console (bring-up builds)
- A write to the DFU BLE characteristic from the companion app

Then:

```bash
mcumgr --conntype serial --connstring "COM5,baud=115200" image upload build/zephyr/app_update.bin
mcumgr --conntype serial --connstring "COM5,baud=115200" reset
```

Or use Nordic's **nRF Connect Device Manager** app if you'd rather not touch a
command line.

### 2. The app is running but you can't drive it

UI is broken, buttons don't respond, screen is dead — but it's powered.

**Hold the left button and keep holding it while the watch resets.** The reset
will come from the watchdog within a few seconds (see below). MCUboot sees the
button before it looks at the app, and stays in recovery.

### 3. The app is hung

Nothing on screen, no response to anything.

Same as step 2 — **hold the left button and wait.** The hardware watchdog
reboots the watch whether or not the application is still executing, and
MCUboot catches the held button on the way back up.

This is why the watchdog matters. Without it, a hung application never gives up
the CPU, MCUboot never runs again, and the only way in is SWD. The watchdog is
what makes the USB-only guarantee actually hold.

### 4. The firmware is corrupt

A flash was interrupted, the battery died mid-upload, the image is bad.

**Just plug in the USB cable.** MCUboot verifies the image's SHA-256 against
flash on every boot; a corrupt image fails, and with `BOOT_SERIAL_NO_APPLICATION`
the bootloader stays in recovery mode indefinitely rather than halting. The
watch is already sitting there waiting for you.

### 5. The battery is flat

Plug in USB. That's a power-on reset, which is a boot, which means every trigger
above is available again. Hold the left button while plugging in if you want to
force recovery.

### 6. The bootloader itself is corrupt

**This is the one case USB cannot fix**, because the thing that would talk to
USB is what's broken.

It requires SWD access to the pads inside the case. A **Raspberry Pi Pico
running picoprobe** works and costs a few pounds — no commercial debugger
needed:

```bash
# Pico wired to the watch's SWD pads: GP2->SWCLK, GP3->SWDIO, GND->GND
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
        -c "program build/zephyr/merged.hex verify reset exit"
```

In practice this should never happen to a shipped watch. MCUboot is written once
during manufacturing and nothing afterwards can rewrite it — the bootloader
partition is marked `read-only` in the devicetree, DFU only ever touches the
application slot, and the app has no flash-write access to that region. It is
listed here for completeness and because the watch is meant to be hackable: if
you deliberately replace the bootloader yourself, this is how you get back.

---

## Implementation

### MCUboot (`child_image/mcuboot.conf`)

```
# Serial recovery over USB CDC, replacing the USB DFU class
CONFIG_MCUBOOT_SERIAL=y
CONFIG_BOOT_SERIAL_CDC_ACM=y

# Entry conditions, all checked before image validation
CONFIG_BOOT_SERIAL_ENTRANCE_GPIO=y    # left button held at boot
CONFIG_BOOT_SERIAL_BOOT_MODE=y        # app requested it via retained register
CONFIG_BOOT_SERIAL_NO_APPLICATION=y   # no valid image -> wait forever
CONFIG_BOOT_SERIAL_PIN_RESET=y        # reset pin, if exposed

# Smaller than the RSA-2048 default, same (nil) security value
CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y
```

`CONFIG_BOOT_USB_DFU_WAIT` is removed. That also eliminates the 5-second delay
currently incurred on **every** boot, connected to USB or not.

### Devicetree

- A `mcuboot_button0` alias pointing at the left button (P0.17). MCUboot fails
  to compile without it once `BOOT_SERIAL_ENTRANCE_GPIO` is on.
- A retention node over `GPREGRET` for the boot-mode flag, plus
  `chosen { zephyr,boot-mode = ... }`. GPREGRET is a hardware register that
  survives a soft reset, so this needs no SRAM carve-out —
  `nordic,nrf-gpreget` and `drivers/retained_mem/retained_mem_nrf_gpregret.c`
  are both already in the tree.

### Application

- **Hardware watchdog.** The single most important piece. A ~10 s timeout fed
  from a low-priority thread. Consider only feeding it while the system is
  actually healthy, so a deadlocked display or BLE thread converts into a reset
  — and therefore into a recovery opportunity — rather than a silent brick.
- **`bootmode_set(BOOT_MODE_TYPE_BOOTLOADER)` then `sys_reboot()`**, replacing
  the current bare `sys_reboot()` on the 3-second button hold.
- A `bootloader` shell command in the bring-up builds.
- Later: a BLE characteristic doing the same, so the companion app can start an
  update.

### Consequence to be aware of

The upload protocol changes from the USB DFU class to MCUmgr/SMP, so the tool
changes from `dfu-util` to `mcumgr` (or nRF Connect Device Manager). Same cable,
same virtual COM port, different command.

---

## Things to verify on hardware

- Whether the watchdog survives and behaves across the MCUboot → app handoff
- That the left button reads correctly in MCUboot before the app has configured
  any GPIO
- That USB enumerates when running from a flat battery on USB power alone
- Whether nRESET (P0.18 by default) is routed anywhere accessible — if not,
  `BOOT_SERIAL_PIN_RESET` does nothing and can be dropped
