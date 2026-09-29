# hosts/web - a web page as the host: the demos compiled to WebAssembly
# (Emscripten), their commands going to the RPi over the page's Web Serial
# port (web/installer: its Test OpenGL and Test video buttons; web/installer/
# gl.js). The transport is the PC's serial one (transports/pc-usb/
# pgpu_host.c) with the page moving the bytes; the demos' blocking waits run
# with Asyncify. Shaders are precompiled at build time (tools/glslc), as for
# the Pico.
#
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -S hosts/web -B hosts/web/build && make -C hosts/web/build -j$(($(nproc) - 1))
#
# The output (DEMO.js, DEMO.wasm) goes to web/installer/demos/: gears, and
# video (its MP4 put at /video.mp4 by the page).
cmake_minimum_required(VERSION 3.13)
project(pgpu_web C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${ROOT}/libpgpu/pgpu.cmake)
target_compile_definitions(pgpu PRIVATE PGL_SHADOW_LIMIT=0)	# (the page has the memory)
set(OUT ${ROOT}/web/installer/demos)

include(${ROOT}/demos/demos.cmake)
foreach(app gears video)
	add_executable(${app}_web ${ROOT}/transports/pc-usb/pgpu_host.c
		$<TARGET_OBJECTS:pgpu> $<TARGET_OBJECTS:pgpu_nocompiler> $<TARGET_OBJECTS:pgpu_hud>)
	target_include_directories(${app}_web PRIVATE ${PGPU_INCLUDE_DIRS} ${PGPU_COMPAT_INCLUDE_DIR}
		${CMAKE_BINARY_DIR}/glsl)
	pgpu_demo(${app}_web ${app})
	# an ES module exporting a factory; main () runs when it's called
	target_link_options(${app}_web PRIVATE -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536
		-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createDemo -sENVIRONMENT=web,node
		-sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1 -sSTACK_SIZE=262144
		-sFORCE_FILESYSTEM=1 -sEXPORTED_RUNTIME_METHODS=FS)	# (the page's files: video.mp4)
	set_target_properties(${app}_web PROPERTIES OUTPUT_NAME ${app} SUFFIX .js)
	add_custom_command(TARGET ${app}_web POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT}
		COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:${app}_web>/${app}.js
			$<TARGET_FILE_DIR:${app}_web>/${app}.wasm ${OUT}/)
endforeach()
