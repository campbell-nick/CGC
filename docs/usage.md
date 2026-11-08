# Usage

```
cgc [options] <repository> [directory]
```

If `directory` is omitted, it is derived from the repository name (with a
trailing `.git` removed).

## Repository forms

| Argument | Resolves to |
| --- | --- |
| `https://github.com/owner/repo.git` | the URL as given |
| `gh:owner/repo` | `https://github.com/owner/repo.git` |
| `gl:owner/repo` | `https://gitlab.com/owner/repo.git` |
| `bb:owner/repo` | `https://bitbucket.org/owner/repo.git` |
| `sym:owner/repo` | `https://git.symlinx.net/owner/repo.git` |
| `owner/repo` | `https://<default-host>/owner/repo.git` |
| `host.tld/owner/repo` | `https://host.tld/owner/repo.git` |

The default host is `github.com`, overridable with `--host` or the
`CGC_DEFAULT_HOST` environment variable.

## Examples

```sh
cgc gh:octocat/Hello-World
cgc sym:team/repo
cgc octocat/Hello-World my-copy
cgc https://github.com/torvalds/linux.git
cgc --host git.example.com team/repo
CGC_DEFAULT_HOST=git.example.com cgc team/repo
```

## Options

```
--host <host>   default host for owner/repo shorthand
--token <tok>   bearer token for HTTPS authentication (env CGC_TOKEN/GIT_TOKEN)
-b, --branch <name>
                check out <name> (branch or tag) instead of the remote's
                default branch; tags are checked out in detached HEAD state
--depth <n>     create a shallow clone with history truncated to n commits
--single-branch clone only the history of one branch
--no-single-branch
                fetch all branches (default unless --depth is given)
-q, --quiet     suppress progress messages
-h, --help      show help
-V, --version   show version
```

## Output

Status is written to stderr in the same style as `git clone`: `Cloning into
'...'...`, any `remote:` messages from the server, a `Receiving objects`
download meter, `Resolving deltas`, and `Updating files` during checkout. Live
meters are drawn on a terminal; a final summary line is printed when output is
redirected. `--quiet` suppresses all of it.

## Safety

`cgc` treats the remote repository as untrusted. Server-provided ref names and
`HEAD` targets are validated before they are written to disk, values written to
`.git/config` are rejected if they contain control characters, and checkout
refuses trees with duplicate paths, file/directory collisions, or paths whose
existing parent is a symlink. This prevents a malicious repository from
escaping the target directory or injecting configuration.

These checks protect the clone destination; they are not a sandbox. As with
`git clone`, do not run code from an untrusted repository, and review hooks and
build scripts before executing them.

## Verifying a clone

```sh
cd <cloned-repo>
git fsck --full
git status
git log --oneline -5
```
