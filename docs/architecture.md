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

## Incremental fetch and pull

`cgc fetch` and `cgc pull` reuse the same protocol and pack layers with a
different flow (`update.c`):

1. **Locate the repository** — walk up from the given directory (default `.`)
   until a `.git` directory or gitfile is found, and read the `origin` URL from
   `.git/config`.
2. **Negotiate** — list refs as before, then send `command=fetch` with a `want`
   for each remote ref and `have` lines for every local ref, branch and tag.
   The server sends only objects reachable from the wants but not the haves.
3. **Unpack** — the same pack reader writes the new objects. An empty pack
   means the clone was already up to date.
4. **Fetch bookkeeping** — rewrite `refs/remotes/origin/*` and tags. Existing
   tags are preflighted and refused if the remote moved them, unless `--force`.
   `--no-tags` skips tags, and `--prune` removes stale loose and packed
   remote-tracking refs. Local branches and the worktree are untouched.
   `FETCH_HEAD` is written atomically.
5. **Fast-forward check** (`pull` only) — walk the new commit's parents to
   confirm the local branch tip is an ancestor. Merges and rebases are not
   supported.
6. **Worktree update** (`pull` only) — `checkout_update` compares the old and
   new trees. Unchanged files are left alone; changed and deleted files must
   still match the old blob, and added paths must not collide with untracked
   files. Staged changes on untouched paths and index flags are preserved in
   the new index. `ORIG_HEAD` records the old `HEAD` before it moves. Ref, index,
   and metadata writes go through atomic rename.

## Source layout

```
src/
  main.c       CLI, shorthand parsing, clone and subcommand dispatch
  url.c        URL parsing and shorthand resolution
  http.c       HTTP/1.1 over TCP + OpenSSL, chunked bodies, redirects
  pktline.c    Git pkt-line framing
  protocol.c   protocol v2: discovery, ls-refs, fetch, sideband
  progress.c   Git-style status output and progress meters
  pack.c       packfile parsing, delta resolution
  odb.c        loose object read/write and hashing
  repo.c       .git skeleton, config, refs, HEAD, local object discovery
  checkout.c   tree walking, working tree, DIRC index, incremental updates
  update.c     fetch, fast-forward pull, and worktree orchestration
  util.c       buffers, SHA-1, zlib, filesystem helpers
```
