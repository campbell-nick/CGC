# cgc — `git clone` from scratch, in C

<p align="center">
  <img src="logo.svg" alt="cgc logo" width="128" height="128">
</p>

[![CI](https://github.com/campbell-nick/cgc/actions/workflows/ci.yml/badge.svg)](https://github.com/campbell-nick/cgc/actions/workflows/ci.yml)

Version: 0.2.0

`cgc` is a self-contained implementation of `git clone` over the Git smart
HTTP protocol (version 2). It does **not** shell out to `git`; it speaks the
protocol itself, validates and unpacks the packfile, resolves deltas, writes
loose objects and refs, checks out the working tree, and writes a Git index.
It can also `fetch`, `pull`, and `update` an existing clone, negotiating with
`have` lines and fast-forwarding the current branch.

Clones can be bare (`--bare`), skip the working-tree checkout (`--no-checkout`),
use a custom remote name (`--origin <name>`), or skip tags (`--no-tags`). Fetch
and pull follow the first remote configured in `.git/config`, so non-`origin`
remote names update `refs/remotes/<name>/*`.

Fetch and pull update remote-tracking refs and tags safely: a local tag that
differs from the remote is refused unless `--force` is given, `--no-tags` skips
tags, and `--prune` removes remote-tracking refs for deleted branches (including
packed refs). Pulls preserve unrelated staged changes and index flags such as
`skip-worktree` and `assume-unchanged`. `FETCH_HEAD` and `ORIG_HEAD` are
maintained, and ref, index, and metadata writes use atomic renames.

```sh
make
./cgc gh:octocat/Hello-World
cd Hello-World
../cgc fetch      # update remote-tracking refs
../cgc pull       # fetch and fast-forward
```

Install with `make install` (supports `PREFIX` and `DESTDIR`), or build a
release tarball with `make dist`. The version is tracked in `VERSION`.

Verified against GitHub and against `git http-backend` with repositories that
contain branches, annotated tags, nested trees, symlinks, executables and
deltas (both `ofs-delta` and `ref-delta`). Clones pass `git fsck --full` and
`git status` is clean; pulls are verified the same way. `make check` runs
offline unit and pack tests; `make test` adds the end-to-end smoke test.

## Documentation

| Guide | Contents |
| --- | --- |
| [Build and test](docs/build.md) | Dependencies, building, installing, tests, releases, CI |
| [Usage](docs/usage.md) | Repository forms, URL shorthands, options, fetch/pull, output, safety, verifying a clone |
| [Architecture](docs/architecture.md) | How cloning, fetch, and pull work, and the source layout |
| [Supported and limitations](docs/limitations.md) | What works today, v1 limitations, roadmap |

## License

MIT. See [LICENSE](LICENSE).
