# Minikv-Project

Currently working on building a minimal networked "Redis-Style" KV-Store.
Plans include implementing an in-memory hash table, TCP server and persistence.

## Build & run

```sh
make                  # build/release/minikv
make sanitize         # build/sanitize/minikv (ASan + UBSan)
make test             # lifecycle + integration tests

build/release/minikv [--port N] [--wal PATH] [--appendfsync always|everysec|no]
```

Defaults: port `6379`, WAL `minikv.wal`, `appendfsync everysec`. Stop with Ctrl-C or
`SIGTERM`. Exit codes: `0` clean shutdown, `1` runtime error, `2` usage error.
Progress is tracked in the roadmap in [docs/architecture.md](docs/architecture.md).

## Networking: 
Single-Threaded event-loop probably using epoll, non-blocking sockets and an event-parser.

## Database: 
WAL, snapshots and key expiration. Long term goal of replacing the hash table with a B+ tree.

## Systems:  
Hash table with memory management and incremental rehashing
