# hosts/pc - a PC as the host: pgl (the GL ES 2.0 / 1.1 API) with the
# commands going to the RPi over its USB link (../../transports/pc-usb) and
# GLSL compiled at run time (tools/glslc). For tests: libpgl.a for dEQP
# (tools/deqp), gltest_host (self test 8), compile_test.
cmake_minimum_required(VERSION 3.13)
project(pgpu_pc C ASM)	# ASM: the media demo links its file in (demos/media_file.S.in)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${ROOT}/libpgpu/pgpu.cmake)

# a PC has the memory: copies of all buffers
target_compile_definitions(pgpu PRIVATE PGL_SHADOW_LIMIT=0)
set_property(TARGET pgpu PROPERTY POSITION_INDEPENDENT_CODE ON)
target_compile_options(pgpu PRIVATE -Wall -Wno-unused-parameter)

# everything in one static library, for linking into dEQP
add_library(pgl STATIC ${ROOT}/transports/pc-usb/pgpu_host.c ${ROOT}/transports/pc-usb/pgl_compiler_glslc.c
	$<TARGET_OBJECTS:pgpu>)
target_compile_definitions(pgl PRIVATE PGL_GLSLC="${ROOT}/tools/glslc/glslc.py")
target_include_directories(pgl PUBLIC ${ROOT}/libpgpu ${ROOT}/libpgpu/gles ${ROOT}/protocol)
set_property(TARGET pgl PROPERTY POSITION_INDEPENDENT_CODE ON)
target_compile_options(pgl PRIVATE -Wall -Wno-unused-parameter)
# the RPi's GL interface
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
target_include_directories(pgl PRIVATE ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(pgl PUBLIC m ${LIBUSB_LIBRARIES})

# self test 8 on the PC
add_executable(gltest_host gltest_main.c ${ROOT}/libpgpu/test/gltest.c)
target_include_directories(gltest_host PRIVATE ${PGPU_COMPAT_INCLUDE_DIR} ${CMAKE_BINARY_DIR}/glsl)
target_link_libraries(gltest_host pgl)
pgpu_glsl_program(TARGET gltest_host NAME gltest DIR ${ROOT}/libpgpu/test
	ARGS -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4)
pgpu_glsl_program(TARGET gltest_host NAME ctrl DIR ${ROOT}/libpgpu/test ARGS -a a_pos:float:2)
pgpu_glsl_program(TARGET gltest_host NAME builtin DIR ${ROOT}/libpgpu/test
	ARGS -a a_pos:float:2 -v triangles -v points)

# shaders compiled at run time (tools/glslc)
add_executable(compile_test compile_test.c)
target_link_libraries(compile_test pgl)

# video: an MP4's H.264 track played by the RPi into a texture (videoplay FILE.mp4)
add_executable(videoplay videoplay.c)
target_link_libraries(videoplay pgl)

# the link at full speed both ways, checked word by word (libpgpu/test/linktest.c:
# over USB here, where the Pico and the P4 use I2S)
add_executable(linktest_host ${ROOT}/libpgpu/test/linktest.c)
target_include_directories(linktest_host PRIVATE ${PGPU_COMPAT_INCLUDE_DIR})
target_compile_definitions(linktest_host PRIVATE LINKTEST_SECONDS=30)
target_link_libraries(linktest_host pgl)

# demos (demos/demos.cmake) on the PC: DEMO_host. The HUD's objects only (its
# pgpu is in pgl already); frames paced by pgpu_wait_frame (the PC transport:
# a PING after each frame)
include(${ROOT}/demos/demos.cmake)
foreach(app gears breakout flight antigrav walk keep isles touch toy-tunnel toy-spheres toy-clouds toy-voronoi toy-pong toy-snake toy-asteroids media)
	add_executable(${app}_host $<TARGET_OBJECTS:pgpu_hud>)
	target_include_directories(${app}_host PRIVATE ${PGPU_COMPAT_INCLUDE_DIR} ${ROOT}/libpgpu/hud
		${CMAKE_BINARY_DIR}/glsl)
	target_link_libraries(${app}_host pgl)
	pgpu_demo(${app}_host ${app})
endforeach()
# media_host plays the file PGPU_MEDIA names (an MP4, an Ogg or an MP3), else the one
# linked in
target_compile_definitions(media_host PRIVATE PGPU_MEDIA_ENV=1)
# the BSP engine's demos take their keys from the terminal as they're pressed
target_compile_definitions(walk_host PRIVATE ENGINE_RAW_TERMINAL=1)
target_compile_definitions(keep_host PRIVATE ENGINE_RAW_TERMINAL=1)
target_compile_definitions(isles_host PRIVATE ENGINE_RAW_TERMINAL=1)
