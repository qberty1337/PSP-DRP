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
# USB functionality moved to separate kernel PRX (psp_drp_usb.prx)
# -lpspusb is needed for sceUsbGetState to check connection status
LIBS = -lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpspnet \
       -lpspwlan -lpsputility -lpsppower -lpspumd -lpsprtc -lpspusb \
       -lpspuser -lc -lgcc

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

clean-all: clean
	rm -f *.prx *.elf
