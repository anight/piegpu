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
# glsl_program(NAME options...): NAME_program.h for the target GLSL_TARGET
# (default gpulink), from shaders/NAME.vert and .frag (GLSL_VS, if set: that
# vertex shader instead)
set(GLSL_TARGET gpulink)
function(glsl_program name)
	set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}_program.h)
	set(vs ${SHADERS}/${name}.vert)
	if(GLSL_VS)
		set(vs ${GLSL_VS})
	endif()
	get_property(defined GLOBAL PROPERTY glsl_program_${name})
	if(defined)				# already built for another target
		target_sources(${GLSL_TARGET} PRIVATE ${out})
		return()
	endif()
	set_property(GLOBAL PROPERTY glsl_program_${name} TRUE)
	add_custom_command(
		OUTPUT ${out}
		COMMAND python3 ${GLSLC_DIR}/glslc.py -n ${name}
			--vs ${vs} --fs ${SHADERS}/${name}.frag ${ARGN} -o ${out}
		DEPENDS ${vs} ${SHADERS}/${name}.frag
			${GLSLC_DIR}/glslc.py ${GLSLC_DIR}/harness.c
			${CMAKE_CURRENT_LIST_DIR}/../../devtools/qpuasm.py
			${CMAKE_CURRENT_LIST_DIR}/../../patches/mesa-vc4-dump.patch
		VERBATIM)
	target_sources(${GLSL_TARGET} PRIVATE ${out})
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

