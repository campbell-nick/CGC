# bash completion for cgc
#
# Install as:
#   $(PREFIX)/share/bash-completion/completions/cgc
#
# or source it directly.

_cgc() {
    local cur prev cmd
    COMPREPLY=()
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}
    cmd=${COMP_WORDS[1]}

    local clone_opts="--host --token --token-file -b --branch --depth
        --single-branch --no-single-branch --bare --mirror --no-checkout
        --no-tags --origin -q --quiet -h --help -V --version"
    local update_opts="--token --token-file -b --branch --tags --no-tags
        --force --prune --remote -q --quiet -h --help -V --version"
    local subcommands="fetch pull update"

    case "$prev" in
        --token-file)
            if declare -F _filedir >/dev/null; then
                _filedir
            else
                COMPREPLY=($(compgen -f -- "$cur"))
            fi
            return
            ;;
        --host|--token|--branch|-b|--depth|--origin|--remote)
            return
            ;;
    esac

    if [[ "$cur" == -* ]]; then
        if [[ "$cmd" == fetch || "$cmd" == pull || "$cmd" == update ]]; then
            COMPREPLY=($(compgen -W "$update_opts" -- "$cur"))
        else
            COMPREPLY=($(compgen -W "$clone_opts" -- "$cur"))
        fi
        return
    fi

    if [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=($(compgen -W "$subcommands" -- "$cur"))
        COMPREPLY+=($(compgen -d -- "$cur"))
        return
    fi

    if [[ "$cmd" != fetch && "$cmd" != pull && "$cmd" != update ]]; then
        COMPREPLY=($(compgen -d -- "$cur"))
    fi
}

complete -F _cgc cgc
