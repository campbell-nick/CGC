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
- End-to-end coverage for fetch, pull, the `update` alias, dirty-worktree
  refusal, and non-fast-forward refusal.

### Changed

- Pull is fast-forward-only and preserves unchanged files. It refuses to
  overwrite local modifications or untracked paths, and refuses to fetch into a
  shallow clone.
- An empty fetch pack is treated as "already up to date" instead of an error,
  and no receive/delta summary is printed for a no-op fetch.

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
