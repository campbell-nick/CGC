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
    local port=$1 mode=${2:-length} fails=${3:-1} skip=${4:-0}
    FLAKY_FAILS=$fails FLAKY_SKIP=$skip \
        python3 "$ROOT/tests/serve-http.py" \
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
        -u all_proxy -u ALL_PROXY -u no_proxy -u NO_PROXY \
        -u CGC_HTTP_TIMEOUT -u CGC_HTTP_RETRIES "$@"
}

PORT=$((11000 + RANDOM % 400))
FLAKY_PORT=$((PORT + 400))
FLAKY_FAIL_PORT=$((PORT + 800))
STALL_PORT=$((PORT + 1200))
PROXY_PORT=$((PORT + 1600))
TRUNC_PORT=$((PORT + 2800))
TRUNC_PULL_PORT=$((PORT + 3200))
TRUNC_OFF_PORT=$((PORT + 3600))

start_server "$PORT" length
start_server "$FLAKY_PORT" flaky 1
start_server "$FLAKY_FAIL_PORT" flaky 1
start_server "$STALL_PORT" stall
start_server "$TRUNC_PORT" truncate-post 1 0
start_server "$TRUNC_PULL_PORT" truncate-post 1 1
start_server "$TRUNC_OFF_PORT" truncate-post 1 0
start_proxy "$PROXY_PORT"

for port in "$PORT" "$FLAKY_PORT" "$FLAKY_FAIL_PORT" "$STALL_PORT" \
            "$PROXY_PORT" "$TRUNC_PORT" "$TRUNC_PULL_PORT" \
            "$TRUNC_OFF_PORT"; do
    wait_port "$port" || { echo "network: port $port did not open" >&2; exit 1; }
done

BASE="http://127.0.0.1:$PORT/repo.git"

# Baseline: direct clone.
run_cgc "$ROOT/cgc" -q "$BASE" "$WORK/direct"
git -C "$WORK/direct" fsck --full

# Connection failures happen before a request is sent, so they are retried.
# Start the server after the first few attempts have already been refused.
LATE_PORT=$((PORT + 2000))
(
    sleep 1.2
    exec env FLAKY_FAILS=1 python3 "$ROOT/tests/serve-http.py" \
        "$WORK/server" "$LATE_PORT" length >/dev/null 2>&1
) &
PIDS+=($!)
if ! run_cgc "$ROOT/cgc" -q "http://127.0.0.1:$LATE_PORT/repo.git" \
        "$WORK/late" 2>"$WORK/late.err"; then
    cat "$WORK/late.err" >&2
    exit 1
fi
git -C "$WORK/late" fsck --full
if [ -s "$WORK/late.err" ]; then
    echo "network: retries printed warnings after succeeding" >&2
    cat "$WORK/late.err" >&2
    exit 1
fi

# Once retries are exhausted, the connection error is reported exactly once.
CLOSED_PORT=$((PORT + 2400))
if run_cgc CGC_HTTP_RETRIES=2 "$ROOT/cgc" -q \
        "http://127.0.0.1:$CLOSED_PORT/repo.git" "$WORK/closed" \
        2>"$WORK/closed.err"; then
    echo "network: closed port unexpectedly succeeded" >&2
    exit 1
fi
if [ "$(grep -c 'cannot connect' "$WORK/closed.err" || true)" -ne 1 ]; then
    echo "network: expected one connection warning" >&2
    cat "$WORK/closed.err" >&2
    exit 1
fi

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

# A failure after the request was sent cannot be retried inside the HTTP
# layer, so clone retries the whole ref-listing operation.
run_cgc "$ROOT/cgc" -q "http://127.0.0.1:$TRUNC_PORT/repo.git" \
    "$WORK/truncated"
git -C "$WORK/truncated" fsck --full

# Pull retries a truncated pack transfer. Skip the ref-listing POST so the
# failure happens during the fetch itself.
run_cgc "$ROOT/cgc" -q "$BASE" "$WORK/pull-retry"
printf 'second commit\n' > "$WORK/src/second.txt"
git -C "$WORK/src" add -A
git -C "$WORK/src" commit -q -m second
git -C "$WORK/src" push -q "$WORK/server/repo.git" main
git -C "$WORK/pull-retry" remote set-url origin \
    "http://127.0.0.1:$TRUNC_PULL_PORT/repo.git"
run_cgc "$ROOT/cgc" pull -q "$WORK/pull-retry"
test -f "$WORK/pull-retry/second.txt"
git -C "$WORK/pull-retry" fsck --full

# CGC_HTTP_RETRIES=0 disables whole-operation retries as well.
if run_cgc CGC_HTTP_RETRIES=0 "$ROOT/cgc" -q \
        "http://127.0.0.1:$TRUNC_OFF_PORT/repo.git" "$WORK/trunc-off" \
        2>"$WORK/trunc-off.err"; then
    echo "network: truncated response was not reported as a failure" >&2
    exit 1
fi
grep -qi 'truncated\|malformed response' "$WORK/trunc-off.err"

echo "network test passed"
