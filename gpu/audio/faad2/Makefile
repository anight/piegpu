#
# FAAD2's libfaad (README.piegpu) and gpu/audio/aac.c for Circle: libaac.a,
# which the gpu app links (gpu/Makefile). Floating point, all of FAAD2's
# object types (LC, HE-AAC's SBR and PS, ...).
#
CIRCLEHOME ?= ../../../circle

OBJS = $(patsubst %.c,%.o,$(wildcard libfaad/*.c)) compat/faad_circle.o ../aac.o

libaac.a: $(OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(OBJS)

# compat/ first (Rules.mk appends Circle's include directories)
INCLUDE = -I compat

include $(CIRCLEHOME)/Rules.mk

CFLAGS += -DHAVE_INTTYPES_H=1 -DHAVE_STRING_H=1 -DHAVE_MEMCPY=1 -DHAVE_LRINTF=1 \
	  -DPACKAGE_VERSION='"2.11.3"' -I . -I libfaad -w
libfaad/%.o: CFLAGS += -include compat/faad_circle.h
ifeq ($(strip $(AARCH)),64)
CFLAGS += -mstrict-align
endif

EXTRACLEAN = libaac.a ../aac.o ../aac.d

-include $(DEPS)
