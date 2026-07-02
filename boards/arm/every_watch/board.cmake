board_runner_args(jlink "--device=nRF52833_xxAA" "--speed=4000")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

board_runner_args(dfu-util "--pid=2fe3:0100" "--alt=0" "--reset-at-upload")
include(${ZEPHYR_BASE}/boards/common/dfu-util.board.cmake)
