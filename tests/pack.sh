#!/usr/bin/env bash
# Packfile round-trip test: build packs with git, unpack them with cgc's
# pack_unpack, and compare the resulting object database with the source.
# Self-contained packs must be retained as pack + index; thin packs must fall
# back to loose objects.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"
make >/dev/null
make tests/packtest >/dev/null

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

count_loose() {
    find "$1/objects" -mindepth 2 -maxdepth 2 -type f \
        ! -path "$1/objects/pack/*" | wc -l
}

count_packs() {
    find "$1/objects/pack" -name '*.pack' 2>/dev/null | wc -l
}

run_case() {
    local name=$1 pack=$2 storage=$3 stats objects deltas packs loose
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

    packs=$(count_packs "$WORK/$name-db")
    loose=$(count_loose "$WORK/$name-db")
    if [[ "$storage" == pack ]]; then
        if [[ "$packs" -ne 1 || "$loose" -ne 0 ]]; then
            echo "pack: $name expected one retained pack and no loose objects" >&2
            echo "pack: $name found $packs pack(s) and $loose loose object(s)" >&2
            exit 1
        fi
        local idx
        idx=$(find "$WORK/$name-db/objects/pack" -name '*.idx')
        if ! git verify-pack -s "$idx" >/dev/null; then
            echo "pack: $name retained pack index is invalid" >&2
            exit 1
        fi
    else
        if [[ "$loose" -le 0 ]]; then
            echo "pack: $name expected thin-pack fallback to loose objects" >&2
            exit 1
        fi
    fi
    echo "pack: $name ok ($objects objects, $deltas deltas, $packs pack(s), $loose loose)"
}

git -C "$WORK/src" repack -adq
ofs_pack=$(find "$WORK/src/.git/objects/pack" -name '*.pack' | head -1)
run_case ofs "$ofs_pack" pack

git -C "$WORK/src" pack-objects --all --no-delta-base-offset --stdout \
    >"$WORK/ref.pack"
run_case ref "$WORK/ref.pack" pack

# A thin pack omits delta bases that the destination already has. cgc must
# resolve those bases from its object database and fall back to loose objects
# rather than retaining a pack that cannot stand on its own.
printf '%s\n^%s\n' HEAD HEAD~4 \
    | git -C "$WORK/src" pack-objects --thin --stdout --revs \
    >"$WORK/thin.pack"
git init -q --bare "$WORK/thin-db"
"$ROOT/tests/packtest" "$ofs_pack" "$WORK/thin-db" >/dev/null
run_case thin "$WORK/thin.pack" loose

echo "pack test passed"
