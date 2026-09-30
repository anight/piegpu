CIRCLEHOME = ../../third_party/circle

OBJS = main.o kernel.o ../../devtools/devlink.o

LIBS = $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../../devtools

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
