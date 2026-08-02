# Vendored SDK patches

Two small patches to the west workspace's own `zephyr` and `bootloader/mcuboot`
checkouts (at `C:\ewdev`, *outside* this repo) — not tracked by git anywhere
except here, so a `west update` or a fresh workspace checkout on another
machine silently loses them unless reapplied.

## Why these exist

Both close the same gap: on this SoC, MCUboot's own watchdog support never
actually arms anything. See `RECOVERY.md` and `child_image/mcuboot.conf`'s
comments for the full story — short version, `CONFIG_WDT_NRFX` (the only nRF
watchdog driver) unconditionally selects `CONFIG_NRFX_WDT0`, which itself
unconditionally selects `CONFIG_NRFX_WDT`, so MCUboot's `mcuboot_config.h`
always takes the branch that only ever defined a feed macro, never a setup
one — the "generic driver" branch that does define a real setup is
unreachable on this hardware no matter what's enabled via Kconfig.

- **`mcuboot-watchdog-setup.patch`** (`bootloader/mcuboot`) — adds a real
  `MCUBOOT_WATCHDOG_SETUP()` to the branch that's actually taken: installs a
  30 s timeout, then calls `wdt_setup()` with `WDT_OPT_PAUSE_HALTED_BY_DBG`
  (without it, the watchdog keeps counting down while the CPU is halted at a
  breakpoint over SWD, resetting the target mid-debug session).
- **`zephyr-usb_dfu-watchdog-feed.patch`** (`zephyr`) — `wait_for_usb_dfu()`
  had zero feed calls anywhere inside it, so arming the watchdog above would
  otherwise reset the board while it's just waiting for a human to plug in a
  USB cable. Adds feeding that distinguishes "idle, waiting on a human" from
  "stuck mid-transfer":
  - While `appIDLE`/`dfuIDLE`, feed unconditionally. That state is allowed to
    last forever by design (commit `d4b4585` replaced a 5 s DFU window with a
    persistent hold precisely so there's no timeout to race while sorting out
    cables and Zadig drivers) — putting any ceiling on it would quietly undo
    that.
  - In any other state, feed only while a download block has actually been
    written to flash within the last 5 minutes. Once nothing has, stop
    feeding on purpose so the 30 s hardware timeout catches up and resets —
    turning "wedged forever, only SWD gets you out" (this board has no VBUS
    sense, so unplugging the cable doesn't reset the chip) into "self-heals
    within a few minutes."

  Progress is tracked with a dedicated `dfu_progress_ctr`, bumped in
  `dfu_flash_write()`. **Do not be tempted to use `dfu_data.block_nr` for
  this** — it is only incremented on the `DFU_UPLOAD` (device→host) path,
  which this build doesn't even enable, so it stays zero for an entire
  firmware download. An earlier version of this patch made exactly that
  mistake, which would have reset the board 5 minutes into *every* DFU
  session including healthy ones, potentially mid-flash-write.

## Reapplying after `west update` or a fresh workspace

```bash
sh patches/apply.sh /c/ewdev
```

Or by hand, from each repo's own root:

```bash
cd /c/ewdev/bootloader/mcuboot && git apply "$OLDPWD/patches/mcuboot-watchdog-setup.patch"
cd /c/ewdev/zephyr           && git apply "$OLDPWD/patches/zephyr-usb_dfu-watchdog-feed.patch"
```

Both are small (well under 100 lines) and self-contained to one file each —
if either fails to apply cleanly after an NCS/Zephyr version bump, open the
target file and re-read the comment block the patch adds; it explains the
exact Kconfig chain and driver behavior the fix depends on, which is what to
re-verify against the new version.
