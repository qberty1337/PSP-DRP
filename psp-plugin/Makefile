all:
	$(MAKE) -C loader
	$(MAKE) -C loader_game
	$(MAKE) -C usb
	$(MAKE) -C net
	cp loader/psp_drp_loader.prx .
	cp loader_game/psp_drp_loader_game.prx .
	cp usb/psp_drp_usb.prx .
	cp net/psp_drp_net.prx .

clean:
	$(MAKE) -C loader clean
	$(MAKE) -C loader_game clean
	$(MAKE) -C usb clean
	$(MAKE) -C net clean

clean-all: clean
	$(MAKE) -C loader clean-all
	$(MAKE) -C loader_game clean-all
	$(MAKE) -C usb clean-all
	$(MAKE) -C net clean-all
	rm -f psp_drp_loader.prx psp_drp_loader_game.prx psp_drp_net.prx psp_drp_usb.prx

