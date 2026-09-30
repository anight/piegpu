# the app (PGPU_APP: demos/demos.cmake, or jet-NAME: demos/jet/jet.cmake):
# its main () is called by app_main
include(${CMAKE_CURRENT_LIST_DIR}/../../../demos/demos.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../demos/jet/jet.cmake)

idf_component_register(SRCS app_main.c sdcard.c REQUIRES pgpu esp_driver_gpio esp_driver_sdmmc sdmmc fatfs)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
	if(PGPU_APP STREQUAL "pincheck")	# the pins, before wiring the RPi
		target_sources(${COMPONENT_LIB} PRIVATE pincheck.c)
	elseif(PGPU_APP STREQUAL "media")	# from the microSD card (else the file linked in)
		pgpu_demo(${COMPONENT_LIB} media)
		set(PGPU_MEDIA_FILE "/sdcard/video1.mp4" CACHE STRING "the MP4 or MP3 the media demo plays")
		target_compile_definitions(${COMPONENT_LIB} PRIVATE PGPU_SDCARD="/sdcard"
			PGPU_MEDIA_PATH="${PGPU_MEDIA_FILE}")
	elseif(PGPU_APP MATCHES "^jet-(.+)$")
		jet_demo(${COMPONENT_LIB} ${CMAKE_MATCH_1})
	else()
		pgpu_demo(${COMPONENT_LIB} ${PGPU_APP})
	endif()
	# the demos' main () becomes pgpu_app_main (); ESP-IDF starts app_main ()
	target_compile_definitions(${COMPONENT_LIB} PRIVATE main=pgpu_app_main)
endif()
