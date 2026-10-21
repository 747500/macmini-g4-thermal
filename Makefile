PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall

g4temp: g4temp.c
	$(CC) $(CFLAGS) -o $@ $< -framework IOKit -framework CoreFoundation

install: g4temp
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 g4temp $(DESTDIR)$(PREFIX)/bin/g4temp

clean:
	rm -f g4temp

.PHONY: install clean
