#
# The gpu app, the RPi's firmware, built with CMake on Circle's CMake build
# (third_party/circle/CMakeLists.txt, the submodule): a build directory a board, e.g.
# build/zero (the Zero: kernel.img), build/zerow (the Zero W: kernel.img
# with the wireless code, PGPU_WIRELESS) and build/zero2 (the Zero 2 W:
# kernel8.img). devtools/build-gpu.sh configures and builds them:
#
#   cmake -S gpu -B build/zero2 -G Ninja -DCMAKE_TOOLCHAIN_FILE=third_party/circle/cmake/toolchain.cmake \
#         -DCIRCLE_PREFIX=<aarch64-none-elf- toolchain> -DCIRCLE_RASPPI=3 "-DCIRCLE_DEFINES=..."
#
cmake_minimum_required(VERSION 3.20)
project(piegpu_gpu C CXX ASM)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)	# (normalized: the prefix maps)
add_subdirectory(${ROOT}/third_party/circle circle)
include(${ROOT}/third_party/circle/cmake/circle-kernel.cmake)

# MMAL (video/userland, BSD-3): the client of the VideoCore's video components
set(ul video/userland/interface/mmal)
set(mmal ${ul}/core/mmal_format.c ${ul}/core/mmal_port.c ${ul}/core/mmal_port_clock.c
	 ${ul}/core/mmal_component.c ${ul}/core/mmal_buffer.c ${ul}/core/mmal_queue.c
	 ${ul}/core/mmal_pool.c ${ul}/core/mmal_events.c ${ul}/core/mmal_logging.c
	 ${ul}/core/mmal_clock.c
	 ${ul}/util/mmal_util.c ${ul}/util/mmal_connection.c ${ul}/util/mmal_list.c
	 ${ul}/util/mmal_util_params.c ${ul}/util/mmal_util_rational.c
	 ${ul}/vc/mmal_vc_client.c ${ul}/vc/mmal_vc_api.c ${ul}/vc/mmal_vc_msgnames.c
	 ${ul}/vc/mmal_vc_opaque_alloc.c ${ul}/vc/mmal_vc_shm.c)

add_executable(gpu
	main.cpp build_info.cpp kernel.cpp settings.cpp commands.cpp geometry.cpp textures.cpp programs.cpp renderer.cpp
	link/i2s_link.cpp link/byte_stream_link.cpp link/usb_link.cpp link/usb_bulk_link.cpp
	display/panel_output.cpp display/touch.cpp display/backlight.cpp display/hdmi_output.cpp display/hdmi_monitor.cpp display/gud_display.cpp
	video/video.cpp video/encode_test.cpp video/mp4_writer.cpp audio/audio.cpp audio/audio_sink.cpp audio/sounds.cpp audio/audio_out.cpp install/installer.cpp ui/settings_app.cpp
	${ROOT}/drivers/v3d.cpp ${ROOT}/drivers/v3dcheck.cpp ${ROOT}/drivers/st7789dma.cpp
	${ROOT}/devtools/devlink.cpp ${ROOT}/devtools/runlog.cpp ${ROOT}/devtools/pgpugadget.cpp
	video/compat/circle_vcos.c video/compat/circle_micros.cpp ${mmal}
	${CMAKE_CURRENT_BINARY_DIR}/shaders.h ${CMAKE_CURRENT_BINARY_DIR}/build_info_values.h)
