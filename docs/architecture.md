# Architecture

## How it works

1. **Address resolution** (`url.c`) — turns a URL, shorthand, or local path into
   an HTTPS URL or an absolute filesystem path, plus a default directory name.
   `file://` URLs and `~` are expanded; a path ending in `/.git` derives the
   parent directory name.
2. **Repository skeleton** (`repo.c`) — creates `.git/objects`, `.git/refs`,
   `HEAD`, and a basic config.
3. **Capability discovery** (`protocol.c`, `http.c`) — `GET
   <repo>/info/refs?service=git-upload-pack` with `Git-Protocol: version=2`.
   TLS certificates are verified with the system trust store. If the server
   does not answer with a `version 2` banner, its protocol v0 ref
   advertisement is parsed instead, including NUL-separated capabilities,
   peeled `^{}` tag lines, and `symref=HEAD:...`.
4. **Ref listing** — for v2, `POST <repo>/git-upload-pack` with
   `command=ls-refs`, requesting peeled refs, symrefs and unborn `HEAD`. This
   yields all branches, tags and the default branch. The v0 advertisement is
   already the ref list.
5. **Fetch** — for v2, another `command=fetch` request asking for every
   advertised ref tip with `ofs-delta` and `done`. The response is a
   sideband-multiplexed packfile, streamed so network progress can be reported
   as it arrives. For v0, `want`/`deepen` lines are terminated with a flush,
   then `have` lines and `done` follow; the response mixes plain pkt-lines
   (`shallow`, `ACK`/`NAK`) with sideband data, with a raw-pack fallback when
   no sideband capability is negotiated.
6. **Unpack** (`pack.c`, `odb.c`) — verifies the pack checksum, inflates every
   entry, resolves `OFS_DELTA` and `REF_DELTA` chains with an oid hash map and
   binary-searched offsets, hashes each object and writes it as a loose,
   read-only object.
7. **Refs and config** (`repo.c`) — writes local branches, tags,
   `refs/remotes/<remote>/*`, `<remote>/HEAD`, `HEAD`, and the named remote
   (default `origin`). A bare clone writes refs and objects directly into the
   destination and omits the fetch refspec and branch section. A mirror clone
   is bare and copies every advertised ref verbatim, with a refs-wide fetch
   refspec and `mirror = true` in the config.
8. **Checkout** (`checkout.c`) — walks the `HEAD` tree, writes files, symlinks,
   executable bits and submodule mount points, then writes a DIRC version 2
   index so Git sees a clean working tree. `--bare` and `--no-checkout` skip
   this step.

## Local transport

For local paths and `file://` URLs, `url.c` resolves an absolute path and
`local.c` replaces the HTTP protocol layer:

1. **Locate the git directory** — accept a bare repository, a working tree with
   a `.git` directory, or a `.git` file pointing at a linked worktree's gitdir.
   A linked worktree's `commondir` file supplies the shared object and ref
   directories.
2. **List refs** — parse `packed-refs` (including peeled `^` lines), walk loose
   refs, and read `HEAD`. Normal clones limit the walk to `refs/heads` and
   `refs/tags`; mirrors and custom refspecs also read other namespaces such as
   `refs/notes`. The result uses the same `RemoteRefs` structure as protocol
   v2, so clone and update logic is shared.
3. **Copy objects** — copy loose objects that are missing from the destination,
   then unpack every source `.pack` file into destination loose objects. The
   source `shallow` file is copied when present.
4. **Annotated tags** — local ref listing has no peeled-commit advertisement, so
   after objects are copied a tag selected with `--branch` is peeled to its
   commit before checkout.

Local transfers deliberately copy all objects rather than negotiating `have`
lines. `--depth` is rejected for local sources because the pack reader has no
local history walker.

## Incremental fetch and pull

`cgc fetch` and `cgc pull` reuse the same protocol and pack layers with a
different flow (`update.c`):

1. **Locate the repository** — walk up from the given directory (default `.`)
   until a `.git` directory, gitfile, or bare repository is found. The remote is
   resolved from `--remote`, then `branch.<name>.remote` for the current
   branch, then `origin`, then the first configured remote.
2. **Refspecs** (`refspec.c`, `repo.c`) — parse the remote's `fetch` lines,
   supporting force (`+`), wildcard, and negative (`^`) refspecs. A mirror uses
   `+refs/*:refs/*`; a non-bare remote without a fetch line defaults to
   `+refs/heads/*:refs/remotes/<remote>/*`.
3. **Negotiate** — list refs as before, then send `command=fetch` with a `want`
   for each selected remote ref and `have` lines for every local ref, branch and
   tag. The server sends only objects reachable from the wants but not the
   haves. For a local remote, `local.c` lists refs and copies all objects
   directly instead of negotiating.
4. **Unpack** — the same pack reader writes the new objects. An empty pack
   means the clone was already up to date.
5. **Fetch bookkeeping** — map each advertised ref through the refspecs and
   write the destinations. Existing tags are preflighted and refused if the
   remote moved them, unless `--force` (a mirror always forces). `--no-tags`
   skips tags, and `--prune` (implied by a mirror) removes stale loose and
   packed refs under wildcard destination namespaces, preserving symrefs such as
   `refs/remotes/<remote>/HEAD`. Local branches and the worktree are untouched.
   `FETCH_HEAD` is written atomically. A bare repository with no fetch refspec
   writes only `FETCH_HEAD`, matching Git's default behavior.
6. **Fast-forward check** (`pull` only) — walk the new commit's parents to
   confirm the local branch tip is an ancestor. The check runs before refspec
   mappings are written so a refused pull cannot clobber a local branch through
   a custom refspec. Merges and rebases are not supported.
7. **Worktree update** (`pull` only) — `checkout_update` compares the old and
   new trees. Unchanged files are left alone; changed and deleted files must
   still match the old blob, and added paths must not collide with untracked
   files. Staged changes on untouched paths and index flags are preserved in
   the new index. `ORIG_HEAD` records the old `HEAD` before it moves. Ref, index,
   and metadata writes go through atomic rename.

## Source layout

```
src/
  main.c       CLI, shorthand parsing, clone and subcommand dispatch
  url.c        URL parsing, local-path detection, shorthand resolution
  http.c       HTTP/1.1 over TCP + OpenSSL, chunked bodies, redirects
  local.c      local repository discovery, refs, object and pack copying
  pktline.c    Git pkt-line framing
  protocol.c   protocol v2 and v0 fallback: discovery, refs, fetch, sideband
  refspec.c    fetch refspec parsing, mapping, negative and wildcard refspecs
  progress.c   Git-style status output and progress meters
  pack.c       packfile parsing, delta resolution
  odb.c        loose object read/write and hashing
  repo.c       .git skeleton, config, refs, HEAD, local object discovery
  checkout.c   tree walking, working tree, DIRC index, incremental updates
  update.c     fetch, fast-forward pull, and worktree orchestration
  util.c       buffers, SHA-1, zlib, filesystem helpers
```
