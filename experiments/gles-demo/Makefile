CIRCLEHOME = ../../circle

OBJS = main.o kernel.o renderer.o ../../drivers/ili9486display.o ../../devtools/devlink.o ../../devtools/v3dperf.o

LIBS = $(CIRCLEHOME)/addon/vc4/interface/bcm_host/libbcm_host.a \
       $(CIRCLEHOME)/addon/vc4/interface/khronos/libkhrn_client.a \
       $(CIRCLEHOME)/addon/vc4/interface/vmcs_host/libvmcs_host.a \
       $(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a \
       $(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a \
       $(CIRCLEHOME)/addon/linux/liblinuxemu.a \
       $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/sched/libsched.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../../drivers -I ../../devtools

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