# Bluetooth (bt/): only in the kernels of the boards with the chip
# (PGPU_WIRELESS in CIRCLE_DEFINES: devtools/build-gpu.sh)
if("PGPU_WIRELESS" IN_LIST CIRCLE_DEFINES)
	# the SBC encoder: Bluedroid's (bt/sbc_encoder, Apache 2.0), no warnings
	file(GLOB sbc_encoder bt/sbc_encoder/srce/*.c)
	add_library(sbc_encoder STATIC ${sbc_encoder})
	target_link_libraries(sbc_encoder PUBLIC circle_flags)
	target_include_directories(sbc_encoder PRIVATE bt/sbc_encoder/include ${CIRCLE_INCLUDE_DIRS})
	target_compile_options(sbc_encoder PRIVATE -w -fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/bt/=)
	target_sources(gpu PRIVATE bt/bt_uart.cpp bt/bt_hci.cpp bt/bt_l2cap.cpp bt/bt_sdp.cpp bt/bt_avdtp.cpp bt/bt_avrcp.cpp
		bt/bt_audio.cpp bt/bluetooth.cpp)
	target_link_libraries(gpu PRIVATE sbc_encoder)
endif()
target_include_directories(gpu PRIVATE ${CIRCLE_INCLUDE_DIRS}	# (Circle's first, as Rules.mk)
	${ROOT}/drivers ${ROOT}/devtools ${ROOT}/protocol ${ROOT}/third_party/circle/addon audio
	video/compat video/userland video/userland/interface/mmal
	${CMAKE_CURRENT_BINARY_DIR})			# (shaders.h, build_info_values.h)
# the MMAL sources (C; their messages 32-bit on any client: video/userland/
# interface/mmal/vc/mmal_vc_msgs.h): what they need beyond Circle's vcos and
# their warnings, for the app's every source (as the Makefile had it). In 64
# bit, MMAL reads its messages where VCHIQ has them, in Circle's coherent
# region: Device memory, where an unaligned access faults (GCC merges
# neighbouring fields into one); Circle's VCHIQ is built the same way
target_compile_options(gpu PRIVATE
	$<$<COMPILE_LANGUAGE:C,CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/video/compat/mmal_circle.h
	-Wno-format -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
	-Wno-unused-but-set-variable $<$<EQUAL:${CIRCLE_AARCH},64>:-mstrict-align>>
	# __FILE__ as the Makefile's build in gpu/ gave it
	-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=
	-fmacro-prefix-map=${ROOT}/drivers/=../drivers/ -fmacro-prefix-map=${ROOT}/devtools/=../devtools/)

# the shaders (gpu/shaders.py: the fixed function's QPU programs)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shaders.h
	COMMAND python3 shaders.py ${CMAKE_CURRENT_BINARY_DIR}/shaders.h
	DEPENDS shaders.py ${ROOT}/devtools/qpuasm.py
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# this build's version, time and configuration (build_info.h), at every build
add_custom_target(build_info_values
	COMMAND ${ROOT}/devtools/build-info.sh ${CMAKE_CURRENT_BINARY_DIR}/build_info_values.h ${ROOT}/third_party/circle
	BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/build_info_values.h)
add_dependencies(gpu build_info_values)

# the audio decoders, a library of their own flags: AAC, audio/aac.c with
# FAAD2 (audio/faad2, GPL 2 or later): its shims first (audio/faad2/compat),
# its defines, no warnings; libfaad's sources with the C library shim
# force-included. MP3, audio/mp3.c with minimp3 (audio/minimp3, CC0, a header).
# Ogg Vorbis, audio/vorbis.c with Tremor and libogg (audio/tremor, BSD-3-Clause:
# <ogg/ogg.h> from there; alloca is the compiler's; its C library shim
# force-included)
# LVGL (third_party/circle/addon/lvgl/lvgl, MIT): the Settings app's widgets
# (gpu/ui), its configuration gpu/ui/lv_conf.h
file(GLOB_RECURSE lvgl_sources ${ROOT}/third_party/circle/addon/lvgl/lvgl/src/*.c)
add_library(lvgl STATIC ${lvgl_sources})
target_link_libraries(lvgl PUBLIC circle_flags)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ui ${ROOT}/third_party/circle/addon/lvgl/lvgl
	PRIVATE ${CIRCLE_INCLUDE_DIRS})
target_compile_options(lvgl PRIVATE -w)

file(GLOB libfaad audio/faad2/libfaad/*.c)
file(GLOB tremor audio/tremor/*.c audio/tremor/libogg/*.c)
add_library(audio_codecs STATIC ${libfaad} audio/faad2/compat/faad_circle.c audio/aac.c audio/aac_enc.c audio/mp3.c
	${tremor} audio/tremor/compat/tremor_circle.c audio/vorbis.c)
target_link_libraries(audio_codecs PUBLIC circle_flags)
target_include_directories(audio_codecs PRIVATE audio/faad2/compat ${CIRCLE_INCLUDE_DIRS} audio/faad2 audio/faad2/libfaad
	audio/tremor)
set_source_files_properties(${tremor} PROPERTIES
	COMPILE_OPTIONS "-Dalloca=__builtin_alloca;-include;${CMAKE_CURRENT_SOURCE_DIR}/audio/tremor/compat/tremor_circle.h")
target_compile_options(audio_codecs PRIVATE -DHAVE_INTTYPES_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMCPY=1 -DHAVE_LRINTF=1
	"-DPACKAGE_VERSION=\"2.11.3\"" -w $<$<EQUAL:${CIRCLE_AARCH},64>:-mstrict-align>
	-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/audio/faad2/=
	-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/audio/=)
set_source_files_properties(${libfaad} PROPERTIES
	COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/audio/faad2/compat/faad_circle.h")

target_link_libraries(gpu PRIVATE audio_codecs lvgl circle_sdcard circle_fatfs circle_vcos circle_vchiqsound circle_vchiq
	circle_linuxemu circle_usb circle_usbgadget circle_input circle_fs circle_sched circle_sound circle)
circle_kernel(gpu)
