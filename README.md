# cgc — `git clone` from scratch, in C

[![CI](https://github.com/campbell-nick/cgc/actions/workflows/ci.yml/badge.svg)](https://github.com/campbell-nick/cgc/actions/workflows/ci.yml)

`cgc` is a self-contained implementation of `git clone` over the Git smart
HTTP protocol (version 2). It does **not** shell out to `git`; it speaks the
protocol itself, validates and unpacks the packfile, resolves deltas, writes
loose objects and refs, checks out the working tree, and writes a Git index.

```sh
make
./cgc gh:octocat/Hello-World
```

Verified against GitHub and against `git http-backend` with repositories that
contain branches, annotated tags, nested trees, symlinks, executables and
deltas (both `ofs-delta` and `ref-delta`). Clones pass `git fsck --full` and
`git status` is clean.

## Documentation

| Guide | Contents |
| --- | --- |
| [Build and test](docs/build.md) | Dependencies, building, installing, test suite, CI |
| [Usage](docs/usage.md) | Repository forms, URL shorthands, options, output, verifying a clone |
| [Architecture](docs/architecture.md) | How cloning works and the source layout |
| [Supported and limitations](docs/limitations.md) | What works today, v1 limitations, roadmap |
