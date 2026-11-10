#!/usr/bin/env bash
# Network behavior tests: HTTP timeouts, retries, richer error messages, and
# proxy support. Uses only local servers, so no internet access is required.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

cd "$ROOT"
make >/dev/null

mkdir -p "$WORK/server" "$WORK/src"
git -C "$WORK/src" init -q -b main
git -C "$WORK/src" config user.email test@example.com
git -C "$WORK/src" config user.name Test
printf 'network tests\n' > "$WORK/src/README.md"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m initial
git clone -q --bare "$WORK/src" "$WORK/server/repo.git"
git -C "$WORK/server/repo.git" symbolic-ref HEAD refs/heads/main

start_server() {
    local port=$1 mode=${2:-length} fails=${3:-1}
    FLAKY_FAILS=$fails python3 "$ROOT/tests/serve-http.py" \
        "$WORK/server" "$port" "$mode" >/dev/null 2>&1 &
    PIDS+=($!)
}

start_proxy() {
    local port=$1
    PROXY_AUTH=user:pass PROXY_LOG="$WORK/proxy.log" \
        python3 "$ROOT/tests/proxy.py" "$port" >/dev/null 2>&1 &
    PIDS+=($!)
}

wait_port() {
    local port=$1
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
            exec 3>&-
            return 0
        fi
        sleep 0.05
    done
    return 1
}

# Run cgc with a clean proxy environment, then add the given variables.
run_cgc() {
    env -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY \
        -u all_proxy -u ALL_PROXY -u no_proxy -u NO_PROXY "$@"
}

PORT=$((11000 + RANDOM % 400))
FLAKY_PORT=$((PORT + 400))
FLAKY_FAIL_PORT=$((PORT + 800))
STALL_PORT=$((PORT + 1200))
PROXY_PORT=$((PORT + 1600))

start_server "$PORT" length
start_server "$FLAKY_PORT" flaky 1
start_server "$FLAKY_FAIL_PORT" flaky 1
start_server "$STALL_PORT" stall
start_proxy "$PROXY_PORT"

for port in "$PORT" "$FLAKY_PORT" "$FLAKY_FAIL_PORT" "$STALL_PORT" \
            "$PROXY_PORT"; do
    wait_port "$port" || { echo "network: port $port did not open" >&2; exit 1; }
done

BASE="http://127.0.0.1:$PORT/repo.git"

# Baseline: direct clone.
run_cgc "$ROOT/cgc" -q "$BASE" "$WORK/direct"
git -C "$WORK/direct" fsck --full

# A stalled server must time out instead of hanging forever.
start=$(date +%s)
if run_cgc CGC_HTTP_TIMEOUT=1 CGC_HTTP_RETRIES=0 \
        "$ROOT/cgc" -q "http://127.0.0.1:$STALL_PORT/repo.git" \
        "$WORK/timeout" 2>"$WORK/timeout.err"; then
    echo "network: stalled server did not fail" >&2
    exit 1
fi
elapsed=$(( $(date +%s) - start ))
if [[ "$elapsed" -gt 10 ]]; then
    echo "network: timeout took ${elapsed}s (expected a few seconds)" >&2
    exit 1
fi
grep -qi 'timed out' "$WORK/timeout.err"

# A transient 503 is retried, and the final failure reports status, hint, and
# the server's message.
run_cgc CGC_HTTP_RETRIES=1 \
    "$ROOT/cgc" -q "http://127.0.0.1:$FLAKY_PORT/repo.git" "$WORK/retried"
git -C "$WORK/retried" fsck --full

if run_cgc CGC_HTTP_RETRIES=0 \
        "$ROOT/cgc" -q "http://127.0.0.1:$FLAKY_FAIL_PORT/repo.git" \
        "$WORK/no-retry" 2>"$WORK/503.err"; then
    echo "network: 503 was not reported as a failure" >&2
    exit 1
fi
grep -q 'HTTP 503' "$WORK/503.err"
grep -qi 'temporary problem' "$WORK/503.err"
grep -qi 'temporary failure' "$WORK/503.err"

# An HTTP proxy is used for absolute-form requests, and credentials are sent.
if run_cgc http_proxy="http://127.0.0.1:$PROXY_PORT" \
        "$ROOT/cgc" -q "$BASE" "$WORK/proxy-noauth" 2>"$WORK/407.err"; then
    echo "network: proxy accepted a request without credentials" >&2
    exit 1
fi
grep -q 'HTTP 407' "$WORK/407.err"
grep -qi 'proxy authentication required' "$WORK/407.err"

run_cgc http_proxy="http://user:pass@127.0.0.1:$PROXY_PORT" \
    "$ROOT/cgc" -q "$BASE" "$WORK/proxy"
git -C "$WORK/proxy" fsck --full
test -s "$WORK/proxy.log"
grep -q "GET http://127.0.0.1:$PORT/repo.git/" "$WORK/proxy.log"

# ALL_PROXY is used as a fallback.
run_cgc all_proxy="http://user:pass@127.0.0.1:$PROXY_PORT" \
    "$ROOT/cgc" -q "$BASE" "$WORK/allproxy"
git -C "$WORK/allproxy" fsck --full

# NO_PROXY bypasses the proxy entirely, including host:port entries.
run_cgc http_proxy="http://127.0.0.1:$PROXY_PORT" NO_PROXY=127.0.0.1 \
    "$ROOT/cgc" -q "$BASE" "$WORK/noproxy"
git -C "$WORK/noproxy" fsck --full

run_cgc http_proxy="http://127.0.0.1:$PROXY_PORT" \
    no_proxy="example.com, 127.0.0.1:$PORT" \
    "$ROOT/cgc" -q "$BASE" "$WORK/noproxy-port"
git -C "$WORK/noproxy-port" fsck --full

echo "network test passed"
