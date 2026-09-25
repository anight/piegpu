CIRCLEHOME = ../circle

OBJS = main.o kernel.o ../drivers/ili9486display.o ../devtools/devlink.o

LIBS = $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../drivers -I ../devtools

include $(CIRCLEHOME)/Rules.mk

-include $(DEPS)
