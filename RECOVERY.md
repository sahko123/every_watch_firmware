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

- **Feed the watchdog** from a low-priority thread, ideally only while the
  system is provably healthy, so a deadlocked display or BLE thread converts
  into a reset — and therefore a recovery opportunity — rather than a silent
  brick. See "The watchdog must start in MCUboot" below for why the app must
  not be the thing that *starts* it.
- **`CONFIG_WDT_DISABLE_AT_BOOT=n`**, or Zephyr switches off the watchdog the
  bootloader just handed over.
- **`bootmode_set(BOOT_MODE_TYPE_BOOTLOADER)` then `sys_reboot()`**, replacing
  the current bare `sys_reboot()` on the 3-second button hold.
- A `bootloader` shell command in the bring-up builds.
- Later: a BLE characteristic doing the same, so the companion app can start an
  update.

---

## The watchdog must start in MCUboot

A corrupt image is **not** caught by the watchdog and does not need to be: the
SHA-256 check fails, MCUboot never jumps, and the CPU never executes a single
instruction of the bad image. MCUboot stays in control and enters recovery.

The watchdog exists for a different failure: an image that *passes* validation
and then misbehaves. That splits in two:

| | Outcome |
|---|---|
| Hangs **after** the watchdog is started | Reset, then recovery. Fine. |
| Hangs **before** the watchdog is started | Nothing fires. Brick. |

The second case is not hypothetical on this board. Zephyr runs every driver's
init before `main()`, and four devices share one I2C bus. A sensor holding SDA
low, or a driver spinning on an ACK that never arrives, hangs the system before
any application code executes — so an application that starts its own watchdog
never gets the chance.

**The fix is to have MCUboot start it.** The nRF52 watchdog cannot be stopped
once started — there is no stop task, only a reset clears it. So a watchdog
started by the bootloader is inherited by the application as something it must
feed or die, covering driver init and everything else before `main()`.

MCUboot already has the hook: `MCUBOOT_WATCHDOG_SETUP()` at `main.c:450`, ahead
of every recovery check. **On nRF it currently expands to nothing:**

- `BOOT_WATCHDOG_FEED` defaults `y` on Nordic and `imply`s `NRFX_WDT`
- the `CONFIG_NRFX_WDT` branch of `mcuboot_config.h` defines only
  `MCUBOOT_WATCHDOG_FEED()`
- the branch defining `MCUBOOT_WATCHDOG_SETUP()` as `wdt_setup()` is the
  generic-driver `#elif`, which is therefore never taken
- `mcuboot_config.h:381` then supplies an empty fallback definition

So today MCUboot faithfully feeds a watchdog that nothing ever started.

To get the real behaviour, force the generic driver path in MCUboot:

```
CONFIG_WATCHDOG=y
CONFIG_WDT_NRFX=y
CONFIG_NRFX_WDT=n     # override the imply so the nrfx branch is not taken
```

plus a `watchdog0` devicetree alias pointing at `&wdt0`.

**Unverified:** MCUboot's macro calls `wdt_setup(wdt, 0)` with no preceding
`wdt_install_timeout()`. Whether that arms a channel on the nrfx driver needs
checking on hardware; if it does not, MCUboot needs a small `SYS_INIT` that
installs a timeout and starts the watchdog itself.

### Watchdog trade-offs

- **Deep sleep.** A deadlocked Zephyr system does not spin — it drops into the
  idle thread and sleeps. The watchdog therefore has to keep counting through
  sleep, which means waking periodically to feed it. That works against the
  <10 µA idle target. The nRF52 watchdog can time out up to ~36 hours, so a
  30-60 s window keeps the wake cost small.
- **Debugger halts.** Get `CONFIG.HALT` right or the watchdog fires while the
  CPU is stopped at a breakpoint during bring-up.

### Proving it works

Do not take any of this on faith — it is all testable, and belongs in the
bring-up procedure:

| Test | Method | Expected |
|---|---|---|
| Corrupt image rejected | Upload a truncated image | MCUboot refuses it, enters recovery |
| Hang after init | Shell command that locks interrupts and spins | Watchdog reset within the timeout |
| Hang before `main()` | Jumper SDA low through boot | Watchdog reset — proves the MCUboot-start fix |
| Reset cause visible | `hwinfo_get_reset_cause()` logged every boot | Reports watchdog, not power-on |

That last one is worth keeping permanently: logging the reset cause turns "it
rebooted on its own" into something diagnosable.

---

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
