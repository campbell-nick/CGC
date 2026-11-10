# Usage

```
cgc [options] <repository> [directory]
```

If `directory` is omitted, it is derived from the repository name (with a
trailing `.git` removed). With `--bare`, the derived name keeps a `.git`
suffix, matching `git clone --bare`.

## Repository forms

| Argument | Resolves to |
| --- | --- |
| `https://github.com/owner/repo.git` | the URL as given |
| `/path/to/repo`, `./repo`, `~/repo` | the local repository path |
| `file:///path/to/repo` | the local repository path |
| `gh:owner/repo` | `https://github.com/owner/repo.git` |
| `gl:owner/repo` | `https://gitlab.com/owner/repo.git` |
| `bb:owner/repo` | `https://bitbucket.org/owner/repo.git` |
| `sym:owner/repo` | `https://git.symlinx.net/owner/repo.git` |
| `owner/repo` | `https://<default-host>/owner/repo.git` |
| `host.tld/owner/repo` | `https://host.tld/owner/repo.git` |

The default host is `github.com`, overridable with `--host` or the
`CGC_DEFAULT_HOST` environment variable.

Local repositories are read directly: `cgc` reads their refs and objects and
copies them into the destination, including packed objects. Working trees with
a `.git` directory or `.git` file (linked worktrees), bare repositories, and
`file://` URLs are supported. `git` is never invoked.

## Examples

```sh
cgc gh:octocat/Hello-World
cgc sym:team/repo
cgc octocat/Hello-World my-copy
cgc https://github.com/torvalds/linux.git
cgc --host git.example.com team/repo
CGC_DEFAULT_HOST=git.example.com cgc team/repo
cgc --bare gh:owner/repo
cgc --no-checkout gh:owner/repo
cgc --origin upstream gh:owner/repo
cgc --no-tags gh:owner/repo
cgc /srv/git/project.git
cgc file:///srv/git/project.git project-copy
```

## Subcommands

```
cgc fetch  [options] [directory]
cgc pull   [options] [directory]
cgc update [options] [directory]
```

`fetch` downloads new objects and updates `refs/remotes/<remote>/*` and tags
without touching local branches or the working tree. `pull` (and its alias
`update`) does the same, then fast-forwards the current branch and updates the
working tree and index. The remote name and URL are read from the first remote
section in `.git/config`, so clones made with `--origin <name>` keep working.
`directory` defaults to the current directory; parent directories are searched
for the repository.

In a bare repository, `fetch` writes `FETCH_HEAD` and leaves refs unchanged,
matching Git's default when no fetch refspec is configured. `pull` is refused
in a bare repository because there is no working tree to update.

When the configured remote is a local path or `file://` URL, `fetch` and `pull`
copy objects directly from the source repository, including unpacking any
source packfiles. Local transfers copy all objects rather than negotiating with
`have` lines, so they may copy more data than an HTTP fetch.

Pull is fast-forward-only. It refuses when the remote branch is not a
descendant of the local branch, when an affected file has local modifications,
or when an untracked path would be overwritten. Staged changes are preserved
when they do not overlap the update; a staged change to a path the update
touches is refused unless it already matches the new tree. Fetching into a
shallow clone is not supported yet.

`--branch <name>` limits `fetch` to that branch (tags are still fetched) and
selects the branch for `pull`/`update`. Pull refuses to switch away from an
existing branch; check out the target branch first. It can create and check out
a branch when `HEAD` is unborn or detached. From a detached `HEAD` it switches
to the branch, fast-forwarding it when possible, keeping the local tip when it
is ahead, and refusing when local and remote have diverged.

Tags are fetched by default. An existing local tag is never overwritten when
the remote tag points at a different object; use `--force` to allow that.
`--force` only relaxes tag clobber protection; pull remains fast-forward-only.
`--no-tags` skips tags entirely. `--prune` deletes remote-tracking refs for
branches deleted upstream, including entries in `packed-refs`; it is skipped
when `--branch` limits the fetch. `fetch` and `pull` write `.git/FETCH_HEAD`,
and `pull` writes `.git/ORIG_HEAD` before moving `HEAD`.

## Options

```
--host <host>   default host for owner/repo shorthand
--token <tok>   bearer token for HTTPS authentication (env CGC_TOKEN/GIT_TOKEN)
--token-file <path>
                read the bearer token from the first line of <path>
-b, --branch <name>
                clone: check out <name> (branch or tag) instead of the
                remote's default branch; tags use a detached HEAD
                fetch/pull: limit or select the branch (see Subcommands)
--tags          fetch and update all advertised tags (the default)
--no-tags       do not fetch or update tags
--force         overwrite a local tag that differs from the remote
--prune         delete stale refs under refs/remotes/<remote>
--depth <n>     create a shallow clone with history truncated to n commits
                (HTTP sources only; rejected for local repositories)
--single-branch clone only the history of one branch
--no-single-branch
                fetch all branches (default unless --depth is given)
--bare          create a bare repository (no working tree or index)
--no-checkout   write refs and config but do not check out files
--origin <name> name the remote (default origin)
-q, --quiet     suppress progress messages
-h, --help      show help
-V, --version   show version
```

`--host`, `--depth`, `--single-branch`, `--no-single-branch`, `--bare`,
`--no-checkout`, and `--origin` are only valid for cloning. `--tags`, `--force`,
and `--prune` are only valid for `fetch`, `pull`, and `update`. `--no-tags` is
valid for cloning and for `fetch`, `pull`, and `update`.

`--bare` stores refs, objects, and configuration directly in the destination
instead of a `.git` subdirectory. `--no-checkout` still writes `HEAD`, refs,
and configuration but leaves the index and working tree empty; `git status`
will therefore report the files as deleted until a checkout is performed.
`--origin` is validated and used consistently in config sections, branch
upstream settings, and remote-tracking refs.

## Output

Status is written to stderr in the same style as `git clone`: `Cloning into
'...'...`, any `remote:` messages from the server, a `Receiving objects`
download meter, `Resolving deltas`, and `Updating files` during checkout. Live
meters are drawn on a terminal; a final summary line is printed when output is
redirected. `--quiet` suppresses all of it.

`fetch` prints a `From <url>` block listing new and updated branches and tags.
`pull` additionally prints `Updating <old>..<new>` and `Fast-forward`, or
`Already up to date.` when there is nothing to do.

## Safety

`cgc` treats the remote repository as untrusted. Server-provided ref names and
`HEAD` targets are validated before they are written to disk, values written to
`.git/config` are rejected if they contain control characters, and checkout
refuses trees with duplicate paths, file/directory collisions, or paths whose
existing parent is a symlink. This prevents a malicious repository from
escaping the target directory or injecting configuration.

`pull` checks the worktree before changing it: an affected file must still
match the old commit, and an added path must not already exist as an untracked
file. If either check fails, nothing is written. Unrelated staged changes are
kept in the index, including `skip-worktree` and `assume-unchanged` flags. Pull
never creates a merge commit or rebases.

Ref updates are written through a sibling `.lock` file and renamed into place.
`index`, `FETCH_HEAD`, and `ORIG_HEAD` are written atomically as well, so an
interrupted update does not leave a truncated file behind.

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
