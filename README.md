## Minikv-Project

Currently working on building a minimal networked "Redis-Style" KV-Store.
Plans include implementing an in-memory hash table, TCP server and persistence.

# Networking: 
Single-Threaded event-loop probably using epoll, non-blocking sockets and an event-parser.

# Database: 
WAL, snapshots and key expiration. Long term goal of replacing the hash table with a B+ tree.

# Systems:  
Hash table with memory management and incremental rehashing
