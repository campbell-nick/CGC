#!/bin/sh
# Install optional per-user shell integration for cgc.
#
# Adds an idempotent managed block to the zsh, bash, and fish startup files
# that puts the cgc manual page on MANPATH, adds the zsh completion directory
# to fpath, and sources the bash completion. The bash, zsh, and fish
# completions are installed under the user's XDG directories. Running this
# repeatedly replaces the existing block instead of duplicating it.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MANDIR=${MANDIR:-${PREFIX:-$HOME/.local}/share/man/man1}
MANROOT=$(dirname "$MANDIR")

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
BASH_COMPLETIONS=$XDG_DATA_HOME/bash-completion/completions
ZSH_COMPLETIONS=$XDG_DATA_HOME/zsh/site-functions
FISH_COMPLETIONS=$XDG_CONFIG_HOME/fish/completions

ZSH_ENV=${ZDOTDIR:-$HOME}/.zshenv
BASH_RC=$HOME/.bashrc
FISH_CONFIG=$XDG_CONFIG_HOME/fish/config.fish

START='# >>> cgc shell integration >>>'
END='# <<< cgc shell integration <<<'

die() {
    echo "install-shell: $*" >&2
    exit 1
}

install_completion() {
    src=$1
    dst=$2
    [ -f "$src" ] || die "missing completion file: $src"
    mkdir -p "$(dirname "$dst")" || die "cannot create $(dirname "$dst")"
    install -m 644 "$src" "$dst" || die "cannot install $dst"
}

# Remove an existing managed block. Refuse to touch a file with an incomplete
# block rather than deleting unrelated content after a missing END marker.
strip_block() {
    file=$1
    [ -f "$file" ] || return 0
    grep -qF "$START" "$file" || return 0
    starts=$(grep -cF "$START" "$file" || true)
    ends=$(grep -cF "$END" "$file" || true)
    if [ "$starts" -ne "$ends" ]; then
        die "$file has an incomplete cgc shell integration block"
    fi
    tmp=$file.cgc-tmp.$$
    if ! awk -v start="$START" -v end="$END" '
        $0 == start { skip = 1; next }
        skip && $0 == end { skip = 0; next }
        !skip { print }
    ' "$file" >"$tmp"; then
        rm -f "$tmp"
        die "cannot update $file"
    fi
    # Write through the original path so its permissions are preserved and a
    # symlinked dotfile stays a symlink.
    if ! cat "$tmp" >"$file"; then
        rm -f "$tmp"
        die "cannot update $file"
    fi
    rm -f "$tmp"
}

append_block() {
    file=$1
    block=$2
    strip_block "$file"
    mkdir -p "$(dirname "$file")" || die "cannot create $(dirname "$file")"
    if [ -s "$file" ]; then
        printf '\n%s\n' "$block" >>"$file" || die "cannot update $file"
    else
        printf '%s\n' "$block" >>"$file" || die "cannot update $file"
    fi
}

zsh_block() {
    cat <<EOF
$START
# Added by cgc install-shell; remove this block to undo.
if [ -n "\${MANPATH:-}" ]; then
    case ":\$MANPATH:" in
        *":$MANROOT:"*) ;;
        *) MANPATH="$MANROOT:\$MANPATH" ;;
    esac
else
    MANPATH="$MANROOT:"
fi
export MANPATH
fpath=("$ZSH_COMPLETIONS" \$fpath)
$END
EOF
}

bash_block() {
    cat <<EOF
$START
# Added by cgc install-shell; remove this block to undo.
if [ -n "\${MANPATH:-}" ]; then
    case ":\$MANPATH:" in
        *":$MANROOT:"*) ;;
        *) MANPATH="$MANROOT:\$MANPATH" ;;
    esac
else
    MANPATH="$MANROOT:"
fi
export MANPATH
if [ -r "$BASH_COMPLETIONS/cgc" ]; then
    . "$BASH_COMPLETIONS/cgc"
fi
$END
EOF
}

fish_block() {
    cat <<EOF
$START
# Added by cgc install-shell; remove this block to undo.
if set -q MANPATH
    if not contains -- "$MANROOT" \$MANPATH
        set -gx MANPATH "$MANROOT" \$MANPATH
    end
else
    set -gx MANPATH "$MANROOT" ""
end
$END
EOF
}

install_completion "$ROOT/completions/cgc.bash" "$BASH_COMPLETIONS/cgc"
install_completion "$ROOT/completions/_cgc" "$ZSH_COMPLETIONS/_cgc"
install_completion "$ROOT/completions/cgc.fish" "$FISH_COMPLETIONS/cgc.fish"

append_block "$ZSH_ENV" "$(zsh_block)"
append_block "$BASH_RC" "$(bash_block)"
append_block "$FISH_CONFIG" "$(fish_block)"

echo "Installed cgc shell integration:"
echo "  zsh:  $ZSH_ENV (MANPATH, fpath)"
echo "  bash: $BASH_RC (MANPATH, completion)"
echo "  fish: $FISH_CONFIG (MANPATH)"
echo "Completions:"
echo "  bash: $BASH_COMPLETIONS/cgc"
echo "  zsh:  $ZSH_COMPLETIONS/_cgc"
echo "  fish: $FISH_COMPLETIONS/cgc.fish"
echo "Start a new shell or source the startup file to apply the changes."
