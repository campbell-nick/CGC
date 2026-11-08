#!/usr/bin/env bash
# Packfile round-trip test: build packs with git, unpack them with cgc's
# pack_unpack, and compare the resulting object database with the source.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"
make >/dev/null

mkdir -p "$WORK/src"
git -C "$WORK/src" init -q -b main
git -C "$WORK/src" config user.email test@example.com
git -C "$WORK/src" config user.name Test

for i in $(seq 1 40); do
    seq 1 500 > "$WORK/src/file$i.txt"
done
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m initial
for i in $(seq 1 6); do
    for f in 1 7 13 29 37; do
        printf 'revision %s\n' "$i" >> "$WORK/src/file$f.txt"
    done
    git -C "$WORK/src" add -A
    git -C "$WORK/src" commit -q -m "change $i"
done
git -C "$WORK/src" tag -a v1 -m release

list_objects() {
    git -C "$1" cat-file --batch-all-objects \
        --batch-check='%(objectname) %(objecttype) %(objectsize)' | sort
}

run_case() {
    local name=$1 pack=$2 stats objects deltas
    git init -q --bare "$WORK/$name-db"
    stats=$("$ROOT/tests/packtest" "$pack" "$WORK/$name-db")
    read -r objects deltas <<<"$stats"
    if [[ "$objects" -le 0 || "$deltas" -le 0 ]]; then
        echo "pack: $name pack had $objects objects and $deltas deltas" >&2
        exit 1
    fi
    if ! diff -q <(list_objects "$WORK/src") \
                 <(list_objects "$WORK/$name-db") >/dev/null; then
        echo "pack: $name object database does not match the source" >&2
        exit 1
    fi
    echo "pack: $name ok ($objects objects, $deltas deltas)"
}

git -C "$WORK/src" repack -adq
ofs_pack=$(find "$WORK/src/.git/objects/pack" -name '*.pack' | head -1)
run_case ofs "$ofs_pack"

git -C "$WORK/src" pack-objects --all --no-delta-base-offset --stdout \
    >"$WORK/ref.pack"
run_case ref "$WORK/ref.pack"

echo "pack test passed"