# gears: a GL ES 2.0 demo (plain gl* calls)
add_executable(gears gears.c hud.c perf.c pgpu.c pgpu_pico.c gles/pgl.c gles/pgl_compiler_none.c)
pico_generate_pio_header(gears ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
target_include_directories(gears PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol
	${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/gles ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gears pico_stdlib hardware_pio hardware_dma)
set(GLSL_TARGET gears)
glsl_program(gears -a a_pos:float:3 -a a_normal:float:3 -v triangles)
glsl_program(hud -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)
pico_enable_stdio_uart(gears 1)
pico_enable_stdio_usb(gears 0)
pico_add_extra_outputs(gears)

# breakout: a self-playing 3D Breakout (plain gl* calls)
add_executable(breakout breakout.c hud.c perf.c pgpu.c pgpu_pico.c gles/pgl.c gles/pgl_compiler_none.c)
pico_generate_pio_header(breakout ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
target_include_directories(breakout PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol
	${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/gles ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(breakout pico_stdlib hardware_pio hardware_dma)
set(GLSL_TARGET breakout)
glsl_program(blocks -a a_pos:float:3 -a a_normal:float:3 -a a_color:ubyte_norm:4 -v triangles)
glsl_program(glow -a a_pos:float:3 -a a_color:ubyte_norm:4 -v points)
glsl_program(hud -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)
pico_enable_stdio_uart(breakout 1)
pico_enable_stdio_usb(breakout 0)
pico_add_extra_outputs(breakout)

# flight: a biplane over a cloud deck, after ~/picojet's flight demo
add_executable(flight flight.c hud.c perf.c pgpu.c pgpu_pico.c gles/pgl.c gles/pgl_compiler_none.c)
pico_generate_pio_header(flight ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
target_include_directories(flight PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol
	${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/gles ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(flight pico_stdlib hardware_pio hardware_dma)
set(GLSL_TARGET flight)
glsl_program(sky -a a_pos:float:2 -v triangles)
glsl_program(deck -a a_grid:float:2 -v triangles)
glsl_program(cirrus -a a_grid:float:2 -v triangles)
glsl_program(biplane -a a_pos:short:3 -a a_uv:short:2 -a a_normal:short:3 -v triangles)
glsl_program(plume -a a_geom:float:4 -v triangles)
glsl_program(hud -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)
pico_enable_stdio_uart(flight 1)
pico_enable_stdio_usb(flight 0)
pico_add_extra_outputs(flight)

# ---- Shadertoy-style demos -------------------------------------------------------
# toy.c draws one full-screen quad; the picture is shaders/toy_NAME.frag (with
# shaders/toy.vert, Shadertoy's iTime, iResolution and fragCoord). A game adds
# its Pico side as the extra sources (toy_game.h).
function(toy_demo name caption)
	add_executable(toy-${name} toy.c ${ARGN} hud.c perf.c pgpu.c pgpu_pico.c gles/pgl.c
		gles/pgl_compiler_none.c)
	if(ARGN)
		target_compile_definitions(toy-${name} PRIVATE TOY_GAME=1)
	endif()
	pico_generate_pio_header(toy-${name} ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
	target_include_directories(toy-${name} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../protocol
		${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/gles ${CMAKE_CURRENT_BINARY_DIR})
	target_compile_definitions(toy-${name} PRIVATE TOY_HEADER="toy_${name}_program.h"
		TOY_INFO=toy_${name}_info TOY_CAPTION="${caption}")
	target_link_libraries(toy-${name} pico_stdlib hardware_pio hardware_dma)
	set(GLSL_TARGET toy-${name})
	set(GLSL_VS ${SHADERS}/toy.vert)
	glsl_program(toy_${name} -a a_pos:float:2 -v triangles)
	unset(GLSL_VS)
	glsl_program(hud -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)
	pico_enable_stdio_uart(toy-${name} 1)
	pico_enable_stdio_usb(toy-${name} 0)
	pico_add_extra_outputs(toy-${name})
endfunction()
toy_demo(tunnel  "TUNNEL")
toy_demo(spheres "SPHERES")
toy_demo(clouds  "CLOUDS")
toy_demo(voronoi "VORONOI")
# games: self-playing, the Pico keeps the state, the shader draws it all
toy_demo(pong      "PONG"      pong.c)
toy_demo(snake     "SNAKE"     snake.c)
toy_demo(asteroids "ASTEROIDS" asteroids.c)

# ---- Jet examples on the GPU ----------------------------------------------------
# The JetExamples scenes picojet runs (jet/examples, MIT, CubeCoders), each
# with Jet (jet/Jet, MIT) compiled against the scene's own JetConfig.hpp;
# Jet's pixel work goes to the GPU (jet/gpu/JetGpu.cpp in place of Jet's
# Renderer.cpp, hooks in Scene.cpp under JET_GPU), the runtime contract is
# jet/runtime/Runtime.cpp.
set(JET_DIR ${CMAKE_CURRENT_LIST_DIR}/jet)
option(JET_PROFILE "print where the Pico's frame time goes (jet-*)" OFF)
file(GLOB JET_SOURCES CONFIGURE_DEPENDS ${JET_DIR}/Jet/*.cpp)
list(REMOVE_ITEM JET_SOURCES ${JET_DIR}/Jet/Renderer.cpp)
function(jet_example name title)
	set(src ${JET_DIR}/examples/${name})
	if(ARGN)				# a scene of our own: its directory
		set(src ${ARGN})
	endif()
	set(cfg ${CMAKE_CURRENT_BINARY_DIR}/jet/${name})
	# Jet includes "JetConfig.hpp" by name: this one comes first on the path
	file(WRITE ${cfg}/JetConfig.hpp
"#pragma once
#include \"${src}/firmware/JetConfig.hpp\"
#include \"${JET_DIR}/runtime/JetConfigGpu.hpp\"
")
	file(GLOB example_sources CONFIGURE_DEPENDS ${src}/*.cpp)
	add_executable(jet-${name} ${example_sources} ${JET_SOURCES} ${JET_DIR}/gpu/JetGpu.cpp
		${JET_DIR}/runtime/Runtime.cpp hud.c perf.c pgpu.c pgpu_pico.c gles/pgl.c
		gles/pgl_compiler_none.c)
	pico_generate_pio_header(jet-${name} ${CMAKE_CURRENT_LIST_DIR}/pcm_out.pio)
	target_include_directories(jet-${name} PRIVATE ${cfg} ${src} ${JET_DIR}/Jet ${JET_DIR}/gpu
		${JET_DIR}/runtime ${CMAKE_CURRENT_LIST_DIR}/../../protocol ${CMAKE_CURRENT_LIST_DIR}
		${CMAKE_CURRENT_LIST_DIR}/gles ${CMAKE_CURRENT_BINARY_DIR})
	target_compile_features(jet-${name} PRIVATE cxx_std_17)
	# some scenes rely on <cstdio> arriving with the ESP-IDF headers (as picojet)
	target_compile_options(jet-${name} PRIVATE -fno-math-errno
		$<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:cstdio>)
	target_compile_definitions(jet-${name} PRIVATE JET_GPU=1 PICOJET_EXAMPLE_NAME="${title}"
		JET_PROFILE=$<BOOL:${JET_PROFILE}>)
	target_link_libraries(jet-${name} pico_stdlib hardware_pio hardware_dma)
	set(GLSL_TARGET jet-${name})
	glsl_program(jet -a a_pos:short:4 -a a_uv:short:2 -a a_color:ubyte_norm:4 -v triangles)
	glsl_program(jetwater -a a_pos:short:4 -a a_uv:short:2 -a a_color:ubyte_norm:4 -v triangles)
	glsl_program(hud -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)
	pico_enable_stdio_uart(jet-${name} 1)
	pico_enable_stdio_usb(jet-${name} 0)
	pico_add_extra_outputs(jet-${name})
endfunction()
jet_example(template-cube    "Rotating cube")
jet_example(particles        "Particle Lab")
jet_example(textured-boxes   "Textured crate")
jet_example(texture-features "Texture Lab")
jet_example(postfx-crt       "CRT / Arcade")
jet_example(lod-billboards   "Woodland")
jet_example(sprite-controls  "Air Mail")
jet_example(tropical-island  "Tropical island")
jet_example(sprites-blending "After Hours")
# picojet's model viewer (app/main.cpp) as a scene: jet/viewer
jet_example(viewer           "Model viewer" ${JET_DIR}/viewer)
