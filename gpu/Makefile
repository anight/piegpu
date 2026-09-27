CIRCLEHOME = ../circle

OBJS = main.o kernel.o commands.o geometry.o textures.o programs.o renderer.o \
       link/i2s_link.o link/usb_link.o display/panel_output.o display/hdmi_output.o display/hdmi_monitor.o \
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
