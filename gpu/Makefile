# the Circle tree of the board (devtools/configure-circle.sh; devtools/
# build-gpu.sh builds a Zero 2 W's with CIRCLEHOME=../circle-zero2)
CIRCLEHOME ?= ../circle
-include $(CIRCLEHOME)/Config.mk		# (AARCH)
AARCH ?= 32

OBJS = main.o build_info.o kernel.o commands.o geometry.o textures.o programs.o renderer.o \
       link/i2s_link.o link/byte_stream_link.o link/usb_link.o link/usb_bulk_link.o display/panel_output.o display/hdmi_output.o display/hdmi_monitor.o display/gud_display.o video/video.o audio/audio.o audio/audio_out.o install/installer.o \
       ../drivers/v3d.o ../drivers/v3dcheck.o ../drivers/st7789dma.o ../devtools/devlink.o ../devtools/runlog.o ../devtools/pgpugadget.o \
       video/compat/circle_vcos.o video/compat/circle_micros.o $(MMAL)

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

LIBS = audio/faad2/libaac.a \
       $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
       $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
       $(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a \
       $(CIRCLEHOME)/addon/vc4/sound/libvchiqsound.a \
       $(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a \
       $(CIRCLEHOME)/addon/linux/liblinuxemu.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/sound/libsound.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../drivers -I ../devtools -I ../protocol -I $(CIRCLEHOME)/addon -I audio \
	       -I video/compat -I video/userland -I video/userland/interface/mmal

# the MMAL sources (C; their messages 32-bit on any client: video/userland/
# interface/mmal/vc/mmal_vc_msgs.h): what they need beyond Circle's vcos, and
# their warnings
CFLAGS += -include video/compat/mmal_circle.h -Wno-format -Wno-unused-parameter \
	  -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-but-set-variable
ifeq ($(strip $(AARCH)),64)
# MMAL reads its messages where VCHIQ has them, in Circle's coherent region:
# Device memory in 64 bit, where an unaligned access faults (GCC merges
# neighbouring fields into one); Circle's VCHIQ is built the same way
CFLAGS += -mstrict-align
endif

EXTRACLEAN = shaders.h $(MMAL) $(MMAL:.o=.d) video/compat/*.o video/compat/*.d

include $(CIRCLEHOME)/Rules.mk

# AAC (audio/aac.c with FAAD2, audio/faad2: GPL 2 or later): a library of its
# own, built with its own flags
audio/faad2/libaac.a: FORCE
	$(MAKE) -C audio/faad2 CIRCLEHOME=$(abspath $(CIRCLEHOME))

# this build's version, time and configuration (build_info.h), at every build.
# The version: the checkout's VERSION (major.minor.patch) and a build number,
# the next of this checkout's (devtools/next-build.sh), unless PGPU_BUILD is
# given (devtools/build-gpu.sh: one number for all its boards)
TOPDIR := $(shell git rev-parse --show-toplevel 2>/dev/null)
PGPU_BUILD ?= $(shell $(TOPDIR)/devtools/next-build.sh)
build_info.o: CPPFLAGS += -DPGPU_FW_VERSION='"$(shell cat $(TOPDIR)/VERSION).$(PGPU_BUILD)"' \
	-DPGPU_FW_GIT='"$(shell git describe --always --dirty 2>/dev/null || echo unknown)"' \
	-DPGPU_BUILT='"$(shell date -u +%Y-%m-%dT%H:%M:%SZ)"' \
	-DPGPU_CIRCLE='"$(shell git -C $(CIRCLEHOME) describe --tags --always 2>/dev/null || echo unknown)"'
build_info.o: FORCE
.PHONY: FORCE
FORCE:

shaders.h: shaders.py ../devtools/qpuasm.py
	@echo "  GEN   $@"
	@python3 shaders.py $@

renderer.o geometry.o: shaders.h

-include $(DEPS)
