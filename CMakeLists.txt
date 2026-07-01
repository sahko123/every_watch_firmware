cmake_minimum_required(VERSION 3.20.0)
# Register the boards/ directory inside this repo as a board root so that
# the every_watch board definition is visible to Zephyr's CMake machinery.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(every_watch)

target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
    src/main.c
    src/led_matrix/led_matrix.c
    src/sand/sand.c
    src/time_display/time_display.c
    src/display/display.c
    src/imu/imu.c
    src/identity/identity.c
    src/ble/ble.c
)
