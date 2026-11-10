# Supported and limitations

## Supported

- Smart HTTP protocol v2 over HTTPS, with a protocol v0 fallback for servers
  that do not advertise v2 (plain HTTP is accepted for local testing)
- Local-path and `file://` transports, including bare repositories, linked
  worktrees (`.git` files and `commondir`), packed refs, and source packfiles
- Redirects, chunked transfer encoding, and verified TLS
- Connect, read, and write timeouts (`CGC_HTTP_TIMEOUT`) and retries with
  backoff for transient `GET`/`HEAD` failures (`CGC_HTTP_RETRIES`)
- HTTP and HTTPS proxies (`http_proxy`, `https_proxy`, `all_proxy`), proxy
  authentication, and `NO_PROXY` bypass
- Branches, annotated tags, remote-tracking refs
- `--branch`/`-b` to check out a branch or tag (detached HEAD for tags)
- `--depth <n>` shallow clones (writes `.git/shallow`), with `--single-branch`
- `--bare` and `--no-checkout` clones
- `--origin <name>` to name the remote, including non-`origin` remote-tracking
  refs; multiple remotes are supported, with `--remote <name>` selection and
  `branch.<name>.remote`/`branch.<name>.merge` resolution
- Configurable fetch refspecs, including force (`+`), wildcard, and negative
  (`^`) refspecs; `--mirror` clones and fetches with `refs/*:refs/*`
- `fetch`, `pull`, and `update` for existing clones, using `have` lines so only
  missing objects are transferred
- Fast-forward-only pulls that preserve unchanged files and refuse to overwrite
  local modifications or untracked paths
- Unrelated staged changes and index flags (`skip-worktree`,
  `assume-unchanged`) survive a pull
- Tag clobber protection (`--force` to override) and `--no-tags`
- `--prune` for stale remote-tracking refs, including packed refs
- `.git/FETCH_HEAD` and `.git/ORIG_HEAD` updates
- Atomic ref, index, and metadata writes
- `ofs-delta` and `ref-delta` resolution, including delta chains
- Self-contained HTTP packs are retained as `objects/pack/pack-<sha>.pack`
  with a v2 `.idx` and read directly; thin packs fall back to loose objects
- Nested trees, symlinks, executable files
- Empty repositories (creates `.git` with the advertised default branch)
- A Git-compatible index: `git status` and `git log` work immediately
- Validation of server-supplied ref names, `HEAD` targets, and config values
- Checkout that rejects duplicate paths, file/directory collisions, and
  symlinked parent directories

## Limitations (v1)

- No SSH or `git://` transport.
- Local transfers copy every object in the source repository instead of using
  `have` negotiation, and `--depth` is rejected for local sources. Source
  packfiles are re-packed into new destination packs rather than copied
  verbatim, so local copies do not preserve the source pack layout.
- HTTPS bearer-token authentication is supported (`--token`, `--token-file`,
  `CGC_TOKEN`, `GIT_TOKEN`); SSH keys and other credential helpers are not.
- Protocol v0 fallback covers the common smart-HTTP upload-pack flow; protocol
  v1 (a v0/v2 hybrid) is not implemented.
- SHA-1 repositories only (SHA-256 object format is detected and rejected).
- The whole pack and all inflated objects are held in memory, and a single
  object larger than 2 GiB is rejected. Small and medium repositories are fine;
  huge repositories need a streaming implementation.
- Pull is fast-forward-only: no merge commits, rebase, or conflict resolution.
  Diverged branches are refused.
- Pull refuses to switch away from an existing branch, and refuses to fetch into
  a shallow clone. From a detached `HEAD`, `pull --branch` switches to the
  branch, fast-forwarding it when possible and refusing divergence.
- `--prune` is skipped when `--branch` limits the fetch, because the omitted
  branches would otherwise look deleted.
- In a bare repository with no fetch refspec, `fetch` records `FETCH_HEAD` but
  does not update refs; `pull` is refused. A bare repository configured with a
  fetch refspec updates refs normally.
- Pull reads `.git/index` (versions 2 and 3) to detect staged changes and
  preserve stat data and flags; other index versions are rejected.
- Submodules are represented as empty directories; they are not cloned or
  updated.
- Tags are fetched in full rather than Git's reachability-based auto-follow.
  A conflicting local tag is refused unless `--force` is given.
- No partial/filtered clone, no submodule recursion (`--recursive`), no Git LFS,
  no hooks, no reflogs.
- Retries cover idempotent `GET`/`HEAD` requests only; a fetch `POST` is not
  retried, and there is no resume of an interrupted transfer.

## Roadmap

- Stream packfiles and objects to bound memory use
- Merge/rebase support and shallow fetch (`--deepen`)
- SSH transport
- Partial clone
- Submodule clone and update
- Parallel batch cloning from a list or config
