# minikv architecture

This document records design decisions and *why* they were made. The enforceable
rules derived from it live in [`AGENTS.md`](../AGENTS.md).

## Modules and dependencies

```
            ┌────────┐
            │ main.c │  wires everything together, owns startup/shutdown
            └───┬────┘
                │
            ┌───▼───┐   bytes in / bytes out, epoll loop, struct client
            │  net  │
            └─┬───┬─┘
              │   │
     ┌────────▼┐ ┌▼────────┐
     │  proto  │ │   cmd   │  dispatch: validate args, log, apply, build reply
     └─────────┘ └─┬─────┬─┘
                   │     │
              ┌────▼┐  ┌─▼───┐
              │ wal │  │ db  │
              └─────┘  └─────┘
```

| Module  | Responsibility                                               | May include      |
|---------|--------------------------------------------------------------|------------------|
| `proto` | RESP parse + reply serialization. Pure, no I/O, no globals.  | (nothing)        |
| `db`    | Hash table, expiry metadata, lazy + active expiry.           | (nothing)        |
| `wal`   | Record encoding, buffered append, flush/fsync, replay.       | (nothing)        |
| `cmd`   | Command table, argument validation, WAL-then-db, replies.    | `proto` `db` `wal` |
| `net`   | Sockets, epoll loop, client buffers, calls proto + cmd.      | `proto` `cmd`    |
| `main`  | Signals, wiring, replay at startup, clean shutdown.          | everything       |
| `config`| Command-line parsing into `struct config`. Pure.             | (nothing); used only by `main` |
| `log`   | Timestamped one-line logging to stderr.                      | (nothing); used by all but `proto` |

`db` and `wal` never know about each other. **Replay avoids a cycle via a callback:**
`wal_replay(path, apply_fn, ctx)` decodes records and hands each to `apply_fn`;
`main` passes a `cmd` function that applies the record to the db *without* logging it again.

## Startup and shutdown order (`main.c`)

1. `config_parse`: usage errors exit with code 2 before anything is started.
2. Signals: `SIGPIPE` ignored; `SIGINT`/`SIGTERM` set a flag and stay **blocked**
   except while waiting (`sigsuspend` now, `epoll_pwait` from step 3 on), which unblocks
   them atomically. Blocking them before any thread exists means the fsync thread
   inherits the mask, so they are always delivered to the main thread.
3. `db_create` → 4. `wal_open` + replay → 5. `cmd_create` → 6. `net_create` (bind/listen).
   Replay happens before `bind`, so clients never see a partially loaded db.
7. Run until a stop signal.
8. Tear down in exact reverse order through a single cleanup path.

Exit codes: `0` clean shutdown or `--help`, `1` runtime/startup failure, `2` usage error.

## Expiry: absolute timestamps

- Expiry is stored as `int64_t` **absolute Unix time in milliseconds** (`CLOCK_REALTIME`),
  both in memory and in the WAL.
- `cmd` converts every relative form (`EXPIRE`, `PEXPIRE`, `SET ... EX/PX`, `SETEX`) to
  an absolute deadline *before* logging. A relative TTL is never written to the WAL;
  otherwise every replay would restart the countdown and keys would live forever across
  restarts.
- On replay, records whose deadline is already in the past may be skipped.
- Expired keys are removed lazily on access plus a bounded active-sampling cycle on a
  timer. Expiry-driven deletions are **not** logged: the absolute deadline in the log
  already reproduces them on replay.
- Wall-clock jumps (NTP, manual changes) shift expiry accordingly. This is accepted and
  matches Redis. Pure *durations* inside the process (e.g. the everysec tick) use
  `CLOCK_MONOTONIC`.

## WAL write path and fsync

### Group commit (all policies)

Durability work happens **once per event-loop iteration**, not once per command:

1. `net` reads from ready sockets; `proto` parses; `cmd` executes each command.
   For a write command, `cmd` first calls `wal_append()`, which encodes the record
   into an **in-memory WAL buffer** (no syscall), and only then mutates the db.
   If encoding fails (e.g. OOM), the command is rejected and the db is untouched.
2. Replies are queued in each client's write buffer but **not sent yet**.
3. Before the loop sends replies or goes back to `epoll_wait`, it calls `wal_flush()`,
   which `write()`s the buffer to the log (`O_APPEND`, handling short writes and
   `EINTR`) and applies the fsync policy below.
4. Only then are client write buffers flushed.

This way a client never sees `+OK` for a write that has not at least reached the
kernel (or the disk, for `always`), and a burst of N writes costs one `write()` and at
most one `fdatasync()` instead of N.

