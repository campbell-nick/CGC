#!/usr/bin/env bash
# Install/uninstall test: stage an install into a temporary DESTDIR and verify
# the binary, manual page, and shell completions are placed correctly.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"
make -s cgc

DEST="$WORK/stage"
PREFIX=/usr/local

make -s install DESTDIR="$DEST" PREFIX="$PREFIX"

test -x "$DEST$PREFIX/bin/cgc"
test -f "$DEST$PREFIX/share/man/man1/cgc.1"
test -f "$DEST$PREFIX/share/bash-completion/completions/cgc"
test -f "$DEST$PREFIX/share/zsh/site-functions/_cgc"
test -f "$DEST$PREFIX/share/fish/vendor_completions.d/cgc.fish"

bash -n "$DEST$PREFIX/share/bash-completion/completions/cgc"

make -s uninstall DESTDIR="$DEST" PREFIX="$PREFIX"

test ! -e "$DEST$PREFIX/bin/cgc"
test ! -e "$DEST$PREFIX/share/man/man1/cgc.1"
test ! -e "$DEST$PREFIX/share/bash-completion/completions/cgc"
test ! -e "$DEST$PREFIX/share/zsh/site-functions/_cgc"
test ! -e "$DEST$PREFIX/share/fish/vendor_completions.d/cgc.fish"

echo "install test passed"
