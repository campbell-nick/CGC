# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[semantic versioning](https://semver.org/).

## [Unreleased]

### Added

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
- Validation of server-supplied ref names, `HEAD` targets, and values written
  to `.git/config`.
- Checkout protection against duplicate paths, file/directory collisions, and
  symlinked parent directories.

### Changed

- Build flags now keep the C standard, feature-test macros, and warnings even
  when `CFLAGS` is overridden.
- Cloning continues to reject URLs containing control characters.

## [0.1.0]

- Initial release: smart HTTP protocol v2 clone, pack unpacking with
  `ofs-delta`/`ref-delta` resolution, checkout, Git-compatible index, and URL
  shorthands.
