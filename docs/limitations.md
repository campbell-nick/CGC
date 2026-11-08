# Supported and limitations

## Supported

- Smart HTTP protocol v2 over HTTPS (plain HTTP is accepted for local testing)
- Redirects, chunked transfer encoding, and verified TLS
- Branches, annotated tags, remote-tracking refs
- `ofs-delta` and `ref-delta` resolution, including delta chains
- Nested trees, symlinks, executable files
- Empty repositories (creates `.git` with the advertised default branch)
- A Git-compatible index: `git status` and `git log` work immediately
- Validation of server-supplied ref names, `HEAD` targets, and config values
- Checkout that rejects duplicate paths, file/directory collisions, and
  symlinked parent directories

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

## Roadmap

- Stream packfiles and objects to bound memory use
- Keep the pack and write a `.idx` instead of exploding into loose objects
- Protocol v0 fallback and SSH transport
- Credential/token authentication
- Shallow and partial clone
- Parallel batch cloning from a list or config
