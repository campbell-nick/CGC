#!/usr/bin/env bash
# End-to-end smoke test: build cgc, serve generated repositories over smart
# HTTP (Content-Length and chunked), clone them with cgc, then let git verify
# the results.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
PORT=${PORT:-$((8000 + RANDOM % 500))}
PORT2=$((PORT + 500))
PORT3=$((PORT + 1000))
SERVER_PID=
CHUNKED_PID=
V0_PID=

cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "$CHUNKED_PID" ]] && kill "$CHUNKED_PID" 2>/dev/null || true
    [[ -n "$V0_PID" ]] && kill "$V0_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

cd "$ROOT"
make >/dev/null

mkdir -p "$WORK/server" "$WORK/src"
git -C "$WORK/src" init -q -b main
git -C "$WORK/src" config user.email test@example.com
git -C "$WORK/src" config user.name Test

printf 'hello\n' > "$WORK/src/README.md"
printf '#!/bin/sh\necho hi\n' > "$WORK/src/run.sh"
chmod +x "$WORK/src/run.sh"
ln -s README.md "$WORK/src/link.txt"
printf 'unicode\n' > "$WORK/src/naïve.txt"
printf 'spaces\n' > "$WORK/src/file with space.txt"
mkdir -p "$WORK/src/src/deep"
printf 'a\n' > "$WORK/src/src/deep/a.txt"
seq 1 1500 > "$WORK/src/big.txt"

git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m initial
for i in 1 2 3 4; do
    seq "$i" 3000 >> "$WORK/src/big.txt"
    printf 'revision %s\n' "$i" > "$WORK/src/src/deep/r$i.txt"
    git -C "$WORK/src" add -A
    git -C "$WORK/src" commit -q -m "change $i"
done
git -C "$WORK/src" checkout -q -b dev
printf 'branch\n' > "$WORK/src/dev.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "dev branch"
git -C "$WORK/src" checkout -q main
git -C "$WORK/src" tag -a v1 -m "release one"
git -C "$WORK/src" tag light

git clone -q --bare "$WORK/src" "$WORK/server/repo.git"
git -C "$WORK/server/repo.git" symbolic-ref HEAD refs/heads/main
git init -q --bare "$WORK/server/empty.git"
git -C "$WORK/server/empty.git" symbolic-ref HEAD refs/heads/main

python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!
python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT2" chunked \
    >/dev/null 2>&1 &
CHUNKED_PID=$!
python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT3" v0 \
    >/dev/null 2>&1 &
V0_PID=$!

URL="http://127.0.0.1:$PORT/repo.git"
URL2="http://127.0.0.1:$PORT2/repo.git"
URL3="http://127.0.0.1:$PORT3/repo.git"
EMPTY_URL="http://127.0.0.1:$PORT/empty.git"
EMPTY_URL3="http://127.0.0.1:$PORT3/empty.git"

