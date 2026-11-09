# Supported and limitations

## Supported

- Smart HTTP protocol v2 over HTTPS (plain HTTP is accepted for local testing)
- Redirects, chunked transfer encoding, and verified TLS
- Branches, annotated tags, remote-tracking refs
- `--branch`/`-b` to check out a branch or tag (detached HEAD for tags)
- `--depth <n>` shallow clones (writes `.git/shallow`), with `--single-branch`
- `ofs-delta` and `ref-delta` resolution, including delta chains
- Nested trees, symlinks, executable files
- Empty repositories (creates `.git` with the advertised default branch)
- A Git-compatible index: `git status` and `git log` work immediately
- Validation of server-supplied ref names, `HEAD` targets, and config values
- Checkout that rejects duplicate paths, file/directory collisions, and
  symlinked parent directories

## Limitations (v1)

- No SSH, `git://` or local-path transports.
- HTTPS bearer-token authentication is supported (`--token`, `--token-file`,
  `CGC_TOKEN`, `GIT_TOKEN`); SSH keys and other credential helpers are not.
- Protocol v2 only; servers that only speak protocol v0/v1 are rejected.
- SHA-1 repositories only (SHA-256 object format is detected and rejected).
- The whole pack and all inflated objects are held in memory, and a single
  object larger than 2 GiB is rejected. Small and medium repositories are fine;
  huge repositories need a streaming implementation.
- No partial/filtered clone, no submodule recursion (`--recursive`), no Git LFS,
  no hooks, no reflogs.
- No retry or resume on network failures.

## Roadmap

- Stream packfiles and objects to bound memory use
- Keep the pack and write a `.idx` instead of exploding into loose objects
- Incremental fetch, pull, and update of an existing clone
- Connect/read timeouts, retries with backoff, and proxy support
- Protocol v0 fallback and SSH transport
- Partial clone
- Parallel batch cloning from a list or config
