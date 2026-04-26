DECODER_DIR=pa-wsprcan

all:
	$(MAKE) -C $(DECODER_DIR)

run: all
	bash wsprwait

clean:
	$(MAKE) -C parecfile clean
	$(MAKE) -C wsprcan clean
	$(MAKE) -C $(DECODER_DIR) clean
	-rm -f ALL_WSPR.TXT wspr_spots.txt wspr_timer.out wsprwait.out
