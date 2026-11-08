# Architecture

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
   packfile, streamed so network progress can be reported as it arrives.
6. **Unpack** (`pack.c`, `odb.c`) — verifies the pack checksum, inflates every
   entry, resolves `OFS_DELTA` and `REF_DELTA` chains with an oid hash map and
   binary-searched offsets, hashes each object and writes it as a loose,
   read-only object.
7. **Refs and config** (`repo.c`) — writes local branches, tags,
   `refs/remotes/origin/*`, `origin/HEAD`, `HEAD`, and the `origin` remote.
8. **Checkout** (`checkout.c`) — walks the `HEAD` tree, writes files, symlinks,
   executable bits and submodule mount points, then writes a DIRC version 2
   index so Git sees a clean working tree.

## Source layout

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
