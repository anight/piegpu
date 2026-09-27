# pgpu.cmake - the host library, for any host build (hosts/pico, hosts/pc, a
# future board): include() it from the board's CMakeLists.txt.
#
#   pgpu            the link protocol (pgpu.c) and the GL ES 2.0 / 1.1 API
#                   (pgl); board independent. The board adds a transport
#                   (transports/*, pgpu_link.h) and a pgl shader compiler back
#                   end: pgpu_nocompiler (precompiled programs only) or
#                   tools/glslc (transports/pc-usb, on a PC).
#   pgpu_nocompiler pgl without a run-time GLSL compiler
#   pgpu_hud        the demos' HUD (frame rate, loads); needs pgpu.
#
# They are object libraries: pgpu and its transport call each other, which
# static libraries would make depend on the link order.
#
#   pgpu_glsl_program (TARGET t NAME n [VS file] [FS file] [DIR d] [ARGS ...])
#              precompile a GLSL program with tools/glslc into n_program.h
#              for target t (the sources: d/n.vert and d/n.frag unless VS or
#              FS say otherwise; ARGS go to glslc). Needs the host Mesa
#              (tools/glslc/build-mesa.sh).

set(PGPU_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(PGPU_LIB ${CMAKE_CURRENT_LIST_DIR})
set(PGPU_GLSLC ${PGPU_ROOT}/tools/glslc)
set(PGPU_GLSL_OUT ${CMAKE_BINARY_DIR}/glsl)

add_library(pgpu OBJECT ${PGPU_LIB}/pgpu.c ${PGPU_LIB}/gles/pgl.c)
target_include_directories(pgpu PUBLIC ${PGPU_LIB} ${PGPU_LIB}/gles ${PGPU_ROOT}/protocol
	${PGPU_GLSL_OUT})

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

add_library(pgpu_nocompiler OBJECT ${PGPU_LIB}/gles/pgl_compiler_none.c)
target_link_libraries(pgpu_nocompiler PUBLIC pgpu)

add_library(pgpu_hud OBJECT ${PGPU_LIB}/hud/hud.c ${PGPU_LIB}/hud/perf.c)
target_include_directories(pgpu_hud PUBLIC ${PGPU_LIB}/hud)
target_link_libraries(pgpu_hud PUBLIC pgpu)
pgpu_glsl_program(TARGET pgpu_hud NAME hud DIR ${PGPU_LIB}/hud
	ARGS -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles)
