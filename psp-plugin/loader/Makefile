TARGET = psp_drp_loader
OBJS = src/main.o

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

PSP_FW_VERSION = 661

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -fno-strict-aliasing -DAUTO_START_NET=1
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspuser

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

clean-all: clean
	rm -f *.prx *.elf
