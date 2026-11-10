# fish completion for cgc
#
# Install as:
#   $(PREFIX)/share/fish/vendor_completions.d/cgc.fish

complete -c cgc -f

complete -c cgc -n '__fish_use_subcommand' -a fetch -d 'Fetch remote updates'
complete -c cgc -n '__fish_use_subcommand' -a pull -d 'Fetch and fast-forward'
complete -c cgc -n '__fish_use_subcommand' -a update -d 'Alias for pull'

# Clone options.
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l host -r -d 'Default host'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l token -r -d 'Bearer token'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l token-file -r -F -d 'Token file'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -s b -l branch -r -d 'Branch to check out'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l depth -r -d 'Shallow clone depth'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l single-branch -d 'Clone only one branch'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l no-single-branch -d 'Fetch all branches'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l bare -d 'Create a bare repository'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l mirror -d 'Mirror all refs'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l no-checkout -d 'Skip the working-tree checkout'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l no-tags -d 'Do not fetch tags'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -l origin -r -d 'Remote name'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -s q -l quiet -d 'Suppress progress messages'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -s h -l help -d 'Show help'
complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -s V -l version -d 'Show version'

# fetch / pull / update options.
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l token -r -d 'Bearer token'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l token-file -r -F -d 'Token file'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -s b -l branch -r -d 'Branch'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l tags -d 'Fetch and update tags'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l no-tags -d 'Do not fetch tags'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l force -d 'Overwrite conflicting local tags'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l prune -d 'Delete stale refs'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -l remote -r -d 'Remote name'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -s q -l quiet -d 'Suppress status output'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -s h -l help -d 'Show help'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -s V -l version -d 'Show version'

complete -c cgc -n 'not __fish_seen_subcommand_from fetch pull update' -a '(__fish_complete_directories)'
complete -c cgc -n '__fish_seen_subcommand_from fetch pull update' -a '(__fish_complete_directories)'
