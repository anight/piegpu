# pgl (the GL ES 2.0 / 1.1 API) on a PC: the commands go to the Zero over its
# USB link (pgpu_host.c) instead of the Pico's I2S link. For tests.
cmake_minimum_required(VERSION 3.13)
project(pgl_host C)
set(CMAKE_C_STANDARD 11)

set(GPULINK ${CMAKE_CURRENT_LIST_DIR}/..)
set(ROOT ${GPULINK}/../..)

add_library(pgl STATIC ${GPULINK}/pgpu.c pgpu_host.c ${GPULINK}/gles/pgl.c pgl_compiler_glslc.c)
target_compile_definitions(pgl PRIVATE PGL_GLSLC="${ROOT}/tools/glslc/glslc.py"
	PGL_SHADOW_LIMIT=0)		# a PC has the memory: copies of all buffers
target_include_directories(pgl PUBLIC ${GPULINK} ${GPULINK}/gles ${ROOT}/protocol)
set_property(TARGET pgl PROPERTY POSITION_INDEPENDENT_CODE ON)
target_compile_options(pgl PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(pgl PUBLIC m)

# GLSL programs, precompiled by tools/glslc (as in ../CMakeLists.txt)
set(GLSLC_DIR ${ROOT}/tools/glslc)
set(SHADERS ${GPULINK}/shaders)
function(glsl_program name)
	set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}_program.h)
	add_custom_command(
		OUTPUT ${out}
		COMMAND python3 ${GLSLC_DIR}/glslc.py -n ${name}
			--vs ${SHADERS}/${name}.vert --fs ${SHADERS}/${name}.frag ${ARGN} -o ${out}
		DEPENDS ${SHADERS}/${name}.vert ${SHADERS}/${name}.frag
			${GLSLC_DIR}/glslc.py ${GLSLC_DIR}/harness.c
			${ROOT}/devtools/qpuasm.py ${ROOT}/patches/mesa-vc4-dump.patch
		VERBATIM)
	target_sources(gltest_host PRIVATE ${out})
endfunction()

# self test 8 on the PC
add_executable(gltest_host gltest_main.c ${GPULINK}/gltest.c)
target_include_directories(gltest_host PRIVATE include ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gltest_host pgl)
glsl_program(gltest -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4)
glsl_program(ctrl -a a_pos:float:2)
glsl_program(builtin -a a_pos:float:2 -v triangles -v points)

# shaders compiled at run time (tools/glslc)
add_executable(compile_test compile_test.c)
target_link_libraries(compile_test pgl)
