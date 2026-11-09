#!/usr/bin/env bash
# Process lifecycle tests: argument handling and signal-driven shutdown.
# Usage: tests/lifecycle.sh [path-to-binary]
set -euo pipefail

BIN="$(realpath "${1:-build/release/minikv}")"
WORKDIR="$(mktemp -d)"
LOG="$WORKDIR/server.log"
SERVER_PID=""
FAILED=0

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then kill -KILL "$SERVER_PID" 2>/dev/null || true; fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

pass() { echo "ok:   $1"; }
fail() { echo "FAIL: $1"; FAILED=1; }

# Succeeds if the file contains no ASan/LSan/UBSan report.
no_sanitizer_report() {
    ! grep -qE 'ERROR: (Address|Leak)Sanitizer|runtime error:' "$1"
}

# check_cli <expected-exit> <usage-stream: stdout|stderr> <args...>
# Runs the binary to completion and checks exit code, usage text and sanitizer output.
check_cli() {
    local want="$1" usage_on="$2"; shift 2
    local desc="args: $*" out="$WORKDIR/out" err="$WORKDIR/err" code=0 usage_file
    "$BIN" "$@" >"$out" 2>"$err" </dev/null || code=$?
    if [[ "$usage_on" == stdout ]]; then usage_file="$out"; else usage_file="$err"; fi

    if [[ "$code" -ne "$want" ]]; then
        fail "$desc -> exit $code (expected $want)"; cat "$err"
    elif ! grep -q '^Usage:' "$usage_file"; then
        fail "$desc -> no usage text on $usage_on"
    elif ! no_sanitizer_report "$err"; then
        fail "$desc -> sanitizer report"; cat "$err"
    else
        pass "$desc -> exit $code"
    fi
}

# start_server <args...>: starts the server in the background, waits until it logs "ready".
start_server() {
    : >"$LOG"
    (cd "$WORKDIR" && exec "$BIN" "$@") >"$LOG" 2>&1 </dev/null &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if grep -q 'ready' "$LOG"; then return 0; fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            fail "server exited during startup"; cat "$LOG"; SERVER_PID=""; return 1
        fi
        sleep 0.1
    done
    fail "server not ready after 5s"; cat "$LOG"; return 1
}

# stop_server <signal>: sends the signal and waits up to 2s for the server to exit.
# Sets EXIT_CODE. If killed its is 137 (128 + SIGKILL).
stop_server() {
    kill "-$1" "$SERVER_PID" 2>/dev/null || true  # may already be dead; wait reports it
    ( sleep 2; kill -KILL "$SERVER_PID" 2>/dev/null ) &
    local watchdog=$!
    EXIT_CODE=0
    wait "$SERVER_PID" || EXIT_CODE=$?
    SERVER_PID=""
    kill "$watchdog" 2>/dev/null || true
    wait "$watchdog" 2>/dev/null || true
}

# check_clean_exit <description>: checks the result of the last stop_server.
check_clean_exit() {
    if [[ "$EXIT_CODE" -ne 0 ]]; then
        fail "$1 -> exit $EXIT_CODE (expected 0, 137 = did not stop within 2s)"; cat "$LOG"
    elif ! grep -q 'shutdown complete' "$LOG"; then
        fail "$1 -> no 'shutdown complete' in log"; cat "$LOG"
    elif ! no_sanitizer_report "$LOG"; then
        fail "$1 -> sanitizer report"; cat "$LOG"
    else
        pass "$1"
    fi
}

# --- Argument handling ---------------------------------------------------------

check_cli 0 stdout --help
check_cli 2 stderr --port 0
check_cli 2 stderr --port 65536
check_cli 2 stderr --port 12abc
check_cli 2 stderr --port -1
check_cli 2 stderr --port " 80"
check_cli 2 stderr --port ""
check_cli 2 stderr --port
check_cli 2 stderr --wal ""
check_cli 2 stderr --appendfsync sometimes
check_cli 2 stderr --bogus
check_cli 2 stderr --portx 80
check_cli 2 stderr 6379

# --- Options reach the server ----------------------------------------------------

if start_server --port=6391 --wal custom.wal --appendfsync always; then
    if grep -q 'port=6391, wal=custom.wal, appendfsync=always' "$LOG"; then
        pass "options are applied (--name=value and --name value)"
    else
        fail "options not applied"; cat "$LOG"
    fi
    stop_server TERM
fi

# --- Signal handling -----------------------------------------------------------

for sig in TERM INT; do
    if start_server; then
        stop_server "$sig"
        check_clean_exit "SIG$sig -> clean shutdown, exit 0"
    fi
done

if start_server; then
    kill -PIPE "$SERVER_PID"
    sleep 0.2
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        pass "SIGPIPE is ignored"
    else
        fail "SIGPIPE killed the server"
    fi
    stop_server TERM
    check_clean_exit "SIGTERM after SIGPIPE -> clean shutdown, exit 0"
fi

if [[ "$FAILED" -ne 0 ]]; then echo "SOME TESTS FAILED"; exit 1; fi
echo "ALL TESTS PASSED"
