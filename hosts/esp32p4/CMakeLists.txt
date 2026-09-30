# hosts/esp32p4 - an ESP32-P4 (Waveshare ESP32-P4-Module-DEV-KIT) as the
# host: one app a build, on the I2S transport (../../transports/esp32p4-i2s)
# and the host library (../../libpgpu). ESP-IDF v5.5 (~/esp/esp-idf-v5.5):
#
#   . ~/esp/esp-idf-v5.5/export.sh
#   idf.py -B build-gears -DPGPU_APP=gears build flash monitor
#
# PGPU_APP: selftest (default), gears, breakout, flight, antigrav, walk, keep, isles, touch, toy-NAME (demos/demos.cmake),
# jet-NAME (demos/jet/jet.cmake), or pincheck (the pins alone, before wiring the RPi: main/pincheck.c)
cmake_minimum_required(VERSION 3.16)

set(PGPU_APP "selftest" CACHE STRING "the app to build (demos/demos.cmake)")

# ninja (and idf.py, which runs it) at a core fewer than the machine has
cmake_host_system_information(RESULT PGPU_CORES QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR PGPU_JOBS "${PGPU_CORES} - 1")
if(PGPU_JOBS LESS 1)
	set(PGPU_JOBS 1)
endif()
set_property(GLOBAL PROPERTY JOB_POOLS pgpu_jobs=${PGPU_JOBS})
set(CMAKE_JOB_POOL_COMPILE pgpu_jobs)
set(CMAKE_JOB_POOL_LINK pgpu_jobs)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pgpu_esp32p4)
