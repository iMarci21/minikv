# minikv: Engineering Rules

A minimal Redis-compatible key-value store in C11. Single-threaded epoll event loop,
RESP protocol, in-memory hash table, append-only-log persistence.

## Commands

| Task                            | Command              | Output                  |
|---------------------------------|----------------------|-------------------------|
| Build (release)                 | `make`               | `build/release/minikv`  |
| Build with ASan + UBSan         | `make sanitize`      | `build/sanitize/minikv` |
| All tests (release)             | `make test`          |                         |
| All tests (sanitized)           | `make test-sanitize` |                         |
| Only lifecycle / integration    | `make test-lifecycle`, `make test-integration` (+ `-sanitize`) | |
| Format in place                 | `make format`        |                         |
| Check formatting (CI)           | `make format-check`  |                         |
| Clean                           | `make clean`         |                         |

Always run `make test-sanitize` before considering a change done. Release and sanitizer
builds use separate object directories, so switching between them never needs `make clean`.

## Tech Stack & Target

- C11, GCC and Clang, Linux x86_64. `_GNU_SOURCE` is defined for `epoll`/`accept4`.
- Core APIs: POSIX.1-2008, Linux `epoll`, non-blocking sockets.
- Build: GNU Make. Warnings are errors: `-Wall -Wextra -Wpedantic -Werror`.
- Formatting: clang-format **22.1.8**, pinned in `Makefile` (`CLANG_FORMAT_VERSION`) and CI.
  Bump both together.
- No third-party runtime dependencies. Test-only tools: `redis-cli`, `redis-benchmark`.

## Project Layout and Module Boundaries

- `src/main.c`   : signal setup, startup/shutdown order, wiring. No logic.
- `src/config.c` : command-line parsing. Pure (no I/O, no globals, never exits).
- `src/log.c`    : `log_info`/`log_warn`/`log_error`, one line per call to stderr.
- `src/net.c`    : sockets, epoll loop, per-connection read/write buffers.
- `src/proto.c`  : RESP parser and reply serializer. **Pure functions: no I/O, no
                   syscalls, no globals.** It takes bytes and returns a parse result,
                   so it can be unit-tested and fuzzed in isolation.
- `src/cmd.c`    : command table and dispatch. Validates arguments, appends to the WAL,
                   then applies to the db, then builds the reply via `proto`.
- `src/db.c`     : hash table and expiry. Knows nothing about sockets, RESP, or the WAL.
- `src/wal.c`    : append-only log, replay, fsync policy. Knows nothing about sockets,
                   RESP, or the db.
- `tests/`       : `lifecycle.sh` (args, signals, exit codes), `integration.sh`
                   (redis-cli end-to-end), later unit tests and fuzz harnesses.

Allowed includes: `net -> {proto, cmd}`, `cmd -> {proto, db, wal}`. `proto`, `db` and
`wal` include no other project module. `log` may be included by every module except
`proto`; `config` only by `main`. WAL replay uses a callback
(`wal_replay(path, apply_fn, ctx)`) wired up in `main.c`, so there is no `wal -> cmd`
or `wal -> db` dependency. Each module exposes a small header with opaque structs.
See [`docs/architecture.md`](docs/architecture.md) for the rationale.

## Roadmap

Work follows the step order in the Roadmap section of `docs/architecture.md`. Do not
start a later step, or pull in parts of one, without discussing it first. Update the
Status column when a step is done.

## Memory & Safety

1. Every allocation has one clear owner and a documented lifecycle. Document ownership
   in the header comment of any function that takes or returns a pointer.
2. Check every `malloc`/`calloc`/`realloc`. Never write `p = realloc(p, n)`; use a
   temporary so the original is not leaked on failure.
3. No unbounded string functions (`strcpy`, `strcat`, `sprintf`, `gets`). Use explicit
   lengths (`size_t`) and `snprintf` / `memcpy` with checked bounds.
4. Never treat network input as NUL-terminated. Keys and values are `(ptr, len)`
   pairs and may contain `\0`.
5. Check for integer overflow before arithmetic on lengths that come from the network
   (e.g. bulk string length in RESP, buffer growth sizes).
6. Enforce hard limits and reject violations with an error reply, never by crashing:
   max bulk length, max array elements, max input buffer per connection, max connections.
7. Code must run clean under `-fsanitize=address,undefined` with leak detection on.
   Sanitizers are built with `-fno-sanitize-recover=all`, so any UB fails the run.
8. Free everything on shutdown (close fds, free clients, free the db) so LeakSanitizer
   output stays meaningful.

## Architecture Constraints

- Single-threaded, non-blocking event loop. Do NOT introduce threads for core I/O.
  The **only** exception is one background thread for `everysec` WAL fsync, which calls
  nothing but `fdatasync(wal_fd)` and never touches the db, clients, or WAL buffer.