wait_ready() {
    for _ in $(seq 1 50); do
        if git ls-remote "$1" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

if ! wait_ready "$URL" || ! wait_ready "$URL2" || ! wait_ready "$URL3"; then
    echo "smoke: test server did not start" >&2
    exit 1
fi

"$ROOT/cgc" -q "$URL" "$WORK/clone"

cd "$WORK/clone"
git fsck --full
test -z "$(git status --porcelain)"
test -x run.sh
test -L link.txt
test -f "naïve.txt"
test -f "file with space.txt"
test "$(git rev-parse HEAD)" = "$(git -C "$WORK/src" rev-parse HEAD)"
test "$(git rev-parse refs/heads/dev)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/dev)"
test "$(git rev-parse refs/remotes/origin/dev)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/dev)"
test "$(git rev-parse refs/tags/v1)" = \
     "$(git -C "$WORK/src" rev-parse refs/tags/v1)"
test "$(git rev-parse refs/tags/light)" = \
     "$(git -C "$WORK/src" rev-parse refs/tags/light)"
cd "$ROOT"

# Status output: non-TTY runs still print the final summary lines.
run_out=$("$ROOT/cgc" "$URL" "$WORK/verbose" 2>&1)
for needle in "Cloning into" "Receiving objects" "Resolving deltas" \
              "Updating files"; do
    if ! grep -q "$needle" <<<"$run_out"; then
        echo "smoke: clone output is missing '$needle'" >&2
        exit 1
    fi
done

# --quiet must not print any status.
quiet_out=$("$ROOT/cgc" -q "$URL" "$WORK/quiet" 2>&1)
if [[ -n "$quiet_out" ]]; then
    echo "smoke: --quiet produced output:" >&2
    printf '%s\n' "$quiet_out" >&2
    exit 1
fi

# Chunked transfer encoding.
"$ROOT/cgc" -q "$URL2" "$WORK/chunked"
git -C "$WORK/chunked" fsck --full
test -z "$(git -C "$WORK/chunked" status --porcelain)"
test "$(git -C "$WORK/chunked" rev-parse HEAD)" = \
     "$(git -C "$WORK/src" rev-parse HEAD)"

# Empty repository: success plus the Git-style warning.
empty_out=$("$ROOT/cgc" "$EMPTY_URL" "$WORK/empty" 2>&1)
grep -q "You appear to have cloned an empty repository" <<<"$empty_out"
test -f "$WORK/empty/.git/HEAD"
grep -q "refs/heads/main" "$WORK/empty/.git/HEAD"

# --bare creates a bare repository: no worktree, no .git directory, and refs
# stored directly under the destination.
"$ROOT/cgc" -q --bare "$URL" "$WORK/bare.git"
test -d "$WORK/bare.git/objects"
test ! -e "$WORK/bare.git/.git"
test ! -e "$WORK/bare.git/README.md"
test "$(git -C "$WORK/bare.git" rev-parse --is-bare-repository)" = true
test "$(git -C "$WORK/bare.git" rev-parse refs/heads/main)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/main)"
test "$(git -C "$WORK/bare.git" rev-parse refs/tags/v1)" = \
     "$(git -C "$WORK/src" rev-parse refs/tags/v1)"
git -C "$WORK/bare.git" fsck --full
BARE_HEAD=$(git -C "$WORK/bare.git" rev-parse refs/heads/main)

# --no-checkout writes refs and HEAD but no index or working-tree files.
"$ROOT/cgc" -q --no-checkout "$URL" "$WORK/nocheckout"
test ! -e "$WORK/nocheckout/.git/index"
test ! -e "$WORK/nocheckout/README.md"
test "$(git -C "$WORK/nocheckout" rev-parse HEAD)" = \
     "$(git -C "$WORK/src" rev-parse HEAD)"
test "$(git -C "$WORK/nocheckout" rev-parse refs/remotes/origin/main)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/main)"

# --origin names the remote and its remote-tracking refs.
"$ROOT/cgc" -q --origin upstream "$URL" "$WORK/renamed"
grep -q '\[remote "upstream"\]' "$WORK/renamed/.git/config"
test "$(git -C "$WORK/renamed" rev-parse refs/remotes/upstream/main)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/main)"
test "$(git -C "$WORK/renamed" rev-parse refs/remotes/upstream/dev)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/dev)"
test ! -e "$WORK/renamed/.git/refs/remotes/origin"

# --no-tags leaves tags out of a clone.
"$ROOT/cgc" -q --no-tags "$URL" "$WORK/notags"
test "$(git -C "$WORK/notags" rev-parse refs/heads/main)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/main)"
test -z "$(git -C "$WORK/notags" for-each-ref --format='%(refname)' refs/tags)"

# A new upstream commit exercises fetch with a non-origin remote and a bare
# repository. Bare fetch records FETCH_HEAD but leaves refs untouched.
printf 'renamed remote\n' > "$WORK/src/renamed.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "renamed remote"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
RENAMED_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
(cd "$WORK/renamed" && "$ROOT/cgc" fetch -q)
test "$(git -C "$WORK/renamed" rev-parse refs/remotes/upstream/main)" = \
     "$RENAMED_HEAD"
test ! -e "$WORK/renamed/.git/refs/remotes/origin"

(cd "$WORK/bare.git" && "$ROOT/cgc" fetch -q)
test "$(git -C "$WORK/bare.git" rev-parse refs/heads/main)" = "$BARE_HEAD"
grep -q "^$RENAMED_HEAD" "$WORK/bare.git/FETCH_HEAD"
if (cd "$WORK/bare.git" && "$ROOT/cgc" pull -q >/dev/null 2>&1); then
    echo "smoke: pull was accepted in a bare repository" >&2
    exit 1
fi

# Invalid --depth values are rejected before any network access.
for depth in 0 -1 abc 12x 999999999999999999999; do
    if "$ROOT/cgc" --depth "$depth" gh:owner/repo "$WORK/bad-depth" \
            >/dev/null 2>&1; then
        echo "smoke: --depth '$depth' was accepted" >&2
        exit 1
    fi
done
test ! -e "$WORK/bad-depth"

# --token-file must fail cleanly when the file is missing or empty.
if "$ROOT/cgc" --token-file "$WORK/no-such-token" gh:owner/repo \
        "$WORK/bad-token" >/dev/null 2>&1; then
    echo "smoke: missing --token-file was accepted" >&2
    exit 1
fi
: > "$WORK/empty-token"
if "$ROOT/cgc" --token-file "$WORK/empty-token" gh:owner/repo \
        "$WORK/bad-token" >/dev/null 2>&1; then
    echo "smoke: empty --token-file was accepted" >&2
    exit 1
fi
test ! -e "$WORK/bad-token"
printf '  test-token  \nignored\n' > "$WORK/token"
"$ROOT/cgc" --token-file "$WORK/token" --help >/dev/null

# ---------------------------------------------------------------------------
# fetch / pull / update
# ---------------------------------------------------------------------------
cd "$WORK/clone"

printf 'hello again\n' > "$WORK/src/README.md"
printf 'pulled\n' > "$WORK/src/pulled.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "upstream change"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
NEW_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
OLD_HEAD=$(git rev-parse HEAD)

# fetch updates remote-tracking refs but not HEAD or the worktree.
"$ROOT/cgc" fetch -q
test "$(git rev-parse refs/remotes/origin/main)" = "$NEW_HEAD"
test "$(git rev-parse HEAD)" = "$OLD_HEAD"
test ! -e pulled.txt

# pull fast-forwards HEAD and the worktree.
"$ROOT/cgc" pull -q
test "$(git rev-parse HEAD)" = "$NEW_HEAD"
test "$(cat README.md)" = "hello again"
test -f pulled.txt
git fsck --full
test -z "$(git status --porcelain)"

# A dirty worktree is not overwritten.
printf 'upstream dirty\n' > "$WORK/src/README.md"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "dirty upstream"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
DIRTY_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
printf 'local dirty\n' > README.md
if "$ROOT/cgc" pull -q >/dev/null 2>&1; then
    echo "smoke: pull overwrote a dirty worktree" >&2
    exit 1
fi
test "$(cat README.md)" = "local dirty"
test "$(git rev-parse HEAD)" = "$NEW_HEAD"
git checkout -- README.md
"$ROOT/cgc" pull -q
test "$(cat README.md)" = "upstream dirty"
test "$(git rev-parse HEAD)" = "$DIRTY_HEAD"

# Local modifications to unaffected files survive a pull and stay visible.
printf 'local edit\n' >> run.sh
printf 'unaffected\n' > "$WORK/src/unaffected.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "unaffected commit"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
UNAFFECTED_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
"$ROOT/cgc" pull -q
test "$(git rev-parse HEAD)" = "$UNAFFECTED_HEAD"
test -f unaffected.txt
grep -q 'local edit' run.sh
test "$(git status --porcelain)" = " M run.sh"
git checkout -- run.sh
test -z "$(git status --porcelain)"

# Unrelated staged changes survive a pull and remain staged.
printf 'staged\n' >> run.sh
git add run.sh
printf 'staged upstream\n' > "$WORK/src/unaffected.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "staged upstream"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
"$ROOT/cgc" pull -q
test "$(cat unaffected.txt)" = "staged upstream"
test "$(git status --porcelain)" = "M  run.sh"
git reset -q HEAD run.sh
git checkout -- run.sh
test -z "$(git status --porcelain)"

# A staged change that overlaps the update is refused.
printf 'staged conflict\n' >> unaffected.txt
git add unaffected.txt
printf 'overlap upstream\n' > "$WORK/src/unaffected.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "overlap upstream"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
OVERLAP_HEAD=$(git rev-parse HEAD)
if "$ROOT/cgc" pull -q >/dev/null 2>&1; then
    echo "smoke: pull accepted overlapping staged changes" >&2
    exit 1
fi
test "$(git rev-parse HEAD)" = "$OVERLAP_HEAD"
git reset -q HEAD unaffected.txt
git checkout -- unaffected.txt
"$ROOT/cgc" pull -q
test "$(cat unaffected.txt)" = "overlap upstream"

# update is an alias for pull.
printf 'alias\n' > "$WORK/src/alias.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "alias commit"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
ALIAS_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
"$ROOT/cgc" update -q
test "$(git rev-parse HEAD)" = "$ALIAS_HEAD"
test -f alias.txt
test -z "$(git status --porcelain)"

# A non-fast-forward is refused and leaves HEAD alone.
git -C "$WORK/src" reset -q --hard HEAD~1
printf 'divergent\n' > "$WORK/src/divergent.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m divergent
git -C "$WORK/src" push -q -f "$WORK/server/repo.git" main
if "$ROOT/cgc" pull -q >/dev/null 2>&1; then
    echo "smoke: pull accepted a non-fast-forward" >&2
    exit 1
fi
test "$(git rev-parse HEAD)" = "$ALIAS_HEAD"
test ! -e divergent.txt

# --branch limits a fetch to that branch (plus tags).
"$ROOT/cgc" fetch -q --branch dev
test "$(git rev-parse refs/remotes/origin/dev)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/dev)"

# Pull --branch from a detached HEAD checks out the branch.
git checkout -q --detach HEAD
"$ROOT/cgc" pull -q --branch dev
test "$(git symbolic-ref HEAD)" = refs/heads/dev
test -f dev.txt
test -z "$(git status --porcelain)"

cd "$ROOT"

# ---------------------------------------------------------------------------
# protocol v0 fallback
# ---------------------------------------------------------------------------
"$ROOT/cgc" -q "$URL3" "$WORK/v0clone"
git -C "$WORK/v0clone" fsck --full
test -z "$(git -C "$WORK/v0clone" status --porcelain)"
test "$(git -C "$WORK/v0clone" rev-parse HEAD)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/main)"
test "$(git -C "$WORK/v0clone" rev-parse refs/remotes/origin/dev)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/dev)"
test "$(git -C "$WORK/v0clone" rev-parse refs/tags/v1)" = \
     "$(git -C "$WORK/src" rev-parse refs/tags/v1)"

# v0 peeled tags and detached checkout from --branch.
"$ROOT/cgc" -q --branch v1 "$URL3" "$WORK/v0tag"
test "$(git -C "$WORK/v0tag" rev-parse HEAD)" = \
     "$(git -C "$WORK/src" rev-parse 'v1^{commit}')"
test -z "$(git -C "$WORK/v0tag" symbolic-ref -q HEAD)"

# v0 shallow clone writes .git/shallow and truncates history.
"$ROOT/cgc" -q --depth 1 "$URL3" "$WORK/v0depth"
test -f "$WORK/v0depth/.git/shallow"
test "$(git -C "$WORK/v0depth" rev-list --count HEAD)" = 1
git -C "$WORK/v0depth" fsck --full
test -z "$(git -C "$WORK/v0depth" status --porcelain)"

# v0 empty repository.
empty_out=$("$ROOT/cgc" "$EMPTY_URL3" "$WORK/v0empty" 2>&1)
grep -q "You appear to have cloned an empty repository" <<<"$empty_out"

# v0 fetch and pull use have negotiation and update refs/worktree.
printf 'v0 fetch\n' > "$WORK/src/v0-fetch.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "v0 fetch"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
V0_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
(cd "$WORK/v0clone" && "$ROOT/cgc" fetch -q)
test "$(git -C "$WORK/v0clone" rev-parse refs/remotes/origin/main)" = "$V0_HEAD"
test ! -e "$WORK/v0clone/v0-fetch.txt"
(cd "$WORK/v0clone" && "$ROOT/cgc" pull -q)
test "$(git -C "$WORK/v0clone" rev-parse HEAD)" = "$V0_HEAD"
test -f "$WORK/v0clone/v0-fetch.txt"
git -C "$WORK/v0clone" fsck --full
test -z "$(git -C "$WORK/v0clone" status --porcelain)"

# ---------------------------------------------------------------------------
# local-path transport
# ---------------------------------------------------------------------------
git -C "$WORK/src" gc -q

"$ROOT/cgc" -q "$WORK/src" "$WORK/local"
git -C "$WORK/local" fsck --full
test -z "$(git -C "$WORK/local" status --porcelain)"
test "$(git -C "$WORK/local" rev-parse HEAD)" = \
     "$(git -C "$WORK/src" rev-parse HEAD)"
test "$(git -C "$WORK/local" rev-parse refs/remotes/origin/dev)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/dev)"

# A bare source exercises packed-refs and packfile unpacking.
"$ROOT/cgc" -q "$WORK/server/repo.git" "$WORK/localbare"
git -C "$WORK/localbare" fsck --full
test -z "$(git -C "$WORK/localbare" status --porcelain)"
test "$(git -C "$WORK/localbare" rev-parse HEAD)" = \
     "$(git -C "$WORK/server/repo.git" rev-parse HEAD)"

# file:// URLs resolve to the same local transport.
"$ROOT/cgc" -q "file://$WORK/src" "$WORK/localfile"
git -C "$WORK/localfile" fsck --full
test -z "$(git -C "$WORK/localfile" status --porcelain)"

# An annotated tag selected from a local source is peeled to its commit.
"$ROOT/cgc" -q --branch v1 "$WORK/src" "$WORK/localtag"
test "$(git -C "$WORK/localtag" rev-parse HEAD)" = \
     "$(git -C "$WORK/src" rev-parse 'v1^{commit}')"
test -z "$(git -C "$WORK/localtag" symbolic-ref -q HEAD)"
git -C "$WORK/localtag" fsck --full
test -z "$(git -C "$WORK/localtag" status --porcelain)"

# Local --bare and --no-tags behave like their HTTP equivalents.
"$ROOT/cgc" -q --bare "$WORK/src" "$WORK/local.git"
test "$(git -C "$WORK/local.git" rev-parse --is-bare-repository)" = true
test ! -e "$WORK/local.git/.git"
git -C "$WORK/local.git" fsck --full

"$ROOT/cgc" -q --no-tags "$WORK/src" "$WORK/localnotags"
test -z "$(git -C "$WORK/localnotags" for-each-ref \
     --format='%(refname)' refs/tags)"

# Local fetch and pull follow a non-origin remote.
"$ROOT/cgc" -q --origin upstream "$WORK/src" "$WORK/localorigin"
test "$(git -C "$WORK/localorigin" rev-parse refs/remotes/upstream/main)" = \
     "$(git -C "$WORK/src" rev-parse refs/heads/main)"
test ! -e "$WORK/localorigin/.git/refs/remotes/origin"

printf 'local pull\n' > "$WORK/src/local-pull.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "local pull"
LOCAL_PULL_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
(cd "$WORK/localorigin" && "$ROOT/cgc" pull -q)
test -f "$WORK/localorigin/local-pull.txt"
test "$(git -C "$WORK/localorigin" rev-parse HEAD)" = "$LOCAL_PULL_HEAD"
test "$(git -C "$WORK/localorigin" rev-parse refs/remotes/upstream/main)" = \
     "$LOCAL_PULL_HEAD"
git -C "$WORK/localorigin" fsck --full
test -z "$(git -C "$WORK/localorigin" status --porcelain)"

# A bare local repository records FETCH_HEAD and leaves refs alone.
"$ROOT/cgc" -q --bare "$WORK/src" "$WORK/localfetch.git"
LOCAL_BARE_HEAD=$(git -C "$WORK/localfetch.git" rev-parse refs/heads/main)
printf 'local fetch\n' > "$WORK/src/local-fetch.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "local fetch"
LOCAL_FETCH_HEAD=$(git -C "$WORK/src" rev-parse HEAD)
(cd "$WORK/localfetch.git" && "$ROOT/cgc" fetch -q)
test "$(git -C "$WORK/localfetch.git" rev-parse refs/heads/main)" = \
     "$LOCAL_BARE_HEAD"
grep -q "^$LOCAL_FETCH_HEAD" "$WORK/localfetch.git/FETCH_HEAD"

# A linked worktree is found through its .git file and commondir.
git -C "$WORK/src" worktree add -q "$WORK/linked" dev
"$ROOT/cgc" -q "$WORK/linked" "$WORK/linkedclone"
git -C "$WORK/linkedclone" fsck --full
test -z "$(git -C "$WORK/linkedclone" status --porcelain)"
test "$(git -C "$WORK/linkedclone" symbolic-ref HEAD)" = refs/heads/dev
git -C "$WORK/src" worktree remove "$WORK/linked"

# --depth is rejected for local repositories before creating anything.
if "$ROOT/cgc" --depth 1 "$WORK/src" "$WORK/localdepth" >/dev/null 2>&1; then
    echo "smoke: --depth was accepted for a local repository" >&2
    exit 1
fi
test ! -e "$WORK/localdepth"

echo "smoke test passed"
