# the host library (libpgpu: pgpu, pgl without a run-time compiler, the HUD)
# on the ESP32-P4 I2S transport, as one ESP-IDF component
include(${CMAKE_CURRENT_LIST_DIR}/../../../../libpgpu/pgpu_sources.cmake)
file(MAKE_DIRECTORY ${PGPU_GLSL_OUT})

idf_component_register(
	SRCS ${PGPU_SOURCES} ${PGPU_NOCOMPILER_SOURCES} ${PGPU_HUD_SOURCES}
		${PGPU_ROOT}/transports/esp32p4-i2s/pgpu_esp32p4.c
	INCLUDE_DIRS ${PGPU_INCLUDE_DIRS} ${PGPU_COMPAT_INCLUDE_DIR} ${PGPU_GLSL_OUT}
	REQUIRES esp_driver_i2s esp_driver_gpio esp_timer esp_mm freertos log)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
	if(PGPU_P4_BCLK_HZ)			# idf.py -DPGPU_P4_BCLK_HZ=...: the I2S bit clock
		target_compile_definitions(${COMPONENT_LIB} PRIVATE PGPU_P4_BCLK_HZ=${PGPU_P4_BCLK_HZ})
	endif()
	if(DEFINED PGPU_P4_DRIVE)		# -DPGPU_P4_DRIVE=0..3: the outputs' drive strength
		target_compile_definitions(${COMPONENT_LIB} PRIVATE PGPU_P4_DRIVE=${PGPU_P4_DRIVE})
	endif()
	pgpu_glsl_program(TARGET ${COMPONENT_LIB} NAME hud DIR ${PGPU_LIB}/hud ARGS ${PGPU_HUD_GLSL_ARGS})
endif()
