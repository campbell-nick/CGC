#!/usr/bin/env bash
# Install/uninstall test: stage an install into a temporary DESTDIR, verify
# the binary, manual page, and shell completions, check the post-install
# hints, and exercise the optional per-user shell integration.
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

# A staged manual page must be discoverable when MANPATH points at the staged
# tree. Skip if man is not installed in the test environment.
if command -v man >/dev/null 2>&1; then
    found=$(MANPATH="$DEST$PREFIX/share/man" man -w cgc)
    test "$found" = "$DEST$PREFIX/share/man/man1/cgc.1"
fi

# A non-system prefix without DESTDIR should print hints for MANPATH and the
# shell completion directories.
USERPREFIX="$WORK/userprefix"
hints=$(make -s install PREFIX="$USERPREFIX" 2>&1)
grep -q 'MANPATH' <<<"$hints"
grep -q 'install-shell' <<<"$hints"
test -x "$USERPREFIX/bin/cgc"
test -f "$USERPREFIX/share/man/man1/cgc.1"

# Optional per-user shell integration. Use a temporary HOME so the developer's
# real dotfiles are untouched.
HOME_DIR="$WORK/home"
mkdir -p "$HOME_DIR"
HOME="$HOME_DIR" make -s install-shell MANDIR="$USERPREFIX/share/man/man1"

BASH_USER="$HOME_DIR/.local/share/bash-completion/completions"
ZSH_USER="$HOME_DIR/.local/share/zsh/site-functions"
FISH_USER="$HOME_DIR/.config/fish/completions"
test -f "$BASH_USER/cgc"
test -f "$ZSH_USER/_cgc"
test -f "$FISH_USER/cgc.fish"

grep -q '>>> cgc shell integration >>>' "$HOME_DIR/.zshenv"
grep -q "$ZSH_USER" "$HOME_DIR/.zshenv"
grep -q "$USERPREFIX/share/man" "$HOME_DIR/.zshenv"

grep -q '>>> cgc shell integration >>>' "$HOME_DIR/.bashrc"
grep -q "$BASH_USER/cgc" "$HOME_DIR/.bashrc"
grep -q "$USERPREFIX/share/man" "$HOME_DIR/.bashrc"

grep -q '>>> cgc shell integration >>>' "$HOME_DIR/.config/fish/config.fish"
grep -q "$USERPREFIX/share/man" "$HOME_DIR/.config/fish/config.fish"

# Running install-shell again must replace the block, not duplicate it.
HOME="$HOME_DIR" make -s install-shell MANDIR="$USERPREFIX/share/man/man1"
test "$(grep -c '>>> cgc shell integration >>>' "$HOME_DIR/.zshenv")" -eq 1
test "$(grep -c '<<< cgc shell integration <<<' "$HOME_DIR/.zshenv")" -eq 1
test "$(grep -c '>>> cgc shell integration >>>' "$HOME_DIR/.bashrc")" -eq 1
test "$(grep -c '>>> cgc shell integration >>>' \
    "$HOME_DIR/.config/fish/config.fish")" -eq 1

# A symlinked dotfile must stay a symlink, keep its mode, and keep unrelated
# content when the managed block is added.
LINKHOME="$WORK/linkhome"
mkdir -p "$LINKHOME" "$WORK/dotfiles"
printf '# existing bashrc\n' >"$WORK/dotfiles/bashrc"
chmod 600 "$WORK/dotfiles/bashrc"
ln -s "$WORK/dotfiles/bashrc" "$LINKHOME/.bashrc"
HOME="$LINKHOME" make -s install-shell MANDIR="$USERPREFIX/share/man/man1"
test -L "$LINKHOME/.bashrc"
grep -q '# existing bashrc' "$WORK/dotfiles/bashrc"
grep -q '>>> cgc shell integration >>>' "$WORK/dotfiles/bashrc"
if stat -c '%a' "$WORK/dotfiles/bashrc" >/dev/null 2>&1; then
    test "$(stat -c '%a' "$WORK/dotfiles/bashrc")" = 600
fi

# Verify the generated zsh and bash blocks actually take effect.
if command -v zsh >/dev/null 2>&1; then
    out=$(HOME="$HOME_DIR" zsh -f -c \
        'source "$HOME/.zshenv"; print -r -- "$MANPATH"; print -l $fpath')
    grep -q "$USERPREFIX/share/man" <<<"$out"
    grep -q "$ZSH_USER" <<<"$out"
fi
if command -v bash >/dev/null 2>&1; then
    out=$(HOME="$HOME_DIR" bash --noprofile --norc -c \
        'source "$HOME/.bashrc"; printf "%s\n" "$MANPATH"; type _cgc >/dev/null && echo completion-loaded')
    grep -q "$USERPREFIX/share/man" <<<"$out"
    grep -q 'completion-loaded' <<<"$out"
fi

make -s uninstall PREFIX="$USERPREFIX"

make -s uninstall DESTDIR="$DEST" PREFIX="$PREFIX"

test ! -e "$DEST$PREFIX/bin/cgc"
test ! -e "$DEST$PREFIX/share/man/man1/cgc.1"
test ! -e "$DEST$PREFIX/share/bash-completion/completions/cgc"
test ! -e "$DEST$PREFIX/share/zsh/site-functions/_cgc"
test ! -e "$DEST$PREFIX/share/fish/vendor_completions.d/cgc.fish"

echo "install test passed"
