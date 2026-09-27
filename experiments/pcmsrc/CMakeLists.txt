cmake_minimum_required(VERSION 3.13)

# Pico 2 W (RP2350) at the SDK default 150 MHz system clock
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pcmsrc C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(pcmsrc main.c)
pico_generate_pio_header(pcmsrc ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
target_link_libraries(pcmsrc pico_stdlib hardware_pio hardware_dma)

# console on UART0 (GP0/GP1) -> Debugprobe UART bridge
pico_enable_stdio_uart(pcmsrc 1)
pico_enable_stdio_usb(pcmsrc 0)
