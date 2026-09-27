# jet.cmake - the Jet examples (demos/jet, README.md) as apps any host build
# can make: jet_demo (target name) adds a scene - the JetExamples scenes
# picojet runs (examples/, MIT, CubeCoders) or picojet's model viewer
# (viewer/) - with Jet (Jet/, MIT) compiled against the scene's own
# JetConfig.hpp; Jet's pixel work goes to the GPU (gpu/JetGpu.cpp in place of
# Jet's Renderer.cpp, hooks in Scene.cpp under JET_GPU), the runtime contract
# is runtime/Runtime.cpp. Its main () calls the scene's app_main (), which is
# renamed jet_app_main here: on ESP-IDF, app_main is the system's.
#
#   JET_APPS      the scene names (apps jet-NAME)
#   JET_PROFILE   ON: print where the host's frame time goes
if(NOT COMMAND jet_demo)

set(JET_DIR ${CMAKE_CURRENT_LIST_DIR})
set(JET_APPS template-cube particles textured-boxes texture-features postfx-crt
	lod-billboards sprite-controls tropical-island sprites-blending viewer)
set(JET_TITLE_template-cube	"Rotating cube")
set(JET_TITLE_particles		"Particle Lab")
set(JET_TITLE_textured-boxes	"Textured crate")
set(JET_TITLE_texture-features	"Texture Lab")
set(JET_TITLE_postfx-crt	"CRT / Arcade")
set(JET_TITLE_lod-billboards	"Woodland")
set(JET_TITLE_sprite-controls	"Air Mail")
set(JET_TITLE_tropical-island	"Tropical island")
set(JET_TITLE_sprites-blending	"After Hours")
set(JET_TITLE_viewer		"Model viewer")

function(jet_demo target name)
	if(NOT DEFINED JET_TITLE_${name})
		message(FATAL_ERROR "jet_demo: no scene ${name} (one of: ${JET_APPS})")
	endif()
	set(src ${JET_DIR}/examples/${name})
	if(name STREQUAL "viewer")		# a scene of our own
		set(src ${JET_DIR}/viewer)
	endif()
	set(cfg ${CMAKE_CURRENT_BINARY_DIR}/jet/${name})
	# Jet includes "JetConfig.hpp" by name: this one comes first on the path
	file(WRITE ${cfg}/JetConfig.hpp
"#pragma once
#include \"${src}/firmware/JetConfig.hpp\"
#include \"${JET_DIR}/runtime/JetConfigGpu.hpp\"
")
	file(GLOB jet_sources CONFIGURE_DEPENDS ${JET_DIR}/Jet/*.cpp)
	list(REMOVE_ITEM jet_sources ${JET_DIR}/Jet/Renderer.cpp)
	file(GLOB scene_sources CONFIGURE_DEPENDS ${src}/*.cpp)
	set(sources ${scene_sources} ${jet_sources} ${JET_DIR}/gpu/JetGpu.cpp ${JET_DIR}/runtime/Runtime.cpp)
	target_sources(${target} PRIVATE ${sources})
	set_source_files_properties(${sources} PROPERTIES COMPILE_DEFINITIONS app_main=jet_app_main)
	target_include_directories(${target} PRIVATE ${cfg} ${src} ${JET_DIR}/.. ${JET_DIR}/Jet ${JET_DIR}/gpu
		${JET_DIR}/runtime)
	target_compile_features(${target} PRIVATE cxx_std_17)
	# some scenes rely on <cstdio> arriving with the ESP-IDF headers (as picojet)
	target_compile_options(${target} PRIVATE -fno-math-errno
		$<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:cstdio>)
	target_compile_definitions(${target} PRIVATE JET_GPU=1
		PICOJET_EXAMPLE_NAME="${JET_TITLE_${name}}" JET_PROFILE=$<BOOL:${JET_PROFILE}>)
	foreach(program jet jetwater)
		pgpu_glsl_program(TARGET ${target} NAME ${program} DIR ${JET_DIR}/../shaders
			ARGS -a a_pos:short:4 -a a_uv:short:2 -a a_color:ubyte_norm:4 -v triangles)
	endforeach()
endfunction()

endif()
