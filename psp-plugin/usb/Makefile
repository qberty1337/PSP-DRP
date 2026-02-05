TARGET = psp_drp_usb
OBJS = src/main.o src/usb_driver.o src/usb_protocol.o src/game_detect.o src/sfo.o src/config.o

BUILD_PRX = 1
PRX_EXPORTS = exports.exp

# Kernel mode for USB driver APIs
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

PSP_FW_VERSION = 661

INCDIR =
CFLAGS = -O2 -G0 -Wall -Wextra -fno-strict-aliasing
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspusb_driver -lpspusbbus_driver -lpspumd_driver -lpsprtc_driver

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build_prx.mak

clean-all: clean
	rm -f *.prx *.elf
