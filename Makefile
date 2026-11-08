CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += $(shell pkg-config --cflags zlib openssl 2>/dev/null)
LDLIBS  += $(shell pkg-config --libs zlib openssl 2>/dev/null || echo -lz -lssl -lcrypto)

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

cgc: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: cgc
	./tests/smoke.sh

clean:
	rm -f $(OBJ) cgc

install: cgc
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 cgc "$(DESTDIR)$(BINDIR)/cgc"
	install -d "$(DESTDIR)$(MANDIR)"
	install -m 644 cgc.1 "$(DESTDIR)$(MANDIR)/cgc.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/cgc"
	rm -f "$(DESTDIR)$(MANDIR)/cgc.1"

.PHONY: clean install uninstall test
