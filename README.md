# Minikv-Project

Minimal networked Redis-compatible key-value store written in C11 on Linux.

## Build & run

```sh
make                  # build/release/minikv
make sanitize         # build/sanitize/minikv (ASan + UBSan)
make test             # unit + lifecycle + network tests

build/release/minikv [--port N] [--wal PATH] [--appendfsync always|everysec|no]

# Connectivity & diagnostics
redis-cli ping                              # -> PONG
redis-cli echo "testing"                    # -> "testing"

# Key-value operations
redis-cli set user:1 "marci"                # -> OK
redis-cli get user:1                        # -> "marci"
redis-cli strlen user:1                     # -> (integer) 5
redis-cli del user:1                        # -> (integer) 1

# Key expiry (EX / PX / SETEX / PSETEX / EXPIRE / PEXPIRE / TTL / PTTL / PERSIST)
redis-cli set session:abc "token" EX 10     # -> OK
redis-cli ttl session:abc                   # -> (integer) 10
redis-cli pttl session:abc                  # -> (integer) ~9995
redis-cli persist session:abc               # -> (integer) 1

# Throughput benchmarks (pipelined)
redis-benchmark -t ping -n 100000 -P 16 -q
redis-benchmark -t set,get -n 200000 -r 100000 -P 16 -q
```

Defaults: port `6379`, WAL `minikv.wal`, `appendfsync everysec`. Stop with Ctrl-C or
`SIGTERM`. Exit codes: `0` clean shutdown, `1` runtime error, `2` usage error.
Progress is tracked in the roadmap in [docs/architecture.md](docs/architecture.md).

## Networking
Single-threaded `epoll` event loop (`epoll_pwait` with atomic signal unblocking) over non-blocking TCP sockets (`accept4`), per-client dynamic read/write buffers, and a pure zero-I/O RESP2 + inline protocol parser supporting pipelined and split packets.

## Database
Binary-safe in-memory key-value store with millisecond-precision key expiration (`CLOCK_REALTIME` absolute deadlines), combining lazy expiration on lookup with bounded active sampling driven by a 10 Hz `CLOCK_MONOTONIC` `timerfd` in `epoll`. Planned: append-only WAL (`always` / `everysec` / `no` fsync policies), log rewrite/snapshots, and a long-term goal of replacing the hash table with a B+ tree.

## Systems
Custom 64-bit FNV-1a chained hash table with inline key storage and two-table incremental rehashing (amortizing bucket migration across operations to avoid resize pauses), strict ownership and bounds checking, and zero-leak builds verified under AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer.
