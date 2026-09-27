# hosts/esp32p4 - an ESP32-P4 (Waveshare ESP32-P4-Module-DEV-KIT) as the
# host: one app a build, on the I2S transport (../../transports/esp32p4-i2s)
# and the host library (../../libpgpu). ESP-IDF v5.5 (~/esp/esp-idf-v5.5):
#
#   . ~/esp/esp-idf-v5.5/export.sh
#   idf.py -B build-gears -DPGPU_APP=gears build flash monitor
#
# PGPU_APP: selftest (default), gears, breakout, flight, toy-NAME (demos/demos.cmake),
# or pincheck (the pins alone, before wiring the Zero: main/pincheck.c)
cmake_minimum_required(VERSION 3.16)

set(PGPU_APP "selftest" CACHE STRING "the app to build (demos/demos.cmake)")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pgpu_esp32p4)
