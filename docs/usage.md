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

## Verifying a clone

```sh
cd <cloned-repo>
git fsck --full
git status
git log --oneline -5
```
