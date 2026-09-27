# hosts/pc - a PC as the host: pgl (the GL ES 2.0 / 1.1 API) with the
# commands going to the Zero over its USB link (../../transports/pc-usb) and
# GLSL compiled at run time (tools/glslc). For tests: libpgl.a for dEQP
# (tools/deqp), gltest_host (self test 8), compile_test.
cmake_minimum_required(VERSION 3.13)
project(pgpu_pc C)
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
target_link_libraries(pgl PUBLIC m)

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

# video: an MP4's H.264 track played by the Zero into a texture (videoplay FILE.mp4)
add_executable(videoplay videoplay.c)
target_link_libraries(videoplay pgl)
