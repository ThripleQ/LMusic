CC = gcc
CFLAGS = -O2
LIBS = -lasound -lncurses -lpthread -lavformat -lavcodec -lavutil -lswresample
PREFIX ?= /usr/local

lmusic: player.c decoder.c decoder.h netease.c netease.h song.h
	$(CC) $(CFLAGS) player.c decoder.c netease.c $(LIBS) -o lmusic

install: lmusic
	cp lmusic $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lmusic

clean:
	rm -f lmusic player

.PHONY: install uninstall clean
