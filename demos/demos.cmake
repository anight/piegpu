# demos.cmake - the demos (and the self tests) as apps any host build can
# make: pgpu_demo (target app) adds the app's sources, definitions and GLSL
# programs to a target. The host provides main()'s surroundings: the Pico
# SDK (hosts/pico) or pico/stdlib.h from libpgpu/compat (hosts/esp32p4).
#
#   selftest            the link, the protocol and pgl (libpgpu/test)
#   linktest            the link at full speed both ways, checked word by word
#   gears breakout flight
#   toy-NAME            Shadertoy-style: shaders/toy_NAME.frag; a game has
#                       NAME.c (pong, snake, asteroids)
#   video               an MP4 played by the Zero's decoder into a texture:
#                       PGPU_VIDEO_MP4 (default: a 720p test pattern made with
#                       ffmpeg), linked in
#
# The Jet demos (demos/jet) are C++ with their own runtime: hosts/pico only.
include(${CMAKE_CURRENT_LIST_DIR}/../libpgpu/pgpu_sources.cmake)

set(PGPU_DEMOS ${CMAKE_CURRENT_LIST_DIR})
set(PGPU_DEMO_SHADERS ${PGPU_DEMOS}/shaders)
set(PGPU_DEMO_APPS selftest linktest gears breakout flight
	toy-tunnel toy-spheres toy-clouds toy-voronoi toy-pong toy-snake toy-asteroids video)

if(NOT COMMAND pgpu_demo)
function(pgpu_demo target app)
	target_include_directories(${target} PRIVATE ${PGPU_DEMOS})
	macro(program name)
		pgpu_glsl_program(TARGET ${target} NAME ${name} DIR ${PGPU_DEMO_SHADERS} ARGS ${ARGN})
	endmacro()

	if(app STREQUAL "selftest")
		target_sources(${target} PRIVATE ${PGPU_LIB}/test/selftest.c ${PGPU_LIB}/test/gltest.c)
		program(plasma -a a_pos:float:2 -v triangles)
		program(cube -a a_pos:float:3 -a a_normal:byte_norm:3 -a a_uv:float:2 -v triangles)
		program(sparks -a a_pos:float:3 -a a_color:ubyte_norm:4 -v points)
		program(solid -a a_pos:float:2 -v triangles -v lines)
		program(texview -a a_pos:float:2 -v triangles)
		pgpu_glsl_program(TARGET ${target} NAME builtin DIR ${PGPU_LIB}/test
			ARGS -a a_pos:float:2 -v triangles -v points)
		pgpu_glsl_program(TARGET ${target} NAME gltest DIR ${PGPU_LIB}/test
			ARGS -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4)
		pgpu_glsl_program(TARGET ${target} NAME ctrl DIR ${PGPU_LIB}/test ARGS -a a_pos:float:2)

	elseif(app STREQUAL "linktest")
		target_sources(${target} PRIVATE ${PGPU_LIB}/test/linktest.c)

	elseif(app STREQUAL "gears")		# three meshing gears, after glxgears
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/gears.c)
		program(gears -a a_pos:float:3 -a a_normal:float:3 -v triangles)

	elseif(app STREQUAL "breakout")	# a self-playing 3D Breakout
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/breakout.c)
		program(blocks -a a_pos:float:3 -a a_normal:float:3 -a a_color:ubyte_norm:4 -v triangles)
		program(glow -a a_pos:float:3 -a a_color:ubyte_norm:4 -v points)

	elseif(app STREQUAL "flight")		# a biplane over a cloud deck, after ~/picojet's
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/flight.c)
		program(sky -a a_pos:float:2 -v triangles)
		program(deck -a a_grid:float:2 -v triangles)
		program(cirrus -a a_grid:float:2 -v triangles)
		program(biplane -a a_pos:short:3 -a a_uv:short:2 -a a_normal:short:3 -v triangles)
		program(plume -a a_geom:float:4 -v triangles)

	elseif(app MATCHES "^toy-(.+)$")	# toy.c: one full-screen quad, the picture is the shader
		set(name ${CMAKE_MATCH_1})
		string(TOUPPER ${name} caption)
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/toy.c)
		if(EXISTS ${PGPU_DEMOS}/${name}.c)	# a game: its state and play (toy_game.h)
			target_sources(${target} PRIVATE ${PGPU_DEMOS}/${name}.c)
			target_compile_definitions(${target} PRIVATE TOY_GAME=1)
		endif()
		target_compile_definitions(${target} PRIVATE TOY_HEADER="toy_${name}_program.h"
			TOY_INFO=toy_${name}_info TOY_CAPTION="${caption}")
		pgpu_glsl_program(TARGET ${target} NAME toy_${name} VS ${PGPU_DEMO_SHADERS}/toy.vert
			DIR ${PGPU_DEMO_SHADERS} ARGS -a a_pos:float:2 -v triangles)

	elseif(app STREQUAL "video" AND EMSCRIPTEN)	# a page's: the MP4 is the page's file
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/video.c ${PGPU_DEMOS}/video_mp4_none.c)
		target_compile_definitions(${target} PRIVATE PGPU_VIDEO_PATH="/video.mp4")
		program(video -a a_pos:float:2 -v triangles)

	elseif(app STREQUAL "video")		# an MP4 into a video texture (video.c)
		set(mp4 "${PGPU_VIDEO_MP4}")
		if(NOT mp4)
			set(mp4 ${CMAKE_BINARY_DIR}/video-test.mp4)
			add_custom_command(OUTPUT ${mp4}
				COMMAND ffmpeg -hide_banner -loglevel error -f lavfi
					-i testsrc=size=1280x720:rate=30 -t 10 -c:v libx264 -profile:v high
					-level 4.0 -pix_fmt yuv420p -bf 2 -g 60 -b:v 2M -y ${mp4}
				COMMENT "Making the test video (ffmpeg)")
		endif()
		set(PGPU_VIDEO_MP4 ${mp4})
		configure_file(${PGPU_DEMOS}/video_mp4.S.in ${CMAKE_CURRENT_BINARY_DIR}/video_mp4.S @ONLY)
		set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/video_mp4.S PROPERTIES OBJECT_DEPENDS ${mp4})
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/video.c ${CMAKE_CURRENT_BINARY_DIR}/video_mp4.S)
		program(video -a a_pos:float:2 -v triangles)

	else()
		message(FATAL_ERROR "pgpu_demo: no app ${app} (one of: ${PGPU_DEMO_APPS})")
	endif()
endfunction()
endif()
