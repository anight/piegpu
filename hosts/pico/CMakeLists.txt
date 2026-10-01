# hosts/pico - the Raspberry Pi Pico 2 W as the host: the self tests
# (main.c) and all the demos (../../demos), on the I2S transport
# (../../transports/pico-i2s) and the host library (../../libpgpu).
cmake_minimum_required(VERSION 3.13)

# Pico 2 W (RP2350) at the SDK default 150 MHz: I2S bit clock 75 MHz
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# ninja (and idf.py, which runs it) at a core fewer than the machine has
cmake_host_system_information(RESULT PGPU_CORES QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR PGPU_JOBS "${PGPU_CORES} - 1")
if(PGPU_JOBS LESS 1)
	set(PGPU_JOBS 1)
endif()
set_property(GLOBAL PROPERTY JOB_POOLS pgpu_jobs=${PGPU_JOBS})
set(CMAKE_JOB_POOL_COMPILE pgpu_jobs)
set(CMAKE_JOB_POOL_LINK pgpu_jobs)

include(pico_sdk_import.cmake)

project(pgpu_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(DEMOS ${ROOT}/demos)
set(SHADERS ${DEMOS}/shaders)
include(${ROOT}/libpgpu/pgpu.cmake)

# the board's analog stick and game controller, for the BSP engine's demos
# (input/: picosdl's drivers and wiring; each is looked for at the start)
set(PGPU_PAD_SOURCES ${CMAKE_CURRENT_LIST_DIR}/input/pad_pico.c ${CMAKE_CURRENT_LIST_DIR}/input/joystick.c
	${CMAKE_CURRENT_LIST_DIR}/input/seesaw_gamepad.c)
set(PGPU_PAD_INCLUDES ${CMAKE_CURRENT_LIST_DIR}/input)
set(PGPU_PAD_LIBS hardware_adc hardware_i2c)

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
# linktest keeps two copies of its texture: 64 KB each here (256 KB by default)
target_compile_definitions(linktest PRIVATE LINKTEST_SIZE=128)

# ---- Jet examples on the GPU (demos/jet/jet.cmake): jet-NAME ---------------------
option(JET_PROFILE "print where the Pico's frame time goes (jet-*)" OFF)
include(${DEMOS}/jet/jet.cmake)
foreach(scene ${JET_APPS})
	pico_host_executable(jet-${scene})
	jet_demo(jet-${scene} ${scene})
endforeach()

# ---- wifi: Wi-Fi for the Pico W, asked of a phone (wifi/) ---------------------------
# The Pico as an access point shown as a QR code, a page for the network's name
# and password, the network kept in the flash. The radio's driver and lwIP run
# in the background (their own interrupts); core 1 isn't started, so the flash
# is written without asking it.
pico_host_executable(wifi wifi/wifi.c wifi/wifi_setup.c wifi/wifi_store.c wifi/net_servers.c wifi/portal.c
	wifi/qrcodegen.c)
demo_program(wifi texview -a a_pos:float:2 -v triangles)
target_include_directories(wifi PRIVATE ${CMAKE_CURRENT_LIST_DIR}/wifi)
target_link_libraries(wifi pico_cyw43_arch_lwip_threadsafe_background pico_lwip_sntp pico_rand pico_flash hardware_flash)
target_compile_definitions(wifi PRIVATE PICO_FLASH_ASSUME_CORE1_SAFE=1)
