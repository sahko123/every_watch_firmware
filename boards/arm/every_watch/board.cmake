# Runners for this board.
#
# OpenOCD is listed first, so it is the default for `west flash` and
# `west debug`. It drives a Raspberry Pi Pico running picoprobe/debugprobe
# firmware over CMSIS-DAP — see support/openocd.cfg for wiring. This is the
# cheap path that needs no commercial debugger, and it is what the first flash
# of a blank board must use, since USB DFU only exists once MCUboot is on the
# chip.
#
# The others remain available explicitly:
#     west flash -r jlink       # if you have a J-Link or an nRF52840 DK
#     west flash -r dfu-util    # over USB, once MCUboot is already flashed
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)

board_runner_args(jlink "--device=nRF52833_xxAA" "--speed=4000")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

board_runner_args(dfu-util "--pid=2fe3:0100" "--alt=0" "--reset-at-upload")
include(${ZEPHYR_BASE}/boards/common/dfu-util.board.cmake)
