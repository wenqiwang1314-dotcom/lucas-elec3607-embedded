CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c99 -O2
LDLIBS ?= -li2c -lm -lrt

all: wspr_transmit

wspr_transmit: wspr_transmit.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Build this on a laptop only to check argument parsing and WSPR symbols.
# It does not access /dev/i2c-*.
mock: wspr_transmit.c
	$(CC) $(CFLAGS) -DNO_I2C -o wspr_transmit_mock $< -lm

clean:
	rm -f wspr_transmit wspr_transmit_mock
