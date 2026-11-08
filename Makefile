CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += $(shell pkg-config --cflags zlib openssl 2>/dev/null)
LDLIBS  += $(shell pkg-config --libs zlib openssl 2>/dev/null || echo -lz -lssl -lcrypto)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

cgc: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) cgc

install: cgc
	install -m 755 cgc /usr/local/bin/cgc

.PHONY: clean install
