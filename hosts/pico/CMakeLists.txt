# hosts/pico - the Raspberry Pi Pico 2 W as the host: the self tests
# (main.c) and all the demos (../../demos), on the I2S transport
# (../../transports/pico-i2s) and the host library (../../libpgpu).
cmake_minimum_required(VERSION 3.13)

# Pico 2 W (RP2350) at the SDK default 150 MHz: I2S bit clock 75 MHz
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pgpu_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(DEMOS ${ROOT}/demos)
set(SHADERS ${DEMOS}/shaders)
include(${ROOT}/libpgpu/pgpu.cmake)

include(${ROOT}/demos/demos.cmake)

# pico_host_executable (name sources...): an executable on the I2S transport,
# with pgpu, pgl (precompiled programs) and the HUD; console on UART0
# (GP0/GP1, the Debugprobe's UART bridge)
function(pico_host_executable name)
	add_executable(${name} ${ARGN} ${ROOT}/transports/pico-i2s/pgpu_pico.c)
	pico_generate_pio_header(${name} ${ROOT}/transports/pico-i2s/pcm_out.pio)
	target_include_directories(${name} PRIVATE ${DEMOS})
	target_link_libraries(${name} pgpu pgpu_nocompiler pgpu_hud pico_stdlib hardware_pio hardware_dma)
	pico_enable_stdio_uart(${name} 1)
	pico_enable_stdio_usb(${name} 0)
	pico_add_extra_outputs(${name})
endfunction()

# demo_program (target name glslc-args...): a program of demos/shaders
function(demo_program target name)
	pgpu_glsl_program(TARGET ${target} NAME ${name} DIR ${SHADERS} ARGS ${ARGN})
endfunction()

# ---- the self tests and the demos (demos/demos.cmake) ------------------------------
# gpulink: the self tests (the link, the protocol and pgl); the others by name
pico_host_executable(gpulink)
pgpu_demo(gpulink selftest)
foreach(app ${PGPU_DEMO_APPS})
	if(NOT app STREQUAL "selftest")
		pico_host_executable(${app})
		pgpu_demo(${app} ${app})
	endif()
endforeach()

# ---- Jet examples on the GPU (demos/jet/jet.cmake): jet-NAME ---------------------
option(JET_PROFILE "print where the Pico's frame time goes (jet-*)" OFF)
include(${DEMOS}/jet/jet.cmake)
foreach(scene ${JET_APPS})
	pico_host_executable(jet-${scene})
	jet_demo(jet-${scene} ${scene})
endforeach()
