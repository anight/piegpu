cmake_minimum_required(VERSION 3.13)

# Pico 2 W (RP2350) at the SDK default 150 MHz: I2S bit clock 75 MHz
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(gpulink C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(gpulink main.c pgpu.c pgpu_pico.c gltest.c gles/pgl.c gles/pgl_compiler_none.c)
pico_generate_pio_header(gpulink ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
target_include_directories(gpulink PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol
	${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/gles)
target_link_libraries(gpulink pico_stdlib hardware_pio hardware_dma)

# GLSL programs, precompiled by tools/glslc (needs the host Mesa built by
# tools/glslc/build-mesa.sh)
set(GLSLC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../tools/glslc)
set(SHADERS ${CMAKE_CURRENT_LIST_DIR}/shaders)
function(glsl_program name)
	set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}_program.h)
	add_custom_command(
		OUTPUT ${out}
		COMMAND python3 ${GLSLC_DIR}/glslc.py -n ${name}
			--vs ${SHADERS}/${name}.vert --fs ${SHADERS}/${name}.frag ${ARGN} -o ${out}
		DEPENDS ${SHADERS}/${name}.vert ${SHADERS}/${name}.frag
			${GLSLC_DIR}/glslc.py ${GLSLC_DIR}/harness.c
			${CMAKE_CURRENT_LIST_DIR}/../../devtools/qpuasm.py
			${CMAKE_CURRENT_LIST_DIR}/../../patches/mesa-vc4-dump.patch
		VERBATIM)
	target_sources(gpulink PRIVATE ${out})
endfunction()
glsl_program(plasma -a a_pos:float:2 -v triangles)
glsl_program(cube -a a_pos:float:3 -a a_normal:byte_norm:3 -a a_uv:float:2 -v triangles)
glsl_program(sparks -a a_pos:float:3 -a a_color:ubyte_norm:4 -v points)
glsl_program(solid -a a_pos:float:2 -v triangles -v lines)
glsl_program(builtin -a a_pos:float:2 -v triangles -v points)
glsl_program(texview -a a_pos:float:2 -v triangles)
glsl_program(gltest -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4)
glsl_program(ctrl -a a_pos:float:2)
target_include_directories(gpulink PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# console on UART0 (GP0/GP1) -> Debugprobe UART bridge
pico_enable_stdio_uart(gpulink 1)
pico_enable_stdio_usb(gpulink 0)
