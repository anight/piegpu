CIRCLEHOME = ../circle

OBJS = main.o kernel.o receiver.o commands.o geometry.o textures.o renderer.o \
       ../drivers/v3d.o ../drivers/st7789dma.o ../devtools/devlink.o

LIBS = $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../drivers -I ../devtools -I ../protocol

EXTRACLEAN = shaders.h

include $(CIRCLEHOME)/Rules.mk

shaders.h: shaders.py ../devtools/qpuasm.py
	@echo "  GEN   $@"
	@python3 shaders.py $@

renderer.o geometry.o: shaders.h

-include $(DEPS)
