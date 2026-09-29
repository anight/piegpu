# dEQP target "pgl" (tools/deqp/build-deqp.sh links this directory into the
# VK-GL-CTS tree as targets/pgl): GL ES 2.0 through pgl on the PC, which sends
# the commands to pigpu's RPi over USB.
message("*** Using the pgl target (pigpu)")

set(DEQP_TARGET_NAME "pgl")
set(DEQP_SUPPORT_GLES1 OFF)

set(TCUTIL_PLATFORM_SRCS ${PGL_DEQP_DIR}/tcuPglPlatform.cpp)
include_directories(${PGL_INCLUDE_DIRS} ${PGL_GENERATED_DIR})
set(DEQP_PLATFORM_LIBRARIES ${PGL_LIBRARY} m usb-1.0)
