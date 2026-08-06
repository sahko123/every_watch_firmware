#!/bin/sh
# Reapply the vendored SDK patches after `west update` or a fresh workspace
# checkout — see patches/README.md for why these exist.
#
# Usage: sh patches/apply.sh /c/ewdev

set -e

WORKSPACE="${1:-/c/ewdev}"
PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"

apply_one() {
	repo="$1"
	patch="$2"

	if [ ! -d "$repo" ]; then
		echo "Skipping $patch: $repo does not exist" >&2
		return
	fi

	echo "Applying $patch to $repo ..."
	if git -C "$repo" apply --check "$PATCH_DIR/$patch" 2>/dev/null; then
		git -C "$repo" apply "$PATCH_DIR/$patch"
		echo "  OK"
	elif git -C "$repo" apply --reverse --check "$PATCH_DIR/$patch" 2>/dev/null; then
		echo "  Already applied, skipping"
	else
		echo "  FAILED to apply cleanly — the vendored file has likely"
		echo "  changed (NCS/Zephyr version bump). Apply by hand; see"
		echo "  patches/README.md for what to re-verify." >&2
		exit 1
	fi
}

apply_one "$WORKSPACE/bootloader/mcuboot" "mcuboot-watchdog-setup.patch"
apply_one "$WORKSPACE/zephyr"             "zephyr-usb_dfu-watchdog-feed.patch"
apply_one "$WORKSPACE/nrf"                "nrf-imgtool-hash-only.patch"
