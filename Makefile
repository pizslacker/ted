CC       ?= gcc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS  = -MMD -MP -Isrc
LDFLAGS  ?=

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man/man1

SRCS     := $(wildcard src/*.c)
OBJS     := $(SRCS:.c=.o)
DEPS     := $(OBJS:.o=.d)
TARGET   := ted

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

-include $(DEPS)

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 644 ted.1 $(DESTDIR)$(MANDIR)/ted.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(MANDIR)/ted.1

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)