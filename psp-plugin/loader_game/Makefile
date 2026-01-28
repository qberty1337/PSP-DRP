TARGET = psp_drp_loader_game
OBJS = src/main.o

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

PSP_FW_VERSION = 661

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -fno-strict-aliasing
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspuser

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

clean-all: clean
	rm -f *.prx *.elf
