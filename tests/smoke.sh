#!/usr/bin/env bash
# Demo smoke: backends A and B plus a proxy serving public/, checked with curl.
# Usage: tests/smoke.sh [path/to/mini-webserver]   (default build/mini-webserver)
set -u

BIN=${1:-build/mini-webserver}
ROOT=public
TMP=$(mktemp -d)
PIDS=()
FAILED=0

cleanup() {
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do
        kill "$pid" 2>/dev/null
    done
    for pid in ${PIDS[@]+"${PIDS[@]}"}; do
        wait "$pid" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; FAILED=$((FAILED + 1)); }

free_port() {
    python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'
}

wait_ready() {
    for _ in $(seq 1 50); do
        if curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$1/"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

if [ ! -x "$BIN" ]; then
    make >/dev/null || { echo "build failed"; exit 1; }
fi

PA=$(free_port)
PB=$(free_port)
PP=$(free_port)
while [ "$PB" = "$PA" ]; do PB=$(free_port); done
while [ "$PP" = "$PA" ] || [ "$PP" = "$PB" ]; do PP=$(free_port); done

"$BIN" --host 127.0.0.1 --port "$PA" --root "$ROOT" --name A >"$TMP/a.log" 2>&1 &
PIDS+=($!)
"$BIN" --host 127.0.0.1 --port "$PB" --root "$ROOT" --name B >"$TMP/b.log" 2>&1 &
PIDS+=($!)
"$BIN" --host 127.0.0.1 --port "$PP" --root "$ROOT" --name proxy \
    --route "/api=127.0.0.1:$PA,127.0.0.1:$PB" --forward-allow 127.0.0.1/32 \
    >"$TMP/proxy.log" 2>&1 &
PIDS+=($!)

for port in "$PA" "$PB" "$PP"; do
    wait_ready "$port" || { echo "server on port $port did not start"; exit 1; }
done
echo "backends A=$PA B=$PB, proxy=$PP"

BASE="http://127.0.0.1:$PP"

check_type() {
    local path=$1 expected=$2 actual
    actual=$(curl -s -o /dev/null -w '%{content_type}' "$BASE$path")
    if [ "$actual" = "$expected" ]; then
        pass "Content-Type $path = $expected"
    else
        fail "Content-Type $path: expected '$expected', got '$actual'"
    fi
}

check_type / "text/html; charset=utf-8"
check_type /index.html "text/html; charset=utf-8"
check_type /style.css "text/css; charset=utf-8"
check_type /app.js "text/javascript; charset=utf-8"
check_type /logo.png "image/png"

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/missing.html")
if [ "$code" = "404" ]; then pass "GET /missing.html = 404"; else fail "GET /missing.html: expected 404, got $code"; fi

names=()
for _ in 1 2 3 4; do
    body=$(curl -s "$BASE/api/whoami")
    name=${body#*\"name\":\"}
    names+=("${name%%\"*}")
done
if [ "${names[0]}" != "${names[1]}" ] && [ "${names[0]}" = "${names[2]}" ] &&
    [ "${names[1]}" = "${names[3]}" ] &&
    { [ "${names[0]}" = A ] || [ "${names[0]}" = B ]; } &&
    { [ "${names[1]}" = A ] || [ "${names[1]}" = B ]; }; then
    pass "whoami alternates ${names[*]}"
else
    fail "whoami should alternate A/B, got ${names[*]}"
fi

printf '안녕하세요, 미니 웹서버! 한글 UTF-8 😀\n' >"$TMP/sent.txt"
curl -s --data-binary @"$TMP/sent.txt" -H 'Content-Type: text/plain; charset=utf-8' \
    -o "$TMP/echoed.txt" "$BASE/api/echo"
if cmp -s "$TMP/sent.txt" "$TMP/echoed.txt"; then
    pass "POST /api/echo returns UTF-8 body byte-exact"
else
    fail "POST /api/echo body differs from what was sent"
fi

# Forward proxy: curl sends absolute-form, the proxy goes straight to B (no route).
body=$(curl -s -x "$BASE" "http://127.0.0.1:$PB/api/whoami")
if [ "$body" = "{\"name\":\"B\",\"port\":$PB}" ]; then
    pass "curl -x reaches backend B directly"
else
    fail "curl -x http://127.0.0.1:$PB/api/whoami: got '$body'"
fi

# CONNECT without TLS: curl opens a tunnel to B and speaks plain HTTP through it.
body=$(curl -s -p -x "$BASE" "http://127.0.0.1:$PB/api/whoami")
if [ "$body" = "{\"name\":\"B\",\"port\":$PB}" ]; then
    pass "curl -x --proxytunnel reaches backend B through CONNECT"
else
    fail "curl -p -x http://127.0.0.1:$PB/api/whoami: got '$body'"
fi

if [ "$FAILED" -ne 0 ]; then
    echo "smoke: $FAILED check(s) failed"
    exit 1
fi
echo "smoke: all checks passed"
