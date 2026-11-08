#!/usr/bin/env bash
# End-to-end smoke test: build cgc, serve a generated repository over smart
# HTTP, clone it with cgc, then let git verify the result.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
PORT=${PORT:-$((8000 + RANDOM % 1000))}
SERVER_PID=

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
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
git -C "$WORK/src" tag -a v1 -m "release one"

git clone -q --bare "$WORK/src" "$WORK/server/repo.git"
git -C "$WORK/server/repo.git" symbolic-ref HEAD refs/heads/main

python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT" >/dev/null 2>&1 &
SERVER_PID=$!

URL="http://127.0.0.1:$PORT/repo.git"
ready=0
for _ in $(seq 1 50); do
    if git ls-remote "$URL" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ "$ready" != 1 ]]; then
    echo "smoke: test server did not start" >&2
    exit 1
fi

"$ROOT/cgc" -q "$URL" "$WORK/clone"

cd "$WORK/clone"
git fsck --full
test -z "$(git status --porcelain)"
test -x run.sh
test -L link.txt
test "$(git rev-parse HEAD)" = "$(git -C "$WORK/src" rev-parse HEAD)"
test "$(git rev-parse refs/tags/v1)" = "$(git -C "$WORK/src" rev-parse refs/tags/v1)"

echo "smoke test passed"
