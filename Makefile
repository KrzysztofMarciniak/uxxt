CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -Wall -Wextra
PREFIX  ?= /usr/local

all: uxxt-logic uxxt-x11 uxxt-buffer uxxt-keys

uxxt-logic: logic/uxxt-logic.c logic/config.h common/common.h
	$(CC) $(CFLAGS) -o $@ logic/uxxt-logic.c -lvterm

uxxt-x11: x11/uxxt-x11.c x11/config.h common/common.h
	$(CC) $(CFLAGS) -o $@ x11/uxxt-x11.c -lX11

uxxt-buffer: buffer/uxxt-buffer.c buffer/config.h common/common.h
	$(CC) $(CFLAGS) -o $@ buffer/uxxt-buffer.c

uxxt-keys: keys/uxxt-keys.c keys/config.h common/common.h
	$(CC) $(CFLAGS) -o $@ keys/uxxt-keys.c

install: all
	install -m 755 uxxt-logic uxxt-x11 uxxt-buffer uxxt-keys uxxt $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f uxxt-logic uxxt-x11 uxxt-buffer uxxt-keys

.PHONY: all install clean
