# the Circle tree of the board (devtools/configure-circle.sh; devtools/
# build-gpu.sh builds a Zero 2 W's with CIRCLEHOME=../circle-zero2)
CIRCLEHOME ?= ../circle
-include $(CIRCLEHOME)/Config.mk		# (AARCH: video only in 32-bit builds)
AARCH ?= 32

OBJS = main.o build_info.o kernel.o commands.o geometry.o textures.o programs.o renderer.o \
       link/i2s_link.o link/byte_stream_link.o link/usb_link.o link/usb_bulk_link.o display/panel_output.o display/hdmi_output.o display/hdmi_monitor.o display/gud_display.o install/installer.o \
       ../drivers/v3d.o ../drivers/st7789dma.o ../devtools/devlink.o ../devtools/pgpugadget.o

# MMAL (video/userland, BSD-3): the client of the VideoCore's video components
UL = video/userland/interface/mmal
MMAL = $(UL)/core/mmal_format.o $(UL)/core/mmal_port.o $(UL)/core/mmal_port_clock.o \
       $(UL)/core/mmal_component.o $(UL)/core/mmal_buffer.o $(UL)/core/mmal_queue.o \
       $(UL)/core/mmal_pool.o $(UL)/core/mmal_events.o $(UL)/core/mmal_logging.o \
       $(UL)/core/mmal_clock.o \
       $(UL)/util/mmal_util.o $(UL)/util/mmal_connection.o $(UL)/util/mmal_list.o \
       $(UL)/util/mmal_util_params.o $(UL)/util/mmal_util_rational.o \
       $(UL)/vc/mmal_vc_client.o $(UL)/vc/mmal_vc_api.o $(UL)/vc/mmal_vc_msgnames.o \
       $(UL)/vc/mmal_vc_opaque_alloc.o $(UL)/vc/mmal_vc_shm.o

LIBS = $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/linux/liblinuxemu.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../drivers -I ../devtools -I ../protocol -I $(CIRCLEHOME)/addon \
	       -I video/compat -I video/userland -I video/userland/interface/mmal

ifeq ($(strip $(AARCH)),32)
# video: MMAL over the VideoCore's services (Circle's vcos is 32 bit only)
OBJS += video/video.o video/compat/circle_vcos.o video/compat/circle_micros.o $(MMAL)
LIBS := $(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a $(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a $(LIBS)
DEFINE += -DPGPU_VIDEO

# the MMAL sources (C): what they need beyond Circle's vcos, and their warnings
CFLAGS += -include video/compat/mmal_circle.h -Wno-format -Wno-unused-parameter \
	  -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-but-set-variable
else
OBJS += video/video_none.o
endif

EXTRACLEAN = shaders.h $(MMAL) $(MMAL:.o=.d) video/compat/*.o video/compat/*.d

include $(CIRCLEHOME)/Rules.mk

# this build's version, time and configuration (build_info.h), at every build
build_info.o: CPPFLAGS += -DPGPU_VERSION='"$(shell git describe --always --dirty 2>/dev/null || echo unknown)"' \
	-DPGPU_BUILT='"$(shell date -u +%Y-%m-%dT%H:%MZ)"' \
	-DPGPU_CIRCLE='"$(shell git -C $(CIRCLEHOME) describe --tags --always 2>/dev/null || echo unknown)"'
build_info.o: FORCE
.PHONY: FORCE
FORCE:

shaders.h: shaders.py ../devtools/qpuasm.py
	@echo "  GEN   $@"
	@python3 shaders.py $@

renderer.o geometry.o: shaders.h

-include $(DEPS)
