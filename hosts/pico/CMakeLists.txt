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

# ---- self tests: the link, the protocol and pgl (main.c, gltest.c) -----------------
pico_host_executable(gpulink main.c ${ROOT}/libpgpu/test/gltest.c)
demo_program(gpulink plasma -a a_pos:float:2 -v triangles)
demo_program(gpulink cube -a a_pos:float:3 -a a_normal:byte_norm:3 -a a_uv:float:2 -v triangles)
demo_program(gpulink sparks -a a_pos:float:3 -a a_color:ubyte_norm:4 -v points)
demo_program(gpulink solid -a a_pos:float:2 -v triangles -v lines)
demo_program(gpulink texview -a a_pos:float:2 -v triangles)
foreach(p builtin gltest ctrl)
	set(args_builtin -a a_pos:float:2 -v triangles -v points)
	set(args_gltest -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4)
	set(args_ctrl -a a_pos:float:2)
	pgpu_glsl_program(TARGET gpulink NAME ${p} DIR ${ROOT}/libpgpu/test ARGS ${args_${p}})
endforeach()

# ---- demos (plain gl* calls) ---------------------------------------------------------
# gears: three meshing gears, after glxgears
pico_host_executable(gears ${DEMOS}/gears.c)
demo_program(gears gears -a a_pos:float:3 -a a_normal:float:3 -v triangles)

# breakout: a self-playing 3D Breakout
pico_host_executable(breakout ${DEMOS}/breakout.c)
demo_program(breakout blocks -a a_pos:float:3 -a a_normal:float:3 -a a_color:ubyte_norm:4 -v triangles)
demo_program(breakout glow -a a_pos:float:3 -a a_color:ubyte_norm:4 -v points)

# flight: a biplane over a cloud deck, after ~/picojet's flight demo
pico_host_executable(flight ${DEMOS}/flight.c)
demo_program(flight sky -a a_pos:float:2 -v triangles)
demo_program(flight deck -a a_grid:float:2 -v triangles)
demo_program(flight cirrus -a a_grid:float:2 -v triangles)
demo_program(flight biplane -a a_pos:short:3 -a a_uv:short:2 -a a_normal:short:3 -v triangles)
demo_program(flight plume -a a_geom:float:4 -v triangles)

# ---- Shadertoy-style demos -------------------------------------------------------
# toy.c draws one full-screen quad; the picture is shaders/toy_NAME.frag (with
# shaders/toy.vert, Shadertoy's iTime, iResolution and fragCoord). A game adds
# its Pico side as the extra sources (toy_game.h).
function(toy_demo name caption)
	set(sources)
	foreach(s ${ARGN})
		list(APPEND sources ${DEMOS}/${s})
	endforeach()
	pico_host_executable(toy-${name} ${DEMOS}/toy.c ${sources})
	if(ARGN)
		target_compile_definitions(toy-${name} PRIVATE TOY_GAME=1)
	endif()
	target_compile_definitions(toy-${name} PRIVATE TOY_HEADER="toy_${name}_program.h"
		TOY_INFO=toy_${name}_info TOY_CAPTION="${caption}")
	pgpu_glsl_program(TARGET toy-${name} NAME toy_${name} VS ${SHADERS}/toy.vert DIR ${SHADERS}
		ARGS -a a_pos:float:2 -v triangles)
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
# The JetExamples scenes picojet runs (demos/jet/examples, MIT, CubeCoders),
# each with Jet (demos/jet/Jet, MIT) compiled against the scene's own
# JetConfig.hpp; Jet's pixel work goes to the GPU (jet/gpu/JetGpu.cpp in place
# of Jet's Renderer.cpp, hooks in Scene.cpp under JET_GPU), the runtime
# contract is jet/runtime/Runtime.cpp.
set(JET_DIR ${DEMOS}/jet)
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
	pico_host_executable(jet-${name} ${example_sources} ${JET_SOURCES} ${JET_DIR}/gpu/JetGpu.cpp
		${JET_DIR}/runtime/Runtime.cpp)
	target_include_directories(jet-${name} PRIVATE ${cfg} ${src} ${JET_DIR}/Jet ${JET_DIR}/gpu
		${JET_DIR}/runtime)
	target_compile_features(jet-${name} PRIVATE cxx_std_17)
	# some scenes rely on <cstdio> arriving with the ESP-IDF headers (as picojet)
	target_compile_options(jet-${name} PRIVATE -fno-math-errno
		$<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:cstdio>)
	target_compile_definitions(jet-${name} PRIVATE JET_GPU=1 PICOJET_EXAMPLE_NAME="${title}"
		JET_PROFILE=$<BOOL:${JET_PROFILE}>)
	demo_program(jet-${name} jet -a a_pos:short:4 -a a_uv:short:2 -a a_color:ubyte_norm:4 -v triangles)
	demo_program(jet-${name} jetwater -a a_pos:short:4 -a a_uv:short:2 -a a_color:ubyte_norm:4 -v triangles)
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
