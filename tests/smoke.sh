#!/usr/bin/env bash
# End-to-end smoke test: build cgc, serve generated repositories over smart
# HTTP (Content-Length and chunked), clone them with cgc, then let git verify
# the results.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
PORT=${PORT:-$((8000 + RANDOM % 500))}
PORT2=$((PORT + 500))
SERVER_PID=
CHUNKED_PID=

cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "$CHUNKED_PID" ]] && kill "$CHUNKED_PID" 2>/dev/null || true
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

URL="http://127.0.0.1:$PORT/repo.git"
URL2="http://127.0.0.1:$PORT2/repo.git"
EMPTY_URL="http://127.0.0.1:$PORT/empty.git"

wait_ready() {
    for _ in $(seq 1 50); do
        if git ls-remote "$1" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

if ! wait_ready "$URL" || ! wait_ready "$URL2"; then
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

# Staged changes are refused rather than mixed with the update.
printf 'staged\n' >> run.sh
git add run.sh
printf 'staged upstream\n' > "$WORK/src/unaffected.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "staged upstream"
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
if "$ROOT/cgc" pull -q >/dev/null 2>&1; then
    echo "smoke: pull accepted staged changes" >&2
    exit 1
fi
test "$(git rev-parse HEAD)" = "$UNAFFECTED_HEAD"
test "$(cat unaffected.txt)" = unaffected
git reset -q HEAD run.sh
git checkout -- run.sh
"$ROOT/cgc" pull -q
test "$(cat unaffected.txt)" = "staged upstream"

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

echo "smoke test passed"
