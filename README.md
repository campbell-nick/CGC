# cgc — `git clone` from scratch, in C

[![CI](https://github.com/campbell-nick/cgc/actions/workflows/ci.yml/badge.svg)](https://github.com/campbell-nick/cgc/actions/workflows/ci.yml)

`cgc` is a self-contained implementation of `git clone` over the Git smart
HTTP protocol (version 2). It does **not** shell out to `git`; it speaks the
protocol itself, validates and unpacks the packfile, resolves deltas, writes
loose objects and refs, checks out the working tree, and writes a Git index.

Verified against GitHub and against `git http-backend` with repositories that
contain branches, annotated tags, nested trees, symlinks, executables and
deltas (both `ofs-delta` and `ref-delta`). Clones pass `git fsck --full` and
`git status` is clean.

## Build

Dependencies:

- a C11 compiler
- zlib
- OpenSSL (`libssl`, `libcrypto`)

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

## Usage

```
cgc [options] <repository> [directory]
```

If `directory` is omitted, it is derived from the repository name (with a
trailing `.git` removed).

### Repository forms

| Argument | Resolves to |
| --- | --- |
| `https://github.com/owner/repo.git` | the URL as given |
| `gh:owner/repo` | `https://github.com/owner/repo.git` |
| `gl:owner/repo` | `https://gitlab.com/owner/repo.git` |
| `bb:owner/repo` | `https://bitbucket.org/owner/repo.git` |
| `owner/repo` | `https://<default-host>/owner/repo.git` |
| `host.tld/owner/repo` | `https://host.tld/owner/repo.git` |

The default host is `github.com`, overridable with `--host` or the
`CGC_DEFAULT_HOST` environment variable.

### Examples

```sh
cgc gh:octocat/Hello-World
cgc octocat/Hello-World my-copy
cgc https://github.com/torvalds/linux.git
cgc --host git.example.com team/repo
CGC_DEFAULT_HOST=git.example.com cgc team/repo
```

### Options

```
--host <host>   default host for owner/repo shorthand
-q, --quiet     suppress progress messages
-h, --help      show help
-V, --version   show version
```

### Output

Status is written to stderr in the same style as `git clone`: `Cloning into
'...'...`, any `remote:` messages from the server, a `Receiving objects`
download meter, `Resolving deltas`, and `Updating files` during checkout. Live
meters are drawn on a terminal; a final summary line is printed when output is
redirected. `--quiet` suppresses all of it.

## How it works

1. **Address resolution** (`url.c`) — turns a URL or shorthand into an HTTPS
   URL and a default directory name.
2. **Repository skeleton** (`repo.c`) — creates `.git/objects`, `.git/refs`,
   `HEAD`, and a basic config.
3. **Capability discovery** (`protocol.c`, `http.c`) — `GET
   <repo>/info/refs?service=git-upload-pack` with `Git-Protocol: version=2`.
   TLS certificates are verified with the system trust store.
4. **Ref listing** — `POST <repo>/git-upload-pack` with `command=ls-refs`,
   requesting peeled refs, symrefs and unborn `HEAD`. This yields all branches,
   tags and the default branch.
5. **Fetch** — another `command=fetch` request asking for every advertised ref
   tip with `ofs-delta` and `done`. The response is a sideband-multiplexed
   packfile.
6. **Unpack** (`pack.c`, `odb.c`) — verifies the pack checksum, inflates every
   entry, resolves `OFS_DELTA` and `REF_DELTA` chains with an oid hash map and
   binary-searched offsets, hashes each object and writes it as a loose,
   read-only object.
7. **Refs and config** (`repo.c`) — writes local branches, tags,
   `refs/remotes/origin/*`, `origin/HEAD`, `HEAD`, and the `origin` remote.
8. **Checkout** (`checkout.c`) — walks the `HEAD` tree, writes files, symlinks,
   executable bits and submodule mount points, then writes a DIRC version 2
   index so Git sees a clean working tree.

### Source layout

```
src/
  main.c       CLI, shorthand parsing, clone orchestration
  url.c        URL parsing and shorthand resolution
  http.c       HTTP/1.1 over TCP + OpenSSL, chunked bodies, redirects
  pktline.c    Git pkt-line framing
  protocol.c   protocol v2: discovery, ls-refs, fetch, sideband
  progress.c   Git-style status output and progress meters
  pack.c       packfile parsing, delta resolution
  odb.c        loose object read/write and hashing
  repo.c       .git skeleton, config, refs, HEAD
  checkout.c   tree walking, working tree, DIRC index
  util.c       buffers, SHA-1, zlib, filesystem helpers
```

## Supported

- Smart HTTP protocol v2 over HTTPS (plain HTTP is accepted for local testing)
- Redirects, chunked transfer encoding, and verified TLS
- Branches, annotated tags, remote-tracking refs
- `ofs-delta` and `ref-delta` resolution, including delta chains
- Nested trees, symlinks, executable files
- Empty repositories (creates `.git` with the advertised default branch)
- A Git-compatible index: `git status` and `git log` work immediately

## Limitations (v1)

- No SSH, `git://` or local-path transports.
- No authentication, so private repositories are not supported yet.
- Protocol v2 only; servers that only speak protocol v0/v1 are rejected.
- SHA-1 repositories only (SHA-256 object format is detected and rejected).
- The whole pack and all inflated objects are held in memory. Small and medium
  repositories are fine; huge repositories need a streaming implementation.
- No shallow/partial/filtered clone, no submodule recursion, no Git LFS,
  no hooks, no reflogs.
- No retry or resume on network failures.

## Verifying a clone

```sh
cd <cloned-repo>
git fsck --full
git status
git log --oneline -5
```

## Roadmap

- Stream packfiles and objects to bound memory use
- Keep the pack and write a `.idx` instead of exploding into loose objects
- Protocol v0 fallback and SSH transport
- Credential/token authentication
- Shallow and partial clone
- Parallel batch cloning from a list or config
