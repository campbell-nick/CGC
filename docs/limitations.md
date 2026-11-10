# Supported and limitations

## Supported

- Smart HTTP protocol v2 over HTTPS (plain HTTP is accepted for local testing)
- Local-path and `file://` transports, including bare repositories, linked
  worktrees (`.git` files and `commondir`), packed refs, and source packfiles
- Redirects, chunked transfer encoding, and verified TLS
- Branches, annotated tags, remote-tracking refs
- `--branch`/`-b` to check out a branch or tag (detached HEAD for tags)
- `--depth <n>` shallow clones (writes `.git/shallow`), with `--single-branch`
- `--bare` and `--no-checkout` clones
- `--origin <name>` to name the remote, including non-`origin` remote-tracking
  refs; `fetch`, `pull`, and `update` use the first configured remote
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
  packfiles are unpacked into loose objects rather than copied as packs.
- HTTPS bearer-token authentication is supported (`--token`, `--token-file`,
  `CGC_TOKEN`, `GIT_TOKEN`); SSH keys and other credential helpers are not.
- Protocol v2 only; servers that only speak protocol v0/v1 are rejected.
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
- In a bare repository, `fetch` records `FETCH_HEAD` but does not update refs
  because there is no configured fetch refspec; `pull` is refused. Custom
  refspecs, multiple remotes, and `--mirror` are not supported yet.
- Pull reads `.git/index` (versions 2 and 3) to detect staged changes and
  preserve stat data and flags; other index versions are rejected.
- Submodules are represented as empty directories; they are not cloned or
  updated.
- Tags are fetched in full rather than Git's reachability-based auto-follow.
  A conflicting local tag is refused unless `--force` is given.
- No partial/filtered clone, no submodule recursion (`--recursive`), no Git LFS,
  no hooks, no reflogs.
- No retry or resume on network failures.

## Roadmap

- Stream packfiles and objects to bound memory use
- Keep the pack and write a `.idx` instead of exploding into loose objects
- Merge/rebase support and shallow fetch (`--deepen`)
- `--mirror`, configurable refspecs, and multiple remotes
- Connect/read timeouts, retries with backoff, and proxy support
- Protocol v0 fallback and SSH transport
- Partial clone
- Submodule clone and update
- Parallel batch cloning from a list or config
