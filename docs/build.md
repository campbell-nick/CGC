# Build and test

## Dependencies

- a C11 compiler (GCC or Clang)
- zlib
- OpenSSL (`libssl`, `libcrypto`)
- `pkg-config` (optional but recommended; without it the build falls back to
  `-lz -lssl -lcrypto`)

## Build

```sh
make
./cgc --version
./cgc --help
```

The version comes from the [`VERSION`](../VERSION) file. Override it for a
one-off build with `make VERSION=1.2.3`.

Standard, warning, and feature-test flags are always applied, even when you
override `CFLAGS`:

```sh
make CFLAGS="-O2 -g -Werror"
```

## Install

Install with `sudo make install` (defaults to `/usr/local/bin/cgc` and
`/usr/local/share/man/man1/cgc.1`). Set `PREFIX` to install elsewhere, or
`DESTDIR` to stage a package install:

```sh
make install PREFIX="$HOME/.local"
make install DESTDIR=/tmp/pkgroot
make uninstall
```

`make install` also installs bash, zsh, and fish completions under
`$(PREFIX)/share/bash-completion/completions`,
`$(PREFIX)/share/zsh/site-functions`, and
`$(PREFIX)/share/fish/vendor_completions.d`. Override `BASHCOMPDIR`,
`ZSHCOMPDIR`, or `FISHCOMPDIR` for a different layout.

For a system prefix such as `/usr` or `/usr/local`, the manual page and
completions are already in the standard search paths. For a per-user prefix
such as `$HOME/.local`, `make install` prints the directories to add to
`MANPATH` and the shell completion paths. After installing, `man cgc` shows
the manual page. The optional `make install-shell` target sets this up
automatically and idempotently:

```sh
make install PREFIX="$HOME/.local"
make install-shell PREFIX="$HOME/.local"
```

It writes a managed block to `~/.zshenv`, `~/.bashrc`, and
`~/.config/fish/config.fish`, adds the manual directory to `MANPATH`, adds the
zsh completion directory to `fpath`, and installs completions under the user's
XDG directories. It does not edit the files outside its marked block, and
running it again replaces the block instead of duplicating it. Start a new
shell afterwards.

## Tests

```sh
make check         # unit, pack round-trip, and completion syntax (no network)
make install-check # staged install, man discovery, and shell integration
make network       # timeouts, retries, proxy use and bypass (local servers)
make features      # comprehensive end-to-end feature matrix
make test          # all of the above, plus smoke and feature tests
```

`make check` runs:

- `tests/unit` — URL parsing and every shorthand, pkt-line framing, SHA-1
  vectors, zlib round-trips, path helpers, and ref-name validation.
- `tests/pack.sh` — builds packfiles with Git (including `ofs-delta`,
  `ref-delta`, and thin packs), unpacks them with `cgc`'s pack reader, and
  compares the object database with the source repository. Self-contained
  packs must be retained as a pack plus index; thin packs must fall back to
  loose objects.
- A syntax check of the bash completion.

`tests/network.sh` starts local HTTP servers and proxies. It verifies that a
stalled server times out, that transient 503 responses are retried, that the
error report includes the status, hint, and server message, and that HTTP
proxies, proxy credentials, `ALL_PROXY`, and `NO_PROXY` work.

`tests/install.sh` stages an install with `DESTDIR`, checks that `man` can find
the staged page through `MANPATH`, verifies the non-system-prefix hints, and
runs `make install-shell` against a temporary `HOME`. It confirms the managed
blocks are applied and remain idempotent, and that the zsh and bash blocks
take effect.

`make test` additionally runs `tests/smoke.sh`, which generates a repository
with history, deltas, branches, annotated and lightweight tags, a symlink, an
executable, and non-ASCII and space-containing filenames. It serves the
repository with `git http-backend` using both `Content-Length` and chunked
transfer encoding, clones it with `cgc`, and verifies the result with `git
fsck`, `git status`, and ref comparisons. It also checks `--quiet`, cloning an
empty repository, fetch/pull/update, and the refusal paths for a dirty worktree
and a non-fast-forward update.

`tests/features.sh` is a wider matrix that checks CLI validation, every clone
mode, fetch/pull/update semantics, refspecs and multiple remotes, mirrors
(including notes), local and `file://` transport, shallow clones, and protocol
v0. It verifies results with `git fsck`, `git status`, and ref comparisons.

Sanitizer builds can be run locally:

```sh
make clean
make CC=clang \
     CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
     LDFLAGS="-fsanitize=address,undefined" check test
```

Static analysis uses `clang-tidy` with a curated set of correctness and safety
checks (the `insecureAPI` style checks are excluded):

```sh
make analyze
```

## Release tarball

```sh
make dist
```

This creates `dist/cgc-$(VERSION).tar.gz` containing the sources, tests,
documentation, man page, licence, changelog, and `VERSION`. Build and test it
from a clean extraction:

```sh
tar -xzf dist/cgc-*.tar.gz -C /tmp
cd /tmp/cgc-*
make check test
```

## Releases

1. Update `VERSION`, the date and version in `cgc.1`, and `CHANGELOG.md`.
2. Commit the release and tag it `v$(VERSION)`.
3. Push the commit and tag. The release workflow builds, tests, creates the
   release tarball, and publishes a GitHub release with generated notes.

## CI

CI runs on every push and pull request:

- GCC and Clang builds with `-Werror`, unit tests, and the smoke test
- ASan + UBSan build and full test run
- `clang-tidy` static analysis (`make analyze`)
- macOS build and test with Homebrew OpenSSL
- `make dist`, followed by a build and test from the extracted tarball
