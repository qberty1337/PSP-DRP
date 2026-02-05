#include <pspmoduleexport.h>
#define NULL ((void *) 0)

extern int module_start;
extern int module_stop;
static const unsigned int __syslib_exports[4] __attribute__((section(".rodata.sceResident"))) = {
	0xD632ACDB,
	0xCEE8593C,
	(unsigned int) &module_start,
	(unsigned int) &module_stop,
};

extern int usb_drp_init;
extern int usb_drp_start;
extern int usb_drp_stop;
extern int usb_drp_shutdown;
extern int usb_drp_is_connected;
extern int usb_drp_send;
extern int usb_drp_receive;
extern int usb_drp_send_game_info;
extern int usb_drp_send_heartbeat;
extern int usb_drp_poll_message;
static const unsigned int __psp_drp_usb_exports[20] __attribute__((section(".rodata.sceResident"))) = {
	0x340AF80F,
	0xC7961D89,
	0xFFEAA6A8,
	0x8C3C9335,
	0x30B3BDD6,
	0x9709088E,
	0x5BCD39B2,
	0x76B54230,
	0x01CC2721,
	0xA1D38098,
	(unsigned int) &usb_drp_init,
	(unsigned int) &usb_drp_start,
	(unsigned int) &usb_drp_stop,
	(unsigned int) &usb_drp_shutdown,
	(unsigned int) &usb_drp_is_connected,
	(unsigned int) &usb_drp_send,
	(unsigned int) &usb_drp_receive,
	(unsigned int) &usb_drp_send_game_info,
	(unsigned int) &usb_drp_send_heartbeat,
	(unsigned int) &usb_drp_poll_message,
};

const struct _PspLibraryEntry __library_exports[2] __attribute__((section(".lib.ent"), used)) = {
	{ NULL, 0x0000, 0x8000, 4, 0, 2, (unsigned int *) &__syslib_exports },
	{ "psp_drp_usb", 0x0000, 0x0001, 4, 0, 10, (unsigned int *) &__psp_drp_usb_exports },
};
