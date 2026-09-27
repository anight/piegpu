# pgpu_sources.cmake - the host library's sources and pgpu_glsl_program(),
# without targets: for build systems that make their own (ESP-IDF components,
# hosts/esp32p4). pgpu.cmake adds the targets for plain CMake builds.
# The variables are set in each including scope (ESP-IDF components have
# their own); the function is defined once.

set(PGPU_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(PGPU_LIB ${CMAKE_CURRENT_LIST_DIR})
set(PGPU_GLSLC ${PGPU_ROOT}/tools/glslc)
set(PGPU_GLSL_OUT ${CMAKE_BINARY_DIR}/glsl)	# the generated NAME_program.h

set(PGPU_SOURCES ${PGPU_LIB}/pgpu.c ${PGPU_LIB}/pgpu_mp4.c ${PGPU_LIB}/gles/pgl.c)
set(PGPU_NOCOMPILER_SOURCES ${PGPU_LIB}/gles/pgl_compiler_none.c)
set(PGPU_HUD_SOURCES ${PGPU_LIB}/hud/hud.c ${PGPU_LIB}/hud/perf.c)
set(PGPU_INCLUDE_DIRS ${PGPU_LIB} ${PGPU_LIB}/gles ${PGPU_LIB}/hud ${PGPU_ROOT}/protocol)
# pico/stdlib.h for hosts other than the Pico (the demos and self tests use it)
set(PGPU_COMPAT_INCLUDE_DIR ${PGPU_LIB}/compat)
set(PGPU_HUD_GLSL_ARGS -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)

if(NOT COMMAND pgpu_glsl_program)
function(pgpu_glsl_program)
	cmake_parse_arguments(G "" "TARGET;NAME;VS;FS;DIR" "ARGS" ${ARGN})
	if(NOT G_DIR)
		set(G_DIR ${CMAKE_CURRENT_SOURCE_DIR})
	endif()
	if(NOT G_VS)
		set(G_VS ${G_DIR}/${G_NAME}.vert)
	endif()
	if(NOT G_FS)
		set(G_FS ${G_DIR}/${G_NAME}.frag)
	endif()
	set(out ${PGPU_GLSL_OUT}/${G_NAME}_program.h)
	get_property(defined GLOBAL PROPERTY pgpu_glsl_${G_NAME})
	if(NOT defined)				# one command per program, however many targets use it
		set_property(GLOBAL PROPERTY pgpu_glsl_${G_NAME} TRUE)
		add_custom_command(
			OUTPUT ${out}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${PGPU_GLSL_OUT}
			COMMAND python3 ${PGPU_GLSLC}/glslc.py -n ${G_NAME}
				--vs ${G_VS} --fs ${G_FS} ${G_ARGS} -o ${out}
			DEPENDS ${G_VS} ${G_FS} ${PGPU_GLSLC}/glslc.py ${PGPU_GLSLC}/harness.c
				${PGPU_ROOT}/devtools/qpuasm.py ${PGPU_ROOT}/patches/mesa-vc4-dump.patch
			VERBATIM)
	endif()
	target_sources(${G_TARGET} PRIVATE ${out})
endfunction()
endif()
