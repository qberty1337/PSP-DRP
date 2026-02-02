TARGET = psp_drp_net
OBJS = src/main.o src/game_detect.o src/network.o src/config.o src/sfo.o src/syscalls.o src/usage_tracker.o

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

PSP_FW_VERSION = 661

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -Wno-unused-variable -fno-strict-aliasing
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpspnet \
       -lpspwlan -lpsputility -lpsppower -lpspumd -lpsprtc \
       -lpspuser -lc -lgcc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

clean-all: clean
	rm -f *.prx *.elf
