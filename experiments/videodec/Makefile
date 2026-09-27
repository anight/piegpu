#
# videodec: H.264 on the VideoCore's decoder from bare metal (kernel.cpp).
#
# MMAL is raspberrypi/userland's (BSD-3), cloned into third_party/ (not in git):
#   git clone --depth 1 https://github.com/raspberrypi/userland.git third_party/userland
# (a54a0db); built here with Circle's vcos and VCHIQ plus compat/. One fix is
# applied to it: mmal_vc_opaque_alloc.c's definitions use its header's type.
#
CIRCLEHOME = ../../circle
UL = ../../third_party/userland/interface/mmal

MMAL = $(UL)/core/mmal_format.o $(UL)/core/mmal_port.o $(UL)/core/mmal_port_clock.o \
       $(UL)/core/mmal_component.o $(UL)/core/mmal_buffer.o $(UL)/core/mmal_queue.o \
       $(UL)/core/mmal_pool.o $(UL)/core/mmal_events.o $(UL)/core/mmal_logging.o \
       $(UL)/core/mmal_clock.o \
       $(UL)/util/mmal_util.o $(UL)/util/mmal_connection.o $(UL)/util/mmal_list.o \
       $(UL)/util/mmal_util_params.o $(UL)/util/mmal_util_rational.o \
       $(UL)/vc/mmal_vc_client.o $(UL)/vc/mmal_vc_api.o $(UL)/vc/mmal_vc_msgnames.o \
       $(UL)/vc/mmal_vc_opaque_alloc.o $(UL)/vc/mmal_vc_shm.o

OBJS = main.o kernel.o stream.o compat/circle_vcos.o ../../devtools/devlink.o $(MMAL)

LIBS = $(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a \
       $(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a \
       $(CIRCLEHOME)/addon/linux/liblinuxemu.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I compat -I ../../third_party/userland -I ../../third_party/userland/interface/mmal -I ../../devtools
CFLAGS += -include compat/mmal_circle.h -Wno-format -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers

include $(CIRCLEHOME)/Rules.mk

$(UL)/vc/mmal_vc_opaque_alloc.o: $(UL)/vc/.circle-fixed
$(UL)/vc/.circle-fixed:
	sed -i 's/(unsigned int handle)/(MMAL_OPAQUE_IMAGE_HANDLE_T handle)/' $(UL)/vc/mmal_vc_opaque_alloc.c
	touch $@

# the test stream: ffmpeg's test pattern (a seconds counter), 10 s of 720p30,
# High profile with B-frames
stream.o: test720.h264
test720.h264:
	ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=1280x720:rate=30 -t 10 \
		-c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p -bf 2 -g 60 -b:v 2M -f h264 -y $@

EXTRACLEAN = compat/*.o compat/*.d

-include $(DEPS)
