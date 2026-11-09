CC         ?= cc
PKG_CONFIG ?= pkg-config
CLANG_TIDY ?= clang-tidy

CFLAGS   ?= -O2 -g
CPPFLAGS ?=
LDFLAGS  ?=
LDLIBS   ?=

VERSION ?= $(shell cat VERSION 2>/dev/null || echo 0.1.0)

STDFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L
WARNFLAGS := -Wall -Wextra -Wno-unused-parameter
ALL_CFLAGS := $(CFLAGS) $(STDFLAGS) $(WARNFLAGS)

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags zlib 2>/dev/null) \
              $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs zlib 2>/dev/null) \
              $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)
ifeq ($(strip $(shell $(PKG_CONFIG) --libs zlib 2>/dev/null)),)
PKG_LIBS += -lz
endif
ifeq ($(strip $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null)),)
PKG_LIBS += -lssl -lcrypto
endif

ALL_CPPFLAGS := $(CPPFLAGS) -Isrc $(PKG_CFLAGS) -DCGC_VERSION='"$(VERSION)"'
ALL_LDLIBS   := $(LDLIBS) $(PKG_LIBS)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man/man1

SRC      := $(wildcard src/*.c)
OBJ      := $(SRC:.c=.o)
LIB_OBJ  := $(filter-out src/main.o,$(OBJ))
UNIT     := tests/unit
PACKTEST := tests/packtest
DISTNAME := cgc-$(VERSION)

all: cgc

cgc: $(OBJ)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(ALL_LDLIBS)

%.o: %.c
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -c -o $@ $<

$(UNIT): tests/unit.c $(LIB_OBJ)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ \
		tests/unit.c $(LIB_OBJ) $(ALL_LDLIBS)

$(PACKTEST): tests/packtest.c src/pack.o src/odb.o src/util.o
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) $(LDFLAGS) -o $@ \
		tests/packtest.c src/pack.o src/odb.o src/util.o $(ALL_LDLIBS)

check: $(UNIT) $(PACKTEST)
	./$(UNIT)
	./tests/pack.sh

# Curated clang-tidy checks: focus on real correctness and safety findings and
# exclude the insecureAPI style checks, which flag ordinary memcpy/memset use.
ANALYZE_CHECKS ?= -*,clang-analyzer-core.*,clang-analyzer-unix.*,clang-analyzer-security.*,-clang-analyzer-security.insecureAPI.*,clang-analyzer-deadcode.*

analyze:
	$(CLANG_TIDY) -checks='$(ANALYZE_CHECKS)' $(SRC) -- \
		$(ALL_CPPFLAGS) $(ALL_CFLAGS)

test: check cgc
	./tests/smoke.sh

dist:
	rm -rf "dist/$(DISTNAME)" "dist/$(DISTNAME).tar.gz"
	mkdir -p "dist/$(DISTNAME)"
	tar -cf - --exclude='*.o' --exclude='tests/unit' \
		--exclude='tests/packtest' --exclude='dist' \
		Makefile VERSION README.md LICENSE CHANGELOG.md cgc.1 \
		docs tests src .gitignore | tar -xf - -C "dist/$(DISTNAME)"
	tar -czf "dist/$(DISTNAME).tar.gz" -C dist "$(DISTNAME)"
	rm -rf "dist/$(DISTNAME)"
	@echo "created dist/$(DISTNAME).tar.gz"

install: cgc
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 cgc "$(DESTDIR)$(BINDIR)/cgc"
	install -d "$(DESTDIR)$(MANDIR)"
	install -m 644 cgc.1 "$(DESTDIR)$(MANDIR)/cgc.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/cgc"
	rm -f "$(DESTDIR)$(MANDIR)/cgc.1"

clean:
	rm -f $(OBJ) cgc $(UNIT) $(PACKTEST)

distclean: clean
	rm -rf dist

.PHONY: all analyze check clean dist distclean install test uninstall