- Level-triggered epoll (`EPOLLIN`, add `EPOLLOUT` only while a write buffer is
  non-empty). Do not switch to edge-triggered without discussion.
- All sockets are non-blocking (`accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`).
- Reads and writes must handle: `EAGAIN`/`EWOULDBLOCK` (stop, wait for next event),
  `EINTR` (retry), **short reads and short writes**, and peer close (`read` returns 0).
- Partial RESP messages are normal. The parser must report "need more data" and
  leave the buffer untouched; it must support pipelined commands (many commands in
  one read) and commands split across reads.
- Connection state lives in an explicit `struct client` (fd, read buffer, write
  buffer, parse state). No hidden per-connection globals.
- Ignore `SIGPIPE` (or use `MSG_NOSIGNAL`); a closed peer must not kill the server.
- Handle `SIGINT`/`SIGTERM` by setting a flag checked by the event loop, then shut
  down cleanly: flush the WAL, free all memory, exit 0.
- No `sleep()` or other blocking calls inside the loop. Anything slow (WAL fsync,
  rewrite) must be bounded or explicitly designed around.

## Persistence (WAL)

- Log format is binary and versioned, and each record carries a length and a checksum.
- Fsync policy is a documented, configurable choice (`always` / `everysec` / `no`).
- Replay on startup must tolerate a truncated or corrupt **tail** (crash mid-write):
  stop at the last valid record, truncate the file there, and log a warning.
- `cmd` appends the record to the in-memory WAL buffer *before* mutating the db. If the
  append fails, the command is rejected and the db is untouched.
- Group commit: the WAL buffer is written (and fsynced under `always`) once per loop
  iteration, **before** any client replies from that iteration are sent. A client must
  never receive `+OK` for a write that has not reached the log.
- Expiry is stored as an **absolute** Unix timestamp in milliseconds (`int64_t`,
  `CLOCK_REALTIME`). Relative TTLs (`EXPIRE`, `SET ... EX`) are converted to absolute
  deadlines before logging; never write a relative TTL to the WAL. Use `CLOCK_MONOTONIC`
  for in-process durations.
- Periodic work (everysec fsync tick, active expiry) is driven by a `timerfd` in epoll.
- A failed `fdatasync` is never retried as if nothing happened: enter read-only mode
  (`everysec`) or shut down cleanly (`always`). See `docs/architecture.md`.

## Error Handling

- Functions that can fail return `0` on success and `-1` on failure (or a dedicated
  enum), with results passed via out-parameters. Never ignore a return value.
- Check every syscall. Log failures with `strerror(errno)` immediately, before another
  call can overwrite `errno`.
- Protocol errors from clients -> send a RESP error (`-ERR ...`) and keep serving.
  Unrecoverable protocol desync -> send an error and close that connection only.
- The server must never `abort()` or `exit()` because of client input.
- `assert()` is only for internal invariants, never for validating external input.

## Code Style

- Format with `clang-format` (config in `.clang-format`). CI enforces it.
- `snake_case` for functions and variables, `UPPER_CASE` for macros and constants.
  Prefix public symbols with their module (`db_get`, `wal_append`, `proto_parse`).
- Mark everything file-local `static`. Use `const` wherever possible.
- Prefer fixed-width integer types (`uint32_t`, `int64_t`) for protocol and file formats.
- Avoid macros that hide control flow. Avoid VLAs.
- Comments explain *why*, not *what*. Document every non-obvious invariant.

## Comments
- Keep comments as natural as possible to describe methods, functions or more complex structures.
- Existing comments shall not be changed, as they might have been subject to user input.
- Existing comments CAN be modified if the underlying logic of the function it refers to changed  aswell. It's imperative to keep the style the same, even when adding onto the existing comment or rewriting it the reflect the new logic.


## Testing

- Every new command or protocol feature comes with an integration test in
  `tests/integration.sh` (or a unit test for pure modules).
- Test edge cases: empty values, binary data, very large values, pipelining,
  split packets, malformed input, client disconnect mid-command, restart + WAL recovery.
- Parser changes should be fuzzed: `proto.c` has a libFuzzer harness in `tests/fuzz/`.

## Git & PR Conventions

- Branch per task: `feat/...`, `fix/...`, `docs/...`, `refactor/...`.
- Conventional Commits: `feat: add EXPIRE command`, `fix: handle short writes`.
- One logical change per commit. Keep PRs small enough to review in one sitting.
- Never commit build artifacts (`*.o`, `minikv`), WAL files, or core dumps.

## Definition of Done

- [ ] Compiles warning-free with both GCC and Clang
- [ ] `make sanitize` + integration tests pass with no ASan/UBSan/LSan reports
- [ ] New behavior covered by tests
- [ ] `make format` produces no diff
- [ ] README/command table updated if user-visible behavior changed
