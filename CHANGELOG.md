# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/).

## [Unreleased]

### Added

- `cgc fetch [directory]` to download missing objects and update
  remote-tracking refs and tags without touching local branches or the working
  tree.
- `cgc pull [directory]` and its `cgc update` alias to fetch, fast-forward the
  current branch, and update the working tree and index.
- `--branch` support for `fetch`, `pull`, and `update`, including fetching a
  single branch. From a detached `HEAD`, `pull --branch` checks out the branch,
  fast-forwarding it when possible and refusing divergence.
- Fetch negotiation with `have` lines, so only objects missing from the local
  clone are transferred.
- Tag clobber protection: `fetch`, `pull`, and `update` refuse to move an
  existing local tag that points at a different object unless `--force` is
  given. All tags are preflighted before any ref is written.
- `--no-tags` and `--tags` to skip or restore tag fetching.
- Clone `--bare` to create a repository without a working tree or index, and
  `--no-checkout` to write refs and configuration without checking out files.
- Clone `--origin <name>` to name the remote; config sections, branch upstream
  settings, and `refs/remotes/<name>/*` all use the chosen name.
- Clone `--no-tags` to skip tags; a tag selected explicitly with `--branch` is
  still checked out.
- Fetch and pull use the first remote configured in `.git/config` rather than a
  hard-coded `origin`, and prune/update `refs/remotes/<name>/*` accordingly.
- Bare-repository detection for `fetch`; a bare fetch writes `FETCH_HEAD` and
  leaves refs unchanged, and `pull` is refused.
- Local-path and `file://` transports for clone, fetch, and pull. Local
  repositories are read directly without invoking `git`, including bare
  repositories, linked worktrees (`.git` files and `commondir`), packed refs,
  and source packfiles. Objects and packed objects are copied into the
  destination.
- Annotated tags selected with `--branch` from a local repository are peeled to
  their commit before checkout.
- Protocol v0 fallback for smart HTTP servers that do not advertise protocol
  v2, including v0 ref advertisement parsing (peeled tags, `symref=HEAD`,
  empty-repository pseudo-refs), sideband demultiplexing with a raw-pack
  fallback, shallow `--depth` fetches, and `have`-line negotiation.
- Configurable fetch refspecs for `fetch`, `pull`, and `update`, supporting
  force (`+`), wildcard (`*`), and negative (`^`) refspecs. The standard
  `+refs/heads/*:refs/remotes/<remote>/*` mapping is used when a non-bare
  remote has no configured fetch line.
- Multiple remotes: `--remote <name>` selects a remote, and the default is
  resolved from `branch.<name>.remote`, then `origin`, then the first configured
  remote. Pull uses `branch.<name>.merge` as its source when configured.
- Clone `--mirror` to create a bare repository that copies every advertised ref
  verbatim (`refs/*:refs/*`), records `mirror = true`, forces tag updates, and
  prunes refs that disappear upstream. Mirror ignores `--no-tags` and
  `--single-branch` and rejects `--depth`.
- `--prune` generalized to the destination namespaces of the configured fetch
  refspecs, including wildcard mappings; symrefs such as
  `refs/remotes/<remote>/HEAD` are preserved.
- `--prune` to delete remote-tracking refs for branches deleted upstream,
  including entries in `packed-refs`.
- `.git/FETCH_HEAD` for fetch and pull, and `.git/ORIG_HEAD` before a pull
  moves `HEAD`.
- End-to-end coverage for fetch, pull, the `update` alias, dirty-worktree
  refusal, and non-fast-forward refusal, plus tag safety, `--prune`, index
  flags, and staged-change handling.
- End-to-end coverage for `--bare`, `--no-checkout`, `--origin`, clone
  `--no-tags`, non-`origin` fetches, and bare fetch behavior.
- End-to-end coverage for local clone, bare and `file://` sources, local
  `--bare`/`--no-tags`/`--origin`, local pull and bare fetch, linked worktrees,
  and local `--depth` rejection.
- End-to-end coverage for protocol v0 clone, peeled tags, shallow clones, empty
  repositories, fetch, and pull.
- End-to-end coverage for `--mirror` clone/fetch/prune, local mirror sources,
  custom fetch refspecs, multiple remotes with `--remote`, default
  branch-remote selection, unknown remote rejection, and `--prune` preserving
  `origin/HEAD`.
- Unit coverage for refspec parsing, force and wildcard mapping, destination
  matching, negative refspecs, and invalid refspec rejection.

### Changed

- Pull is fast-forward-only and preserves unchanged files. It refuses to
  overwrite local modifications or untracked paths, and refuses to fetch into a
  shallow clone.
- Pull preserves unrelated staged changes instead of refusing any dirty index.
  It refuses only when a staged change overlaps a path the update changes and
  differs from the new tree. `skip-worktree`, `assume-unchanged`, and extended
  index flags survive the update.
- Refs are written through a sibling `.lock` file and renamed into place; the
  index, `FETCH_HEAD`, and `ORIG_HEAD` are written atomically. `index_write`
  failures are reported instead of ignored.
- An empty fetch pack is treated as "already up to date" instead of an error,
  and no receive/delta summary is printed for a no-op fetch.
- `fetch`, `pull`, and `update` now honor each remote's configured fetch
  refspecs and select the remote from `--remote`, `branch.<name>.remote`,
  `origin`, or the first configured remote, instead of always using the first
  remote. A bare repository with no fetch refspec still records only
  `FETCH_HEAD`, but its summary now lists the fetched refs.

## [0.2.0] - 2026-11-09

### Added

- `--branch`/`-b` to check out a branch or tag instead of the remote default
  (tags use a detached HEAD).
- `--depth <n>` shallow clones that write `.git/shallow`, plus
  `--single-branch` / `--no-single-branch`.
- HTTPS bearer-token authentication via `--token`, `--token-file`,
  `CGC_TOKEN`, or `GIT_TOKEN`. Tokens are stripped from the URL and never
  written to `.git/config`.
- Git-style clone status: `Cloning into`, `remote:` lines, `Receiving objects`,
  `Resolving deltas`, and `Updating files`.
- `sym:` shorthand for `git.symlinx.net`.
- Man page (`cgc.1`).
- Configurable `make install` with `PREFIX`, `DESTDIR`, and `make uninstall`.
- Focused documentation under `docs/`.
- `VERSION` as the single source of truth for the reported version.
- `make dist` release tarballs and a tag-driven release workflow.
- Offline unit tests and pack round-trip tests (`make check`).
- End-to-end coverage for chunked responses, `--quiet`, empty repositories,
  multiple branches and tags, and non-ASCII/space-containing filenames.
- CI jobs for GCC, Clang, ASan/UBSan, macOS, and release-tarball builds.
- A curated `clang-tidy` static-analysis target (`make analyze`) and CI job.
- Validation of server-supplied ref names, `HEAD` targets, and values written
  to `.git/config`.
- Checkout protection against duplicate paths, file/directory collisions, and
  symlinked parent directories.

### Changed

- Build flags now keep the C standard, feature-test macros, and warnings even
  when `CFLAGS` is overridden.
- Cloning continues to reject URLs containing control characters.

### Fixed

- `--depth` is parsed strictly; non-numeric, zero, negative, and overflowing
  values are rejected instead of silently becoming zero.
- A pack that declares an implausible object count or an object larger than
  2 GiB is rejected before allocating memory.
- Avoided a zero-length `qsort` call when validating an empty checkout index.

## [0.1.0]

- Initial release: smart HTTP protocol v2 clone, pack unpacking with
  `ofs-delta`/`ref-delta` resolution, checkout, Git-compatible index, and URL
  shorthands.
