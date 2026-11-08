# Build and test

## Dependencies

- a C11 compiler
- zlib
- OpenSSL (`libssl`, `libcrypto`)

## Build

```sh
make
./cgc --help
```

Install with `sudo make install` (defaults to `/usr/local/bin/cgc` and
`/usr/local/share/man/man1/cgc.1`). Set `PREFIX` to install elsewhere, or
`DESTDIR` to stage a package install:

```sh
make install PREFIX="$HOME/.local"
make install DESTDIR=/tmp/pkgroot
make uninstall
```

After installing, `man cgc` shows the manual page.

## Tests

```sh
make test          # runs tests/smoke.sh
```

The smoke test generates a repository with history, deltas, a tag, a symlink
and an executable, serves it with `git http-backend`, clones it with `cgc`, and
then verifies the result with `git fsck`, `git status`, and ref comparisons.

CI runs the same test on every push and pull request with both GCC and Clang,
building with `-Werror`.
