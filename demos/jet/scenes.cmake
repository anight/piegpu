# scenes.cmake - the Jet scenes (CubeCoders' JetExamples and picojet's model
# viewer, written again for piegpu's OpenGL: kit.hpp, README.md) as apps any
# host build can make: jet_scene (target name) adds the scene's source, the
# kit and its programs.
#
#   JET_SCENES   the scenes' names (apps jet-NAME)
if(NOT COMMAND jet_scene)

set(JET_DIR ${CMAKE_CURRENT_LIST_DIR})
set(JET_SCENES template-cube lighting-teapot depth-teapot postfx-cel textured-boxes texture-features postfx-crt
	particles lod-billboards sprite-controls sprites-blending tropical-island mesh-instancing neon-car matter
	neon-film viewer)

function(jet_scene target name)
	if(NOT EXISTS ${JET_DIR}/scenes/${name}.cpp)
		message(FATAL_ERROR "jet_scene: no scene ${name} (one of: ${JET_SCENES})")
	endif()
	set(sources ${JET_DIR}/scenes/${name}.cpp ${JET_DIR}/kit.cpp)
	target_sources(${target} PRIVATE ${sources})
	target_include_directories(${target} PRIVATE ${JET_DIR} ${JET_DIR}/..)	# (the viewer's models: demos/assets)
	target_compile_features(${target} PRIVATE cxx_std_17)
	target_compile_options(${target} PRIVATE -fno-math-errno)
	# antialiased by the V3D: the programs blend each sample (--ms)
	foreach(program kit_mesh kit_phong)
		pgpu_glsl_program(TARGET ${target} NAME ${program} DIR ${JET_DIR}/../shaders
			ARGS -a a_pos:short:4 -a a_normal:short_norm:4 -a a_color:ubyte_norm:4 -a a_uv:short:2
				-a a_material:ubyte:4 -v triangles --ms)
	endforeach()
	pgpu_glsl_program(TARGET ${target} NAME kit_sprite DIR ${JET_DIR}/../shaders
		ARGS -a a_pos:float:2 -a a_uv:float:2 -a a_color:ubyte_norm:4 -v triangles --ms)
	if(name STREQUAL "matter")		# its paper landscape and its ribbons, shaped by the vertex shader
		foreach(program kit_paper kit_ribbon)
			pgpu_glsl_program(TARGET ${target} NAME ${program} VS ${JET_DIR}/../shaders/${program}.vert
				FS ${JET_DIR}/../shaders/kit_mesh.frag
				ARGS -a a_pos:short:4 -a a_normal:short_norm:4 -a a_color:ubyte_norm:4 -a a_uv:short:2
					-a a_material:ubyte:4 -v triangles --ms)
		endforeach()
	endif()
	if(name STREQUAL "neon-film")		# its neon tubes, turned to the camera by the vertex shader
		pgpu_glsl_program(TARGET ${target} NAME kit_neon VS ${JET_DIR}/../shaders/kit_neon.vert
			FS ${JET_DIR}/../shaders/kit_mesh.frag
			ARGS -a a_pos:short:4 -a a_normal:short_norm:4 -a a_color:ubyte_norm:4 -a a_uv:short:2
				-a a_material:ubyte:4 -v triangles --ms)
	endif()
	if(name STREQUAL "tropical-island" OR name STREQUAL "neon-film")	# its sea
		pgpu_glsl_program(TARGET ${target} NAME kit_water DIR ${JET_DIR}/../shaders
			ARGS -a a_pos:short:4 -v triangles --ms)
	endif()
endfunction()

endif()
