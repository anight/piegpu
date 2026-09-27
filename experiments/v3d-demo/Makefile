CIRCLEHOME = ../../circle

OBJS = main.o kernel.o scene.o ../../drivers/v3d.o ../../drivers/st7789dma.o ../../devtools/devlink.o ../../devtools/v3dperf.o

LIBS = $(CIRCLEHOME)/lib/usb/libusb.a \
       $(CIRCLEHOME)/lib/usb/gadget/libusbgadget.a \
       $(CIRCLEHOME)/lib/input/libinput.a \
       $(CIRCLEHOME)/lib/fs/libfs.a \
       $(CIRCLEHOME)/lib/libcircle.a

EXTRAINCLUDE = -I ../../drivers -I ../../devtools

EXTRACLEAN = shaders.h

include $(CIRCLEHOME)/Rules.mk

shaders.h: shaders.py ../../devtools/qpuasm.py
	@echo "  GEN   $@"
	@python3 shaders.py $@

scene.o: shaders.h

-include $(DEPS)
