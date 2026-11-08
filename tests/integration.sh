#!/usr/bin/env bash
# Integration tests for minikv. Usage: tests/integration.sh [path-to-binary]
set -euo pipefail

BIN="$(realpath "${1:-build/release/minikv}")"
PORT="${PORT:-6390}"
WORKDIR="$(mktemp -d)"
LOG="$WORKDIR/server.log"
CLI=(redis-cli -p "$PORT")
SERVER_PID=""
FAILED=0

start_server() {
    # Server takes port as first arg
    (cd "$WORKDIR" && exec "$BIN" "$PORT") >>"$LOG" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if "${CLI[@]}" ping >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "server did not start"; cat "$LOG"; exit 1
}

stop_server() {
    if [[ -n "$SERVER_PID" ]]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        # San abort or crash here
        wait "$SERVER_PID" || { echo "FAIL: server exited non-zero"; FAILED=1; }
        SERVER_PID=""
    fi
}

cleanup() {
    stop_server
    if [[ "$FAILED" -ne 0 ]]; then echo "--- server log ---"; cat "$LOG"; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

expect() { # expect <expected-output> <redis-cli args...>
    local want="$1"; shift
    local got
    got="$("${CLI[@]}" "$@")"
    if [[ "$got" == "$want" ]]; then
        echo "ok:   $*"
    else
        echo "FAIL: $* -> '$got' (expected '$want')"
        FAILED=1
    fi
}

start_server

expect "PONG" ping
expect "OK"   set foo bar
expect "bar"  get foo
expect "1"    del foo
expect ""     get foo            # missing key -> nil (prints empty)
expect "OK"   set empty ""
expect ""     get empty
expect "OK"   set big "$(head -c 100000 /dev/zero | tr '\0' 'x')"
expect "100000" strlen big       # remove if I dont implement STRLEN

# Pipelining: many commands in a single write
pipelined="$(printf 'SET a 1\r\nSET b 2\r\nGET a\r\nGET b\r\n' | "${CLI[@]}")"
[[ "$pipelined" == $'OK\nOK\n1\n2' ]] && echo "ok:   pipelining" \
    || { echo "FAIL: pipelining -> '$pipelined'"; FAILED=1; }

# Persistence across restart
expect "OK" set persisted yes
stop_server
start_server
expect "yes" get persisted

if [[ "$FAILED" -ne 0 ]]; then echo "SOME TESTS FAILED"; exit 1; fi
echo "ALL TESTS PASSED"
