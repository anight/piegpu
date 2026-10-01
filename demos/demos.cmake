# demos.cmake - the demos (and the self tests) as apps any host build can
# make: pgpu_demo (target app) adds the app's sources, definitions and GLSL
# programs to a target. The host provides main()'s surroundings: the Pico
# SDK (hosts/pico) or pico/stdlib.h from libpgpu/compat (hosts/esp32p4).
#
#   selftest            the link, the protocol and pgl (libpgpu/test)
#   linktest            the link at full speed both ways, checked word by word
#   gears breakout flight
#   antigrav            anti-gravity racing, after WipEout: six craft, a circuit
#   walk                the BSP engine (engine/): a Quake-format level walked
#                       through; PGPU_LEVEL (default engine/levels/base.bsp)
#   keep                the BSP engine outdoors: a castle at dusk, gems to
#                       find (default engine/levels/keep.bsp)
#   isles               the BSP engine in the sky: floating islands, a moving
#                       platform, a jump pad, a portal, coins (engine/levels/isles.bsp)
#   touch               the panel's touch screen: a drawing board
#   toy-NAME            Shadertoy-style: shaders/toy_NAME.frag; a game has
#                       NAME.c (pong, snake, asteroids)
#   media               an MP4 or an MP3 played by the RPi: the video decoded
#                       into a texture, the sound (AAC, MP3) on HDMI;
#                       PGPU_MEDIA_EMBED (default: a 720p test pattern made
#                       with ffmpeg), linked in, or a file (PGPU_MEDIA_PATH)
#
# The Jet demos (demos/jet) are C++ with their own runtime: demos/jet/jet.cmake
# (hosts/pico and hosts/esp32p4).
include(${CMAKE_CURRENT_LIST_DIR}/../libpgpu/pgpu_sources.cmake)

set(PGPU_DEMOS ${CMAKE_CURRENT_LIST_DIR})
set(PGPU_DEMO_SHADERS ${PGPU_DEMOS}/shaders)
set(PGPU_LEVEL_walk base)			# the BSP engine's demos' levels (engine/levels)
set(PGPU_LEVEL_keep keep)
set(PGPU_LEVEL_isles isles)
set(PGPU_DEMO_APPS selftest linktest gears breakout flight antigrav walk keep isles touch
	toy-tunnel toy-spheres toy-clouds toy-voronoi toy-pong toy-snake toy-asteroids media)

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

	elseif(app STREQUAL "antigrav")	# anti-gravity racing, after WipEout
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/antigrav.c)
		program(sky -a a_pos:float:2 -v triangles)
		program(scenery -a a_pos:float:3 -a a_uv:float:2 -a a_shade:float:1 -v triangles)
		program(craft -a a_pos:float:3 -a a_normal:float:3 -a a_uv:float:2 -v triangles)
		program(nightsky -a a_pos:float:2 -a a_dir:float:3 -v triangles)

	elseif(app STREQUAL "walk" OR app STREQUAL "keep" OR app STREQUAL "isles")	# the BSP engine
		set(engine ${PGPU_DEMOS}/../engine)
		if(NOT PGPU_LEVEL)
			set(PGPU_LEVEL ${engine}/levels/${PGPU_LEVEL_${app}}.bsp)
		endif()
		set(PGPU_LEVEL_INCBIN ${PGPU_LEVEL})
		configure_file(${engine}/level_file.S.in ${CMAKE_CURRENT_BINARY_DIR}/level_file_${app}.S @ONLY)
		set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/level_file_${app}.S PROPERTIES OBJECT_DEPENDS ${PGPU_LEVEL})
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/${app}.c ${engine}/bsp.c ${engine}/render.c ${engine}/collide.c
			${engine}/game.c ${engine}/keys.c ${engine}/palette.c ${CMAKE_CURRENT_BINARY_DIR}/level_file_${app}.S)
		target_include_directories(${target} PRIVATE ${engine})
		pgpu_glsl_program(TARGET ${target} NAME world DIR ${engine}/shaders
			ARGS -a a_pos:float:3 -a a_uv:float:2 -a a_luv:float:2 -v triangles)
		pgpu_glsl_program(TARGET ${target} NAME liquid DIR ${engine}/shaders
			ARGS -a a_pos:float:3 -a a_uv:float:2 -v triangles)
		pgpu_glsl_program(TARGET ${target} NAME skydome DIR ${engine}/shaders
			ARGS -a a_pos:float:3 -v triangles)
		if(NOT app STREQUAL "walk")		# items to pick up
			target_sources(${target} PRIVATE ${PGPU_DEMOS}/pickups.c)
			program(gem -a a_pos:float:3 -a a_normal:float:3 -v triangles)
			program(halo -a a_corner:float:2 -v triangles)
		endif()

	elseif(app STREQUAL "touch")		# the touch screen: a drawing board
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/touch.c)
		program(brush -a a_pos:float:2 -v triangles)
		program(texview -a a_pos:float:2 -v triangles)

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

	elseif(app STREQUAL "media" AND EMSCRIPTEN)	# a page's: the file is the page's
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/media.c ${PGPU_DEMOS}/media_file_none.c)
		target_compile_definitions(${target} PRIVATE PGPU_MEDIA_PATH="/media")
		program(video -a a_pos:float:2 -v triangles)

	elseif(app STREQUAL "media")		# an MP4 or an MP3, linked in (media.c)
		set(file "${PGPU_MEDIA_EMBED}")
		if(NOT file)
			set(file ${CMAKE_BINARY_DIR}/video-test.mp4)
			add_custom_command(OUTPUT ${file}
				COMMAND ffmpeg -hide_banner -loglevel error -f lavfi
					-i testsrc=size=1280x720:rate=30 -t 10 -c:v libx264 -profile:v high
					-level 4.0 -pix_fmt yuv420p -bf 2 -g 60 -b:v 2M -y ${file}
				COMMENT "Making the test video (ffmpeg)")
		endif()
		set(PGPU_MEDIA_INCBIN ${file})
		configure_file(${PGPU_DEMOS}/media_file.S.in ${CMAKE_CURRENT_BINARY_DIR}/media_file.S @ONLY)
		set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/media_file.S PROPERTIES OBJECT_DEPENDS ${file})
		target_sources(${target} PRIVATE ${PGPU_DEMOS}/media.c ${CMAKE_CURRENT_BINARY_DIR}/media_file.S)
		program(video -a a_pos:float:2 -v triangles)

	else()
		message(FATAL_ERROR "pgpu_demo: no app ${app} (one of: ${PGPU_DEMO_APPS})")
	endif()
endfunction()
endif()
