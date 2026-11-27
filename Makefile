CC       ?= gcc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Werror -pedantic
CPPFLAGS  = -MMD -MP -Isrc
LDFLAGS  ?=

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man/man1

SRCS      := $(wildcard src/*.c)
OBJS      := $(SRCS:.c=.o)
CORE_OBJS := src/buffer.o src/syntax.o src/edit.o src/fileio.o
TEST_SRC  := tests/test_ted.c
TEST_OBJ  := $(TEST_SRC:.c=.o)
DEPS      := $(OBJS:.o=.d) $(TEST_OBJ:.o=.d)

TARGET      := ted
TEST_TARGET := test_ted

.PHONY: all clean install uninstall check sanitize

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/%.o: tests/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TEST_TARGET): $(CORE_OBJS) $(TEST_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(CORE_OBJS) $(TEST_OBJ)

check: $(TEST_TARGET)
	./$(TEST_TARGET)

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" check
	$(MAKE) clean

install: all
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 644 ted.1 $(DESTDIR)$(MANDIR)/ted.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) $(DESTDIR)$(MANDIR)/ted.1

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJS) $(TEST_OBJ) $(DEPS)

-include $(DEPS)