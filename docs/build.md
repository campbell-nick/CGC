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

Install with `sudo make install` (installs to `/usr/local/bin/cgc`).

## Tests

```sh
make test          # runs tests/smoke.sh
```

The smoke test generates a repository with history, deltas, a tag, a symlink
and an executable, serves it with `git http-backend`, clones it with `cgc`, and
then verifies the result with `git fsck`, `git status`, and ref comparisons.

CI runs the same test on every push and pull request with both GCC and Clang,
building with `-Werror`.
