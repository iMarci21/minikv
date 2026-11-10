#!/usr/bin/env bash
# End-to-end TCP / epoll / RESP tests for net.c + cmd.c (PING, ECHO).
# Uses Python raw sockets so it works without redis-cli installed locally and
# can test true single-write pipelining, split packets, and malformed framing.
# Usage: tests/net.sh [path-to-binary]
set -euo pipefail

BIN="$(realpath "${1:-build/release/minikv}")"
PORT="${PORT:-6392}"
WORKDIR="$(mktemp -d)"
LOG="$WORKDIR/server.log"
SERVER_PID=""
FAILED=0

stop_server() {
    if [[ -n "$SERVER_PID" ]]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in $(seq 1 40); do
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then
                break
            fi
            sleep 0.05
        done
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill -KILL "$SERVER_PID" 2>/dev/null || true
        fi
        local code=0
        wait "$SERVER_PID" || code=$?
        SERVER_PID=""
        if [[ "$code" -ne 0 ]]; then
            echo "FAIL: server exited with code $code"
            cat "$LOG"
            FAILED=1
        fi
    fi
}

cleanup() {
    stop_server
    if grep -qE 'ERROR: (Address|Leak)Sanitizer|runtime error:' "$LOG" 2>/dev/null; then
        echo "FAIL: sanitizer report in server log"
        cat "$LOG"
        FAILED=1
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

start_server() {
    : >"$LOG"
    (cd "$WORKDIR" && exec "$BIN" --port "$PORT") >"$LOG" 2>&1 </dev/null &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if grep -q 'ready, listening on port' "$LOG"; then return 0; fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "FAIL: server exited during startup"
            cat "$LOG"
            exit 1
        fi
        sleep 0.1
    done
    echo "FAIL: server not ready after 5s"
    cat "$LOG"
    exit 1
}

start_server

python3 - "$PORT" <<'PYEOF' || FAILED=1
import socket
import sys
import time

port = int(sys.argv[1])


def connect():
    s = socket.create_connection(("127.0.0.1", port), timeout=2.0)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise AssertionError(f"unexpected EOF after {len(buf)}/{n} bytes: {bytes(buf)!r}")
        buf.extend(chunk)
    return bytes(buf)


def recv_line(s):
    buf = bytearray()
    while not buf.endswith(b"\r\n"):
        ch = s.recv(1)
        if not ch:
            raise AssertionError(f"unexpected EOF in line: {bytes(buf)!r}")
        buf.extend(ch)
    return bytes(buf)


# 1. PING, inline PING (used by redis-benchmark), PING <arg>, binary ECHO
with connect() as s:
    s.sendall(b"*1\r\n$4\r\nPING\r\n")
    assert recv_exact(s, 7) == b"+PONG\r\n"

    s.sendall(b"PING\r\n")
    assert recv_exact(s, 7) == b"+PONG\r\n"

    s.sendall(b"echo inline_hi\r\n")
    assert recv_exact(s, 15) == b"$9\r\ninline_hi\r\n"

    s.sendall(b"*1\r\n$4\r\npInG\r\n")
    assert recv_exact(s, 7) == b"+PONG\r\n"

    s.sendall(b"*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n")
    assert recv_exact(s, 11) == b"$5\r\nhello\r\n"

    s.sendall(b"*2\r\n$4\r\nECHO\r\n$5\r\na\x00b\r\n\r\n")
    assert recv_exact(s, 11) == b"$5\r\na\x00b\r\n\r\n"
print("ok:   PING, inline PING/ECHO, case-insensitive ping, PING <arg>, binary ECHO")

# 2. Command errors keep the connection open
with connect() as s:
    s.sendall(b"*1\r\n$7\r\nUNKNOWN\r\n")
    assert recv_line(s).startswith(b"-ERR ")

    s.sendall(b"*1\r\n$4\r\nECHO\r\n")
    assert recv_line(s).startswith(b"-ERR ")

    s.sendall(b"*1\r\n$4\r\nPING\r\n")
    assert recv_exact(s, 7) == b"+PONG\r\n"
print("ok:   unknown command and wrong arity return -ERR without dropping client")

# 3. True pipelining: 3 commands in a single write()
with connect() as s:
    s.sendall(
        b"*1\r\n$4\r\nPING\r\n"
        b"*2\r\n$4\r\nECHO\r\n$3\r\nfoo\r\n"
        b"*1\r\n$4\r\nPING\r\n"
    )
    want = b"+PONG\r\n$3\r\nfoo\r\n+PONG\r\n"
    assert recv_exact(s, len(want)) == want
print("ok:   pipelining (3 commands in one write)")

# 4. Split packet across two writes
with connect() as s:
    part1 = b"*2\r\n$4\r\nEC"
    part2 = b"HO\r\n$5\r\nsplit\r\n"
    s.sendall(part1)
    time.sleep(0.05)
    s.sendall(part2)
    assert recv_exact(s, 11) == b"$5\r\nsplit\r\n"
print("ok:   split command across two writes")

# 5. Malformed RESP -> -ERR Protocol error and only that connection closes
with connect() as bad, connect() as good:
    bad.sendall(b"*-1\r\n")
    line = recv_line(bad)
    assert line.startswith(b"-ERR Protocol error:"), f"unexpected reply: {line!r}"
    assert bad.recv(64) == b""  # server must close the desynced connection

    good.sendall(b"*1\r\n$4\r\nPING\r\n")
    assert recv_exact(good, 7) == b"+PONG\r\n"
print("ok:   protocol error sends -ERR, closes bad client, keeps other clients working")
PYEOF

# 6. Keep an active client connected while SIGTERM shuts the server down
#    (verifies net_destroy frees open clients cleanly under LSan).
python3 - "$PORT" <<'PYEOF' &
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2.0)
s.sendall(b"*1\r\n$4\r\nPING\r\n")
s.recv(7)
time.sleep(5.0)
PYEOF
HOLDER_PID=$!
sleep 0.1
stop_server
kill "$HOLDER_PID" 2>/dev/null || true
wait "$HOLDER_PID" 2>/dev/null || true
echo "ok:   clean shutdown on SIGTERM with an active client connected"

if [[ "$FAILED" -ne 0 ]]; then echo "SOME NET TESTS FAILED"; exit 1; fi
echo "ALL NET TESTS PASSED"
