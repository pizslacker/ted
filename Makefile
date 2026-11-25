CC = cc
CFLAGS = -Wall -Wextra -pedantic -std=c99
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

ted: ted.c
	$(CC) $(CFLAGS) ted.c -o ted
	strip ted

install: ted ted.1
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 755 ted $(DESTDIR)$(BINDIR)/ted
	install -m 644 ted.1 $(DESTDIR)$(MANDIR)/ted.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/ted $(DESTDIR)$(MANDIR)/ted.1

clean:
	rm -f ted