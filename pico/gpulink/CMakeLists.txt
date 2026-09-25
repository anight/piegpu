cmake_minimum_required(VERSION 3.13)

# Pico 2 W (RP2350) at the SDK default 150 MHz: I2S bit clock 75 MHz
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(gpulink C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(gpulink main.c pgpu.c)
pico_generate_pio_header(gpulink ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
target_include_directories(gpulink PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol)
target_link_libraries(gpulink pico_stdlib hardware_pio hardware_dma)

# console on UART0 (GP0/GP1) -> Debugprobe UART bridge
pico_enable_stdio_uart(gpulink 1)
pico_enable_stdio_usb(gpulink 0)
