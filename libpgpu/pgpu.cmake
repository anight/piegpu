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
#              (tools/glslc/build-mesa.sh). From pgpu_sources.cmake, which
#              also lists the sources for builds without these targets.

include(${CMAKE_CURRENT_LIST_DIR}/pgpu_sources.cmake)

add_library(pgpu OBJECT ${PGPU_SOURCES})
target_include_directories(pgpu PUBLIC ${PGPU_LIB} ${PGPU_LIB}/gles ${PGPU_ROOT}/protocol
	${PGPU_GLSL_OUT})

add_library(pgpu_nocompiler OBJECT ${PGPU_NOCOMPILER_SOURCES})
target_link_libraries(pgpu_nocompiler PUBLIC pgpu)

add_library(pgpu_hud OBJECT ${PGPU_HUD_SOURCES})
target_include_directories(pgpu_hud PUBLIC ${PGPU_LIB}/hud)
target_link_libraries(pgpu_hud PUBLIC pgpu)
pgpu_glsl_program(TARGET pgpu_hud NAME hud DIR ${PGPU_LIB}/hud ARGS ${PGPU_HUD_GLSL_ARGS})
