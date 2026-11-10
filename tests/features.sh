#!/usr/bin/env bash
# Comprehensive feature matrix for cgc. Exercises every documented feature and
# a set of edge cases, and verifies the results with git. Complements
# smoke.sh: smoke.sh covers the main paths, this script goes wider.
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CGC="$ROOT/cgc"
VERSION=$(cat "$ROOT/VERSION")
WORK=$(mktemp -d)
# Local test servers must bypass any proxy configured in the environment.
export NO_PROXY="127.0.0.1,localhost${NO_PROXY:+,$NO_PROXY}"
export no_proxy="$NO_PROXY"
BASE=$((12000 + RANDOM % 10000))
PORT=$BASE
PORT2=$((BASE + 1))
PORT3=$((BASE + 2))
PIDS=()
PASS=0
FAIL=0

cleanup() {
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { PASS=$((PASS + 1)); }
bad()  { FAIL=$((FAIL + 1)); echo "FAIL: $*" >&2; }
eq()   { if [[ "$1" == "$2" ]]; then ok; else bad "$3: got '$1', want '$2'"; fi; }
nz()   { if [[ -n "$1" ]]; then ok; else bad "$2: expected non-empty"; fi; }
z()    { if [[ -z "$1" ]]; then ok; else bad "$2: expected empty, got '$1'"; fi; }
exists(){ if [[ -e "$1" ]]; then ok; else bad "missing path: $1"; fi; }
missing(){ if [[ ! -e "$1" ]]; then ok; else bad "unexpected path: $1"; fi; }
grepf(){ if grep -Fq -- "$2" "$1"; then ok; else bad "$1 does not contain '$2'"; fi; }
fsck(){ if git -C "$1" fsck --full --no-dangling >/dev/null 2>&1; then ok; else bad "fsck failed in $1"; fi; }
clean_status(){ z "$(git -C "$1" status --porcelain 2>/dev/null)" "clean status $1"; }
expect_fail(){ local d=$1; shift; if "$@" >/dev/null 2>&1; then bad "$d: unexpectedly succeeded"; else ok; fi; }
rev(){ git -C "$1" rev-parse --verify --quiet "$2" 2>/dev/null; }

# ---------------------------------------------------------------------------
# Fixture repository
# ---------------------------------------------------------------------------
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
seq 1 2000 > "$WORK/src/big.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m initial
for i in 1 2 3 4; do
    seq "$i" 4000 >> "$WORK/src/big.txt"
    printf 'r%s\n' "$i" > "$WORK/src/src/deep/r$i.txt"
    git -C "$WORK/src" add -A
    git -C "$WORK/src" commit -q -m "change $i"
done

git -C "$WORK/src" checkout -q -b dev
printf 'dev\n' > "$WORK/src/dev.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "dev branch"
git -C "$WORK/src" checkout -q -b feature/x main
printf 'feature\n' > "$WORK/src/feature.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "feature branch"
git -C "$WORK/src" checkout -q main
git -C "$WORK/src" tag -a v1 -m "release one"
git -C "$WORK/src" tag light
git -C "$WORK/src" tag -a devtag -m "dev tag" dev
git -C "$WORK/src" notes add -m "a note" main

git clone -q --mirror "$WORK/src" "$WORK/server/repo.git"
git -C "$WORK/server/repo.git" symbolic-ref HEAD refs/heads/main
git init -q --bare "$WORK/server/empty.git"
git -C "$WORK/server/empty.git" symbolic-ref HEAD refs/heads/main

python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT" >/dev/null 2>&1 &
PIDS+=($!)
python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT2" chunked >/dev/null 2>&1 &
PIDS+=($!)
python3 "$ROOT/tests/serve-http.py" "$WORK/server" "$PORT3" v0 >/dev/null 2>&1 &
PIDS+=($!)

URL="http://127.0.0.1:$PORT/repo.git"
URL2="http://127.0.0.1:$PORT2/repo.git"
URL3="http://127.0.0.1:$PORT3/repo.git"
EMPTY_URL="http://127.0.0.1:$PORT/empty.git"
EMPTY_URL3="http://127.0.0.1:$PORT3/empty.git"

for u in "$URL" "$URL2" "$URL3"; do
    ready=0
    for _ in $(seq 1 100); do
        if git ls-remote "$u" >/dev/null 2>&1; then ready=1; break; fi
        sleep 0.1
    done
    [[ $ready == 1 ]] || { echo "server did not start for $u" >&2; exit 1; }
done

push_upstream() {
    git -C "$WORK/src" add -A
    git -C "$WORK/src" commit -q -m "$1"
    git -C "$WORK/server/repo.git" fetch -q "$WORK/src" '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*'
}

# ---------------------------------------------------------------------------
echo "== CLI, help, and argument validation =="
# ---------------------------------------------------------------------------
"$CGC" --help >"$WORK/help.out" 2>&1 || bad "clone --help failed"
grepf "$WORK/help.out" "--mirror"
"$CGC" fetch --help >"$WORK/fhelp.out" 2>&1 || bad "fetch --help failed"
grepf "$WORK/fhelp.out" "--prune"
grepf "$WORK/fhelp.out" "--remote <name>"
eq "$("$CGC" --version)" "cgc $VERSION" "version string"
expect_fail "unknown option" "$CGC" --bogus
expect_fail "no arguments" "$CGC"
expect_fail "too many arguments" "$CGC" gh:a/b c d
expect_fail "ftp URL" "$CGC" ftp://example.com/x "$WORK/x1"
expect_fail "clone --remote" "$CGC" --remote origin gh:a/b "$WORK/x2"
expect_fail "clone --prune" "$CGC" --prune gh:a/b "$WORK/x3"
expect_fail "clone --force" "$CGC" --force gh:a/b "$WORK/x4"
expect_fail "clone --tags" "$CGC" --tags gh:a/b "$WORK/x5"
expect_fail "invalid origin" "$CGC" --origin 'bad..name' gh:a/b "$WORK/x6"
expect_fail "invalid branch" "$CGC" --branch 'bad..name' gh:a/b "$WORK/x7"
expect_fail "control char URL" "$CGC" $'https://example.com/a\nb' "$WORK/x8"
for d in 0 -1 abc 12x 999999999999999999999; do
    expect_fail "invalid depth $d" "$CGC" --depth "$d" gh:a/b "$WORK/depth-$d"
done
expect_fail "mirror depth" "$CGC" --mirror --depth 1 "$URL" "$WORK/mirrordepth"
missing "$WORK/mirrordepth"
mkdir -p "$WORK/occupied"; printf x > "$WORK/occupied/file"
expect_fail "non-empty destination" "$CGC" "$URL" "$WORK/occupied"
eq "$(cat "$WORK/occupied/file")" x "destination untouched"
expect_fail "missing token file" "$CGC" --token-file "$WORK/nope" gh:a/b "$WORK/tok1"
: > "$WORK/emptytok"
expect_fail "empty token file" "$CGC" --token-file "$WORK/emptytok" gh:a/b "$WORK/tok2"
printf 'valid-token\n' > "$WORK/goodtok"
"$CGC" --help >/dev/null 2>&1 # after token-free help
"$CGC" --token-file "$WORK/goodtok" --help >/dev/null 2>&1 && ok || bad "help with token file"

# ---------------------------------------------------------------------------
echo "== HTTP v2 clone matrix =="
# ---------------------------------------------------------------------------
"$CGC" -q "$URL" "$WORK/clone" || bad "default clone"
fsck "$WORK/clone"; clean_status "$WORK/clone"
eq "$(rev "$WORK/clone" HEAD)" "$(rev "$WORK/src" main)" "default HEAD"
eq "$(rev "$WORK/clone" refs/heads/dev)" "$(rev "$WORK/src" dev)" "clone dev"
eq "$(rev "$WORK/clone" refs/heads/feature/x)" "$(rev "$WORK/src" feature/x)" "clone feature/x"
eq "$(rev "$WORK/clone" refs/tags/v1)" "$(rev "$WORK/src" v1)" "clone tag v1"
eq "$(rev "$WORK/clone" refs/tags/light)" "$(rev "$WORK/src" light)" "clone light tag"
[[ -x "$WORK/clone/run.sh" ]] && ok || bad "executable bit"
[[ -L "$WORK/clone/link.txt" ]] && ok || bad "symlink"
exists "$WORK/clone/naïve.txt"; exists "$WORK/clone/file with space.txt"
nz "$(git -C "$WORK/clone" symbolic-ref -q refs/remotes/origin/HEAD)" "remote HEAD symref"

# --quiet emits nothing
qout=$("$CGC" -q "$URL" "$WORK/quiet" 2>&1); z "$qout" "--quiet output"

# chunked transfer
"$CGC" -q "$URL2" "$WORK/chunked" || bad "chunked clone"
fsck "$WORK/chunked"; clean_status "$WORK/chunked"

# --branch branch
"$CGC" -q --branch dev "$URL" "$WORK/bdev" || bad "--branch dev clone"
eq "$(git -C "$WORK/bdev" symbolic-ref HEAD)" refs/heads/dev "branch dev HEAD"
exists "$WORK/bdev/dev.txt"; fsck "$WORK/bdev"; clean_status "$WORK/bdev"

# --branch annotated tag -> detached at peeled commit
"$CGC" -q --branch v1 "$URL" "$WORK/btag" || bad "--branch v1 clone"
eq "$(rev "$WORK/btag" HEAD)" "$(git -C "$WORK/src" rev-parse 'v1^{commit}')" "tag detached HEAD"
z "$(git -C "$WORK/btag" symbolic-ref -q HEAD)" "tag detached"
fsck "$WORK/btag"; clean_status "$WORK/btag"

# --single-branch
"$CGC" -q --single-branch "$URL" "$WORK/single" || bad "--single-branch clone"
eq "$(git -C "$WORK/single" for-each-ref --format='%(refname)' refs/heads)" refs/heads/main "single branch refs"
missing "$WORK/single/.git/refs/remotes/origin/dev"
fsck "$WORK/single"; clean_status "$WORK/single"

# --depth implies single-branch
"$CGC" -q --depth 2 "$URL" "$WORK/depth2" || bad "--depth 2 clone"
exists "$WORK/depth2/.git/shallow"
eq "$(git -C "$WORK/depth2" rev-list --count HEAD)" 2 "depth count"
eq "$(git -C "$WORK/depth2" for-each-ref --format='%(refname)' refs/heads)" refs/heads/main "depth single refs"
fsck "$WORK/depth2"; clean_status "$WORK/depth2"

# --depth + --no-single-branch
"$CGC" -q --depth 2 --no-single-branch "$URL" "$WORK/depthall" || bad "depth all clone"
exists "$WORK/depthall/.git/shallow"
eq "$(git -C "$WORK/depthall" rev-list --count HEAD)" 2 "depth all count"
eq "$(rev "$WORK/depthall" refs/heads/dev)" "$(rev "$WORK/src" dev)" "depth all dev ref"
fsck "$WORK/depthall"

# --depth + --branch
"$CGC" -q --depth 1 --branch dev "$URL" "$WORK/depthdev" || bad "depth branch clone"
eq "$(git -C "$WORK/depthdev" symbolic-ref HEAD)" refs/heads/dev "depth branch HEAD"
eq "$(git -C "$WORK/depthdev" rev-list --count HEAD)" 1 "depth branch count"
fsck "$WORK/depthdev"; clean_status "$WORK/depthdev"

# --depth + an annotated tag: detached at the peeled commit with no dangling
# remote HEAD symref.
"$CGC" -q --depth 1 --branch v1 "$URL" "$WORK/depthtag" || bad "depth tag clone"
eq "$(rev "$WORK/depthtag" HEAD)" \
   "$(git -C "$WORK/src" rev-parse 'v1^{commit}')" "depth tag HEAD"
eq "$(git -C "$WORK/depthtag" rev-list --count HEAD)" 1 "depth tag count"
z "$(git -C "$WORK/depthtag" symbolic-ref -q HEAD)" "depth tag detached"
missing "$WORK/depthtag/.git/refs/remotes/origin/HEAD"
fsck "$WORK/depthtag"; clean_status "$WORK/depthtag"

# --bare / --no-checkout / --origin / --no-tags
"$CGC" -q --bare "$URL" "$WORK/bare.git" || bad "bare clone"
eq "$(git -C "$WORK/bare.git" rev-parse --is-bare-repository)" true "bare flag"
missing "$WORK/bare.git/.git"; fsck "$WORK/bare.git"
"$CGC" -q --no-checkout "$URL" "$WORK/nocheckout" || bad "no-checkout clone"
missing "$WORK/nocheckout/.git/index"; missing "$WORK/nocheckout/README.md"
eq "$(rev "$WORK/nocheckout" HEAD)" "$(rev "$WORK/src" main)" "no-checkout HEAD"
"$CGC" -q --origin upstream "$URL" "$WORK/origin" || bad "origin clone"
grepf "$WORK/origin/.git/config" '[remote "upstream"]'
eq "$(rev "$WORK/origin" refs/remotes/upstream/main)" "$(rev "$WORK/src" main)" "origin refs"
missing "$WORK/origin/.git/refs/remotes/origin"
"$CGC" -q --no-tags "$URL" "$WORK/notags" || bad "no-tags clone"
z "$(git -C "$WORK/notags" for-each-ref --format='%(refname)' refs/tags)" "no tags"
"$CGC" -q --no-tags --branch v1 "$URL" "$WORK/notagbranch" || bad "no-tags branch clone"
eq "$(rev "$WORK/notagbranch" HEAD)" "$(git -C "$WORK/src" rev-parse 'v1^{commit}')" "no-tags branch detached"

# --token must not be persisted
"$CGC" -q --token super-secret-token "$URL" "$WORK/tokclone" || bad "token clone"
if grep -rq "super-secret-token" "$WORK/tokclone/.git"; then bad "token persisted"; else ok; fi

# empty repo
eout=$("$CGC" "$EMPTY_URL" "$WORK/empty" 2>&1)
grepf <(printf '%s' "$eout") "empty repository"
exists "$WORK/empty/.git/HEAD"

# ---------------------------------------------------------------------------
echo "== fetch / pull / update semantics =="
# ---------------------------------------------------------------------------
cd "$WORK/clone"
git config user.email test@example.com
git config user.name Test
old=$(rev . HEAD)
printf 'ff\n' > "$WORK/src/ff.txt"
push_upstream "ff upstream"
new=$(rev "$WORK/src" main)
"$CGC" fetch -q || bad "fetch"
eq "$(rev . refs/remotes/origin/main)" "$new" "fetch tracking"
eq "$(rev . HEAD)" "$old" "fetch leaves HEAD"
missing ff.txt
grepf .git/FETCH_HEAD "not-for-merge	branch 'main'"
grepf .git/FETCH_HEAD "not-for-merge	tag 'v1'"
"$CGC" pull -q || bad "pull"
eq "$(rev . HEAD)" "$new" "pull HEAD"
exists ff.txt; fsck .; clean_status .
exists .git/ORIG_HEAD
eq "$(cat .git/ORIG_HEAD)" "$old" "ORIG_HEAD"
grepf .git/FETCH_HEAD "		branch 'main'"

# update alias
printf 'alias2\n' > "$WORK/src/alias2.txt"; push_upstream "alias2"
alias_head=$(rev "$WORK/src" main)
"$CGC" update -q || bad "update alias"
eq "$(rev . HEAD)" "$alias_head" "update alias HEAD"; exists alias2.txt

# dirty worktree refused
printf 'dirty\n' > "$WORK/src/ff.txt"; push_upstream "dirty"
dirty_head=$(rev "$WORK/src" main)
printf 'local\n' > ff.txt
if "$CGC" pull -q >/dev/null 2>&1; then bad "pull overwrote dirty file"; else ok; fi
eq "$(cat ff.txt)" local "dirty file preserved"
eq "$(rev . HEAD)" "$alias_head" "dirty HEAD unchanged"
git checkout -- ff.txt
"$CGC" pull -q || bad "pull after dirty recovery"
eq "$(rev . HEAD)" "$dirty_head" "dirty recovery HEAD"

# untracked collision refused
printf 'collision\n' > "$WORK/src/collide.txt"; push_upstream "collide"
collide_head=$(rev "$WORK/src" main)
printf 'mine\n' > collide.txt
if "$CGC" pull -q >/dev/null 2>&1; then bad "pull overwrote untracked"; else ok; fi
eq "$(cat collide.txt)" mine "untracked preserved"
eq "$(rev . HEAD)" "$dirty_head" "untracked HEAD unchanged"
rm collide.txt
"$CGC" pull -q || bad "pull after collision recovery"
exists collide.txt
eq "$(rev . HEAD)" "$collide_head" "collision recovery HEAD"

# staged unrelated survives; overlapping refused
printf 'staged\n' >> run.sh; git add run.sh
printf 'unrelated\n' > "$WORK/src/unrelated.txt"; push_upstream "unrelated"
"$CGC" pull -q || bad "pull with unrelated staged"
eq "$(git status --porcelain)" "M  run.sh" "unrelated staged preserved"
git reset -q HEAD run.sh; git checkout -- run.sh
printf 'overlap\n' >> README.md; git add README.md
printf 'overlap-up\n' > "$WORK/src/README.md"; push_upstream "overlap"
overlap_head=$(rev "$WORK/src" main)
before=$(rev . HEAD)
if "$CGC" pull -q >/dev/null 2>&1; then bad "pull accepted overlapping staged"; else ok; fi
eq "$(rev . HEAD)" "$before" "overlap HEAD unchanged"
git reset -q HEAD README.md; git checkout -- README.md
"$CGC" pull -q || bad "pull after overlap recovery"
eq "$(cat README.md)" overlap-up "overlap recovery content"

# non-fast-forward refused
printf 'local divergent\n' > local-diverge.txt
git add local-diverge.txt; git commit -q -m "local divergent"
printf 'divergent\n' > "$WORK/src/divergent.txt"; push_upstream "divergent"
div_head=$(rev . HEAD)
if "$CGC" pull -q >/dev/null 2>&1; then bad "pull accepted non-ff"; else ok; fi
eq "$(rev . HEAD)" "$div_head" "non-ff HEAD unchanged"
missing divergent.txt
cd "$WORK"

# fetch --branch limits refs
"$CGC" -q "$URL" "$WORK/fbranch" || bad "fbranch clone"
git -C "$WORK/src" checkout -q main
git -C "$WORK/src" branch devmove main
printf 'dev moved\n' > "$WORK/src/devmoved.txt"
git -C "$WORK/src" checkout -q devmove; git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m "dev moved"
git -C "$WORK/src" checkout -q main
git -C "$WORK/server/repo.git" fetch -q "$WORK/src" '+refs/heads/*:refs/heads/*'
(cd "$WORK/fbranch" && "$CGC" fetch -q --branch devmove) || bad "fetch --branch"
eq "$(rev "$WORK/fbranch" refs/remotes/origin/devmove)" "$(rev "$WORK/src" devmove)" "fetch branch devmove"

# pull --branch from detached HEAD creates and switches to a new local branch
git -C "$WORK/fbranch" checkout -q --detach HEAD
(cd "$WORK/fbranch" && "$CGC" pull -q --branch devmove) || bad "detached pull --branch"
eq "$(git -C "$WORK/fbranch" symbolic-ref HEAD)" refs/heads/devmove "detached pull HEAD"
exists "$WORK/fbranch/devmoved.txt"; clean_status "$WORK/fbranch"

# pull refuses to switch away from an existing branch
git -C "$WORK/fbranch" checkout -q main
if (cd "$WORK/fbranch" && "$CGC" pull -q --branch devmove >/dev/null 2>&1); then
    bad "pull switched branches"; else ok; fi
eq "$(git -C "$WORK/fbranch" symbolic-ref HEAD)" refs/heads/main "branch not switched"

# pull with detached HEAD and no --branch refuses
git -C "$WORK/fbranch" checkout -q --detach HEAD
if (cd "$WORK/fbranch" && "$CGC" pull -q >/dev/null 2>&1); then
    bad "detached pull without --branch"; else ok; fi

# shallow fetch refusal
if (cd "$WORK/depth2" && "$CGC" fetch -q >/dev/null 2>&1); then
    bad "fetch into shallow clone"; else ok; fi

# no remote / not a repo
mkdir -p "$WORK/noremote"; git -C "$WORK/noremote" init -q
if (cd "$WORK/noremote" && "$CGC" fetch -q >/dev/null 2>&1); then
    bad "fetch with no remote"; else ok; fi
expect_fail "fetch outside repo" "$CGC" fetch -q "$WORK/notarepo"

# invalid fetch refspec
"$CGC" -q "$URL" "$WORK/badspec" || bad "badspec clone"
git -C "$WORK/badspec" config remote.origin.fetch 'not-a-refspec'
if (cd "$WORK/badspec" && "$CGC" fetch -q >/dev/null 2>&1); then
    bad "invalid refspec accepted"; else ok; fi

# ---------------------------------------------------------------------------
echo "== refspecs, multiple remotes, and branch.<name>.merge =="
# ---------------------------------------------------------------------------
"$CGC" -q "$URL" "$WORK/refspec" || bad "refspec clone"
printf 'custom\n' > "$WORK/src/custom.txt"; push_upstream "custom"
custom_head=$(rev "$WORK/src" main)
git -C "$WORK/refspec" config --replace-all remote.origin.fetch '+refs/heads/*:refs/custom/*'
git -C "$WORK/refspec" config --add remote.origin.fetch '^refs/heads/dev'
(cd "$WORK/refspec" && "$CGC" fetch -q) || bad "custom refspec fetch"
eq "$(rev "$WORK/refspec" refs/custom/main)" "$custom_head" "custom main"
missing "$WORK/refspec/.git/refs/custom/dev"
git -C "$WORK/refspec" remote add backup "$URL"
(cd "$WORK/refspec" && "$CGC" fetch -q --remote backup) || bad "backup fetch"
eq "$(rev "$WORK/refspec" refs/remotes/backup/main)" "$custom_head" "backup main"
if (cd "$WORK/refspec" && "$CGC" fetch -q --remote missing >/dev/null 2>&1); then
    bad "unknown remote accepted"; else ok; fi

# A refspec outside refs/heads and refs/tags makes the client advertise and
# fetch that namespace (notes are the common real-world case).
"$CGC" -q "$URL" "$WORK/notesref" || bad "notesref clone"
git -C "$WORK/notesref" config --replace-all remote.origin.fetch \
    '+refs/notes/*:refs/notes/*'
(cd "$WORK/notesref" && "$CGC" fetch -q) || bad "notes refspec fetch"
eq "$(rev "$WORK/notesref" refs/notes/commits)" \
   "$(rev "$WORK/src" refs/notes/commits)" "notes refspec"

# branch.<name>.merge selects the pull source
"$CGC" -q "$URL" "$WORK/mergecfg" || bad "mergecfg clone"
git -C "$WORK/src" checkout -q main
git -C "$WORK/src" checkout -q -b mergesrc
printf 'merge src\n' > "$WORK/src/mergesrc.txt"
git -C "$WORK/src" add -A; git -C "$WORK/src" commit -q -m "merge source"
git -C "$WORK/src" checkout -q main
git -C "$WORK/server/repo.git" fetch -q "$WORK/src" '+refs/heads/*:refs/heads/*'
git -C "$WORK/mergecfg" config branch.main.merge refs/heads/mergesrc
(cd "$WORK/mergecfg" && "$CGC" pull -q) || bad "pull branch.<name>.merge"
eq "$(rev "$WORK/mergecfg" HEAD)" "$(rev "$WORK/src" mergesrc)" "merge source HEAD"
exists "$WORK/mergecfg/mergesrc.txt"; clean_status "$WORK/mergecfg"

# bare repo with a configured refspec updates refs
"$CGC" -q --bare "$URL" "$WORK/barefetch.git" || bad "barefetch clone"
git -C "$WORK/barefetch.git" config remote.origin.fetch '+refs/heads/*:refs/heads/*'
printf 'bare refspec\n' > "$WORK/src/bareref.txt"; push_upstream "bare refspec"
bare_head=$(rev "$WORK/src" main)
(cd "$WORK/barefetch.git" && "$CGC" fetch -q) || bad "bare refspec fetch"
eq "$(rev "$WORK/barefetch.git" refs/heads/main)" "$bare_head" "bare refspec main"
missing "$WORK/barefetch.git/.git"

# bare no-refspec fetch still records FETCH_HEAD only
"$CGC" -q --bare "$URL" "$WORK/bareplain.git" || bad "bareplain clone"
plain_head=$(rev "$WORK/bareplain.git" refs/heads/main)
printf 'bare plain\n' > "$WORK/src/bareplain.txt"; push_upstream "bare plain"
new_plain=$(rev "$WORK/src" main)
(cd "$WORK/bareplain.git" && "$CGC" fetch -q) || bad "bare plain fetch"
eq "$(rev "$WORK/bareplain.git" refs/heads/main)" "$plain_head" "bare plain refs untouched"
grepf "$WORK/bareplain.git/FETCH_HEAD" "$new_plain"

# prune loose + packed, preserving origin/HEAD, skipped with --branch
"$CGC" -q "$URL" "$WORK/prune" || bad "prune clone"
git -C "$WORK/src" branch prunetmp main
git -C "$WORK/server/repo.git" fetch -q "$WORK/src" '+refs/heads/*:refs/heads/*'
(cd "$WORK/prune" && "$CGC" fetch -q) || bad "prune initial fetch"
exists "$WORK/prune/.git/refs/remotes/origin/prunetmp"
git -C "$WORK/server/repo.git" update-ref -d refs/heads/prunetmp
(cd "$WORK/prune" && "$CGC" fetch -q --prune --branch main) || bad "prune branch fetch"
exists "$WORK/prune/.git/refs/remotes/origin/prunetmp" # skipped under --branch
(cd "$WORK/prune" && "$CGC" fetch -q --prune) || bad "prune fetch"
missing "$WORK/prune/.git/refs/remotes/origin/prunetmp"
eq "$(git -C "$WORK/prune" symbolic-ref refs/remotes/origin/HEAD)" refs/remotes/origin/main "origin HEAD survives prune"

# tag clobber protection and --force
"$CGC" -q "$URL" "$WORK/tagforce" || bad "tagforce clone"
git -C "$WORK/src" tag -f -a v1 -m "moved v1"
git -C "$WORK/src" push -q -f "$WORK/server/repo.git" refs/tags/v1
if (cd "$WORK/tagforce" && "$CGC" fetch -q >/dev/null 2>&1); then bad "tag clobber accepted"; else ok; fi
if (cd "$WORK/tagforce" && "$CGC" fetch -q --force) ; then ok; else bad "tag --force failed"; fi
eq "$(rev "$WORK/tagforce" refs/tags/v1)" "$(rev "$WORK/src" v1)" "tag forced"

# ---------------------------------------------------------------------------
echo "== mirror =="
# ---------------------------------------------------------------------------
"$CGC" -q --mirror "$URL" "$WORK/mirror.git" || bad "mirror clone"
eq "$(git -C "$WORK/mirror.git" rev-parse --is-bare-repository)" true "mirror bare"
missing "$WORK/mirror.git/.git"
eq "$(rev "$WORK/mirror.git" refs/heads/main)" "$(rev "$WORK/src" main)" "mirror main"
eq "$(rev "$WORK/mirror.git" refs/heads/dev)" "$(rev "$WORK/src" dev)" "mirror dev"
eq "$(rev "$WORK/mirror.git" refs/heads/feature/x)" "$(rev "$WORK/src" feature/x)" "mirror feature"
eq "$(rev "$WORK/mirror.git" refs/tags/v1)" "$(rev "$WORK/src" v1)" "mirror tag"
z "$(git -C "$WORK/mirror.git" for-each-ref --format='%(refname)' refs/remotes)" "mirror no remotes"
grepf "$WORK/mirror.git/config" 'fetch = +refs/*:refs/*'
grepf "$WORK/mirror.git/config" 'mirror = true'
fsck "$WORK/mirror.git"
if git ls-remote "$URL" | grep -q refs/notes; then
    eq "$(rev "$WORK/mirror.git" refs/notes/commits)" "$(rev "$WORK/src" refs/notes/commits)" "mirror notes"
fi

# mirror fetch updates refs and force-updates moved tags
printf 'mirror update\n' > "$WORK/src/mirrorup.txt"; push_upstream "mirror update"
mirror_head=$(rev "$WORK/src" main)
(cd "$WORK/mirror.git" && "$CGC" fetch -q) || bad "mirror fetch"
eq "$(rev "$WORK/mirror.git" refs/heads/main)" "$mirror_head" "mirror updated main"
eq "$(rev "$WORK/mirror.git" HEAD)" "$mirror_head" "mirror updated HEAD"
git -C "$WORK/src" tag -f -a v1 -m "mirror moved tag"
git -C "$WORK/src" push -q -f "$WORK/server/repo.git" refs/tags/v1
(cd "$WORK/mirror.git" && "$CGC" fetch -q) || bad "mirror tag force"
eq "$(rev "$WORK/mirror.git" refs/tags/v1)" "$(rev "$WORK/src" v1)" "mirror tag forced"

# Mirror fetches also update non-branch, non-tag refs such as notes.
git -C "$WORK/src" notes add -m "mirror note" main
git -C "$WORK/src" push -q "$WORK/server/repo.git" refs/notes/commits
(cd "$WORK/mirror.git" && "$CGC" fetch -q) || bad "mirror notes fetch"
eq "$(rev "$WORK/mirror.git" refs/notes/commits)" \
   "$(rev "$WORK/src" refs/notes/commits)" "mirror notes update"

# mirror prunes deleted branches
git -C "$WORK/src" branch mtemp main
git -C "$WORK/server/repo.git" fetch -q "$WORK/src" '+refs/heads/*:refs/heads/*'
(cd "$WORK/mirror.git" && "$CGC" fetch -q) || bad "mirror mtemp fetch"
exists "$WORK/mirror.git/refs/heads/mtemp"
git -C "$WORK/server/repo.git" update-ref -d refs/heads/mtemp
git -C "$WORK/src" branch -D mtemp >/dev/null
(cd "$WORK/mirror.git" && "$CGC" fetch -q) || bad "mirror prune fetch"
missing "$WORK/mirror.git/refs/heads/mtemp"

# mirror --branch sets HEAD but keeps every ref
"$CGC" -q --mirror --branch dev "$URL" "$WORK/mirrorbranch.git" || bad "mirror branch clone"
eq "$(git -C "$WORK/mirrorbranch.git" symbolic-ref HEAD)" refs/heads/dev "mirror branch HEAD"
eq "$(rev "$WORK/mirrorbranch.git" refs/heads/main)" "$(rev "$WORK/src" main)" "mirror branch keeps main"

# mirror ignores --no-tags and --single-branch
"$CGC" -q --mirror --no-tags --single-branch "$URL" "$WORK/mirrorflags.git" || bad "mirror flags clone"
eq "$(rev "$WORK/mirrorflags.git" refs/tags/v1)" "$(rev "$WORK/src" v1)" "mirror ignores no-tags"
eq "$(rev "$WORK/mirrorflags.git" refs/heads/dev)" "$(rev "$WORK/src" dev)" "mirror ignores single-branch"

# ---------------------------------------------------------------------------
echo "== local transport =="
# ---------------------------------------------------------------------------
git -C "$WORK/src" checkout -q main
git -C "$WORK/src" gc -q
git -C "$WORK/src" pack-refs --all
"$CGC" -q "$WORK/src" "$WORK/local" || bad "local clone"
fsck "$WORK/local"; clean_status "$WORK/local"
eq "$(rev "$WORK/local" HEAD)" "$(rev "$WORK/src" main)" "local HEAD"
eq "$(rev "$WORK/local" refs/remotes/origin/dev)" "$(rev "$WORK/src" dev)" "local dev"
"$CGC" -q "file://$WORK/src" "$WORK/localfile" || bad "file clone"
fsck "$WORK/localfile"; clean_status "$WORK/localfile"
"$CGC" -q "$WORK/server/repo.git" "$WORK/localbare" || bad "local bare source clone"
fsck "$WORK/localbare"; clean_status "$WORK/localbare"
"$CGC" -q --branch v1 "$WORK/src" "$WORK/localtag" || bad "local tag clone"
eq "$(rev "$WORK/localtag" HEAD)" "$(git -C "$WORK/src" rev-parse 'v1^{commit}')" "local tag detached"
fsck "$WORK/localtag"
"$CGC" -q --bare "$WORK/src" "$WORK/local.git" || bad "local bare clone"
eq "$(git -C "$WORK/local.git" rev-parse --is-bare-repository)" true "local bare flag"
"$CGC" -q --no-tags "$WORK/src" "$WORK/localnotags" || bad "local no-tags"
z "$(git -C "$WORK/localnotags" for-each-ref --format='%(refname)' refs/tags)" "local no tags"
"$CGC" -q --origin upstream "$WORK/src" "$WORK/localorigin" || bad "local origin"
eq "$(rev "$WORK/localorigin" refs/remotes/upstream/main)" "$(rev "$WORK/src" main)" "local origin refs"
"$CGC" -q --mirror "$WORK/src" "$WORK/localmirror.git" || bad "local mirror"
eq "$(rev "$WORK/localmirror.git" refs/heads/dev)" "$(rev "$WORK/src" dev)" "local mirror dev"
eq "$(rev "$WORK/localmirror.git" refs/notes/commits)" \
   "$(rev "$WORK/src" refs/notes/commits)" "local mirror notes"
z "$(git -C "$WORK/localmirror.git" for-each-ref --format='%(refname)' refs/remotes)" "local mirror no remotes"

# Local fetch honours a notes refspec too.
"$CGC" -q "$WORK/src" "$WORK/localnotes" || bad "localnotes clone"
git -C "$WORK/localnotes" config --replace-all remote.origin.fetch \
    '+refs/notes/*:refs/notes/*'
(cd "$WORK/localnotes" && "$CGC" fetch -q) || bad "local notes fetch"
eq "$(rev "$WORK/localnotes" refs/notes/commits)" \
   "$(rev "$WORK/src" refs/notes/commits)" "local notes refspec"

# local linked worktree
git -C "$WORK/src" worktree add -q "$WORK/linked" dev
"$CGC" -q "$WORK/linked" "$WORK/linkedclone" || bad "linked clone"
fsck "$WORK/linkedclone"; clean_status "$WORK/linkedclone"
eq "$(git -C "$WORK/linkedclone" symbolic-ref HEAD)" refs/heads/dev "linked HEAD"
git -C "$WORK/src" worktree remove "$WORK/linked"

# local fetch/pull
printf 'local pull 2\n' > "$WORK/src/localpull2.txt"
git -C "$WORK/src" add -A; git -C "$WORK/src" commit -q -m "local pull 2"
local_head=$(rev "$WORK/src" main)
(cd "$WORK/local" && "$CGC" pull -q) || bad "local pull"
eq "$(rev "$WORK/local" HEAD)" "$local_head" "local pull HEAD"
exists "$WORK/local/localpull2.txt"; fsck "$WORK/local"; clean_status "$WORK/local"

# local mirror fetch prunes
git -C "$WORK/src" branch lmtemp main
(cd "$WORK/localmirror.git" && "$CGC" fetch -q) || bad "local mirror fetch"
exists "$WORK/localmirror.git/refs/heads/lmtemp"
git -C "$WORK/src" branch -D lmtemp >/dev/null
(cd "$WORK/localmirror.git" && "$CGC" fetch -q) || bad "local mirror prune"
missing "$WORK/localmirror.git/refs/heads/lmtemp"

# local empty repo
git init -q --bare "$WORK/localempty.git"
git -C "$WORK/localempty.git" symbolic-ref HEAD refs/heads/main
leout=$("$CGC" "$WORK/localempty.git" "$WORK/localemptyclone" 2>&1)
grepf <(printf '%s' "$leout") "empty repository"
exists "$WORK/localemptyclone/.git/HEAD"

# local --depth rejected before creating
expect_fail "local depth" "$CGC" --depth 1 "$WORK/src" "$WORK/localdepth"
missing "$WORK/localdepth"

# ---------------------------------------------------------------------------
echo "== protocol v0 =="
# ---------------------------------------------------------------------------
"$CGC" -q "$URL3" "$WORK/v0" || bad "v0 clone"
fsck "$WORK/v0"; clean_status "$WORK/v0"
eq "$(rev "$WORK/v0" HEAD)" "$(rev "$WORK/server/repo.git" refs/heads/main)" "v0 HEAD"
eq "$(rev "$WORK/v0" refs/tags/v1)" "$(rev "$WORK/src" v1)" "v0 tag"
"$CGC" -q --depth 1 "$URL3" "$WORK/v0depth" || bad "v0 depth"
exists "$WORK/v0depth/.git/shallow"
eq "$(git -C "$WORK/v0depth" rev-list --count HEAD)" 1 "v0 depth count"
fsck "$WORK/v0depth"
printf 'v0 pull 2\n' > "$WORK/src/v0pull2.txt"; push_upstream "v0 pull 2"
v0_head=$(rev "$WORK/src" main)
(cd "$WORK/v0" && "$CGC" fetch -q) || bad "v0 fetch"
eq "$(rev "$WORK/v0" refs/remotes/origin/main)" "$v0_head" "v0 fetch ref"
(cd "$WORK/v0" && "$CGC" pull -q) || bad "v0 pull"
eq "$(rev "$WORK/v0" HEAD)" "$v0_head" "v0 pull HEAD"
exists "$WORK/v0/v0pull2.txt"; fsck "$WORK/v0"; clean_status "$WORK/v0"

# Protocol v0 also honours a refspec outside heads/tags.
"$CGC" -q "$URL3" "$WORK/v0notes" || bad "v0notes clone"
git -C "$WORK/v0notes" config --replace-all remote.origin.fetch \
    '+refs/notes/*:refs/notes/*'
(cd "$WORK/v0notes" && "$CGC" fetch -q) || bad "v0 notes fetch"
eq "$(rev "$WORK/v0notes" refs/notes/commits)" \
   "$(rev "$WORK/src" refs/notes/commits)" "v0 notes refspec"

eout=$("$CGC" "$EMPTY_URL3" "$WORK/v0empty" 2>&1)
grepf <(printf '%s' "$eout") "empty repository"

# ---------------------------------------------------------------------------
echo "== cleanliness =="
# ---------------------------------------------------------------------------
leftovers=$(find "$WORK" -path '*/.git/*' \( -name '*.lock' -o -name '.cgc-tmp.*' \) 2>/dev/null)
z "$leftovers" "no lock or temp files"

# ---------------------------------------------------------------------------
echo
echo "features: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
