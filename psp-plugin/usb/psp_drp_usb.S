	.set noreorder

#include "pspstub.s"

	STUB_START "psp_drp_usb",0x00090000,0x000A0005
	STUB_FUNC  0x340AF80F,usb_drp_init
	STUB_FUNC  0xC7961D89,usb_drp_start
	STUB_FUNC  0xFFEAA6A8,usb_drp_stop
	STUB_FUNC  0x8C3C9335,usb_drp_shutdown
	STUB_FUNC  0x30B3BDD6,usb_drp_is_connected
	STUB_FUNC  0x9709088E,usb_drp_send
	STUB_FUNC  0x5BCD39B2,usb_drp_receive
	STUB_FUNC  0x76B54230,usb_drp_send_game_info
	STUB_FUNC  0x01CC2721,usb_drp_send_heartbeat
	STUB_FUNC  0xA1D38098,usb_drp_poll_message
	STUB_END
