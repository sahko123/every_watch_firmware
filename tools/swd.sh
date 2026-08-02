#!/bin/sh
# SWD helper for Every Watch — connectivity check, flash, RTT console, recover.
#
# Usage (Git Bash):
#   sh tools/swd.sh check              # is the probe reaching the chip?
#   sh tools/swd.sh flash [image.hex]  # program over SWD (default: bring-up build)
#   sh tools/swd.sh rtt                # start RTT servers, logs on 9090, shell on 9091
#   sh tools/swd.sh recover            # mass-erase + unlock (DESTRUCTIVE, prompts)
#
# Env overrides:
#   SPEED=100        adapter clock in kHz (default 1000; drop to 100 if flaky)
#   OPENOCD=/path    explicit openocd binary
#
# Wiring, for reference (Pico running debugprobe -> watch SWD pads):
#   GP2 -> SWCLK, GP3 -> SWDIO, GND -> GND.
#   Do NOT power the watch from the Pico — it runs on its own battery/USB,
#   share ground only.

set -e

SPEED="${SPEED:-1000}"
DEFAULT_IMAGE="/c/ewdev/build_bringup/zephyr/merged.hex"

XPACK="/c/Users/sahko/AppData/Local/Microsoft/WinGet/Packages/xpack-dev-tools.openocd-xpack_Microsoft.Winget.Source_8wekyb3d8bbwe/xpack-openocd-0.12.0-7"
OCD="${OPENOCD:-$XPACK/bin/openocd.exe}"
SCRIPTS="$XPACK/openocd/scripts"

if [ ! -x "$OCD" ]; then
	OCD="$(command -v openocd 2>/dev/null || true)"
	if [ -z "$OCD" ]; then
		echo "openocd not found. Set OPENOCD=/path/to/openocd.exe" >&2
		exit 1
	fi
	SCRIPTS=""
fi

ocd() {
	if [ -n "$SCRIPTS" ]; then
		"$OCD" -s "$SCRIPTS" -f interface/cmsis-dap.cfg -f target/nordic/nrf52.cfg \
			-c "adapter speed $SPEED" "$@"
	else
		"$OCD" -f interface/cmsis-dap.cfg -f target/nordic/nrf52.cfg \
			-c "adapter speed $SPEED" "$@"
	fi
}

# "cannot read IDR" means the probe never got a reply from the chip at all.
# That is always electrical, never firmware: SWD's IDR read needs nothing
# from the target except power and three intact wires.
hint_no_idr() {
	cat >&2 <<'EOF'

Could not reach the chip. In rough order of likelihood:

  1. Watch not powered. SWD carries no power — the watch must be on its own
     battery or USB. A flat cell looks exactly like this.
  2. GND not connected. SWCLK/SWDIO are referenced to it; without a ground
     return the two signal wires read as connected but communicate nothing.
  3. SWCLK/SWDIO loose, swapped, or leads too long. Reseat both.
  4. Marginal signal integrity — retry with: SPEED=100 sh tools/swd.sh check

To tell "no power" apart from "bad wiring": hold the left button, replug USB
while still holding, keep holding a few seconds, then check for the DFU device:
  powershell -c "Get-PnpDevice -PresentOnly | ? { \$_.InstanceId -like '*VID_2FE3*' }"
If it appears, the chip is alive and powered and the fault is the SWD wiring.
EOF
}

cmd_check() {
	echo "Probing target at ${SPEED} kHz ..."
	if ocd -c "init; targets; exit" 2>&1 | tee /dev/stderr | grep -q "DPIDR"; then
		echo ""
		echo "OK — chip is reachable."
	else
		hint_no_idr
		exit 1
	fi
}

cmd_flash() {
	IMAGE="${1:-$DEFAULT_IMAGE}"

	if [ ! -f "$IMAGE" ]; then
		echo "No such image: $IMAGE" >&2
		echo "Build one first, e.g. from /c/ewdev:" >&2
		echo "  west build -b every_watch/nrf52833 every_watch_firmware \\" >&2
		echo "      -d build_bringup -- -DEXTRA_CONF_FILE=bringup.conf" >&2
		exit 1
	fi

	echo "Flashing $IMAGE ($(stat -c %s "$IMAGE") bytes) at ${SPEED} kHz ..."
	echo "Note: merged.hex contains MCUboot + app; app_update.bin does NOT and"
	echo "      is for USB DFU, not for this path."
	echo ""

	ocd -c "init" \
	    -c "targets" \
	    -c "reset init" \
	    -c "flash write_image erase $IMAGE" \
	    -c "reset run" \
	    -c "shutdown"

	echo ""
	echo "Done. MCUboot verifies the image's SHA-256 before chainloading, so if"
	echo "the watch comes up, the app slot is intact."
}

cmd_rtt() {
	echo "Starting RTT servers — logs on 9090, shell on 9091. Ctrl-C to stop."
	echo "Connect with:  telnet localhost 9090"
	echo ""
	ocd -c "init; reset run" \
	    -c "rtt setup 0x20000000 0x20000 \"SEGGER RTT\"" \
	    -c "rtt start" \
	    -c "rtt server start 9090 0" \
	    -c "rtt server start 9091 1"
}

cmd_recover() {
	cat <<'EOF'
DESTRUCTIVE: nrf52_recover mass-erases the entire chip — MCUboot, the app,
and both NVS partitions (BLE bonds, encounter history). Everything.

Only useful if the chip has readback protection (APPROTECT) engaged, which
shows up as an explicit "AP lock engaged" warning, not as "cannot read IDR".
If you are seeing "cannot read IDR", this will NOT help — that is a wiring
or power fault and a mass erase cannot fix it.

EOF
	printf "Type 'erase' to proceed: "
	read -r reply
	if [ "$reply" != "erase" ]; then
		echo "Aborted."
		exit 1
	fi

	ocd -c "init; nrf52_recover; exit"
	echo ""
	echo "Chip erased and unlocked. It now has NO bootloader — reflash with:"
	echo "  sh tools/swd.sh flash"
}

case "${1:-}" in
	check)   shift; cmd_check "$@" ;;
	flash)   shift; cmd_flash "$@" ;;
	rtt)     shift; cmd_rtt "$@" ;;
	recover) shift; cmd_recover "$@" ;;
	*)
		sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
		exit 1
		;;
esac
