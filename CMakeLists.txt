cmake_minimum_required(VERSION 3.20.0)
# Register the boards/ directory inside this repo as a board root so that
# the every_watch board definition is visible to Zephyr's CMake machinery.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# In-repo Zephyr module: BMI260 IMU driver. Neither Zephyr nor NCS ships one
# (checked upstream too — zero mentions of bmi260 anywhere in the tree), and
# the BMI270 driver rejects this part on its chip ID. Kept in-repo rather than
# as a west module so it survives a workspace rebuild without a second repo to
# keep in sync — see modules/bmi260/README.md.
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/modules/bmi260)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(every_watch)

target_include_directories(app PRIVATE src)

if(CONFIG_EW_STAGE0)
    # Stage 0 replaces the application entirely — see stage0.conf. None of the
    # normal modules are compiled, because the drivers they depend on are
    # switched off in that configuration.
    target_sources(app PRIVATE src/stage0/stage0.c)
else()
    target_sources(app PRIVATE
        src/main.c
        src/led_matrix/led_matrix.c
        src/sand/sand.c
        src/time_display/time_display.c
        src/display/display.c
        src/imu/imu.c
        src/identity/identity.c
        src/ble/ble.c
        src/battery/battery.c
        src/light/light.c
        src/buttons/buttons.c
        src/ui/ui.c
    )

    # Guarded: watchdog.c's DEVICE_DT_GET(DT_ALIAS(watchdog0)) fails to link
    # (undefined __device_dts_ord_N) under any config without the driver.
    target_sources_ifdef(CONFIG_WATCHDOG app PRIVATE src/watchdog/watchdog.c)

    # Hardware self-test — only in bring-up / manufacturing builds.
    target_sources_ifdef(CONFIG_EW_SELFTEST app PRIVATE src/selftest/selftest.c)

    # Epoch -> RTC conversion. Built unconditionally: it used to be gated on
    # CONFIG_SHELL because the shell command was its only caller, but the BLE
    # time characteristic needs the same conversion and the production image
    # has no shell. The `settime` command inside it is still shell-gated.
    target_sources(app PRIVATE src/time_sync/time_sync.c)
endif()