### Policies

| Policy     | After `write()` in `wal_flush()`                         | Max loss on power failure |
|------------|----------------------------------------------------------|---------------------------|
| `always`   | `fdatasync()` on the main thread before replies go out.  | nothing acknowledged      |
| `everysec` | Ask the fsync thread for an `fdatasync()` once per second. | ~1–2 s                  |
| `no`       | Nothing; the kernel flushes when it decides.             | ~30 s (kernel default)    |

`always` blocks the loop for one fsync per iteration. That is the whole point of the
policy: you trade throughput for durability. Group commit keeps it tolerable.

### `everysec`: one background fsync thread

The "no threads for core I/O" rule has exactly one exception: a single fsync thread.

- It only ever calls `fdatasync(wal_fd)`. It never touches the db, clients, or the WAL
  buffer. Communication goes through a mutex + condition variable guarding
  `{fsync_requested, fsync_in_progress, fsync_error, shutdown}`.
- The main loop owns a `timerfd` (registered in epoll) that ticks every second, so the
  tick fires even when no clients are active. On a tick: if bytes were written since
  the last fsync and no fsync is in flight, signal the thread. The main thread never
  waits on it.
- **Slow disk:** if an fsync is still in flight, `write()` to the same file can block on
  some filesystems. In that case `wal_flush()` keeps buffering instead of calling
  `write()`, for up to 2 s, then writes anyway. This bounds both loop stalls and data loss.

### Failure handling

- **`write()` fails** (e.g. `ENOSPC`): truncate the file back to the last fully written
  record offset, keep the buffer, log the error, and enter **read-only mode**: write
  commands get `-ERR` until a later flush succeeds. Reads keep working.
- **`fdatasync()` fails:** on Linux, a failed fsync may have already dropped the dirty
  pages, and a retry can falsely report success ("fsyncgate"). The on-disk state is
  unknown, so the server logs the error and stays read-only until restarted (`everysec`).
  With `always`, it refuses to acknowledge the pending batch, logs, and shuts down
  cleanly, because it can no longer keep its durability promise.
- None of this is client-triggered, so it doesn't conflict with "never exit on client input".

### Other durability details

- Use `fdatasync`. For append-only files it also persists the size change.
- When the WAL file is **created** (first start, or later during rewrite), `fsync` the
  parent directory once so the directory entry itself survives a crash.
- Clean shutdown: stop reading new commands, `wal_flush()`, tell the fsync thread to
  exit and `pthread_join` it, final `fdatasync` on the main thread, `close`.

## WAL record format (sketch)

```
file   := header record*
header := magic "MKVWAL" (6 bytes) | version u16
record := len u32 | crc32c u32 | payload[len]
payload:= op u8 | op-specific fields  (keys/values as len u32 + bytes, deadlines as i64)
```

All integers little-endian. `crc32c` covers the payload. Replay stops at the first
record that is short or fails its checksum, truncates the file there, and logs a warning.

## Roadmap

One module per step, each implemented properly and tested on its own, in an order where
every step also adds something that can be run and tested from the outside.

| # | Step | Depends on | What works afterwards | Tests added | Status |
|---|---|---|---|---|---|
| 1 | `main` skeleton + `log` + `config` | nothing | Process starts, parses args, shuts down cleanly on signals | `lifecycle.sh` (in CI) | done |
| 2 | `proto`: RESP parser + reply serializer | nothing (pure) | Correct parsing of partial, pipelined and malformed input | Unit tests (small C test runner, no framework), later a fuzz harness | done |
| 3 | `net` + a tiny `cmd` (`PING`/`ECHO`) | `proto` | `redis-cli ping` → `PONG` | First integration tests: pipelining, split packets, disconnects, connection limits | |
| 4 | `db` hash table + `cmd` gains `GET`/`SET`/`DEL`/`STRLEN` | `cmd` | In-memory KV store | Hash table unit tests (binary keys, resizing) + integration | |
| 5 | Incremental rehashing in `db` | `db` | Large datasets resize without pauses | Unit tests (operations correct during a rehash) | |
| 6 | Expiry: `EXPIRE`/`TTL`/`SET EX`, absolute deadlines, periodic cleanup | `db`, `net` | Keys expire | Integration | |
| 7 | `wal`: record format, replay, fsync policies, fsync thread | `cmd` | Data survives restarts; full integration suite re-enabled in CI | Unit (encoding, corrupt-tail replay) + restart tests | |
| 8+ | Snapshots / log rewrite, B+ tree | | Long-term goals | | |

Deviations from this order are discussed first and recorded here.
