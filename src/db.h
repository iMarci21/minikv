#ifndef MINIKV_DB_H
#define MINIKV_DB_H

#include <stddef.h>
#include <stdint.h>

/*
 * In-memory key-value hash table.
 *
 * Pure storage module: knows nothing about the rest.
 * Keys and values are binary-safe (ptr, len) pairs and may contain '\0'.
 *
 * Expiry deadlines are stored as absolute Unix timestamps in milliseconds
 * (CLOCK_REALTIME), or DB_NO_EXPIRE (-1) for keys without a TTL. Callers pass
 * the current timestamp (now_ms) into accessors so db remains pure and
 * deterministic under test.
 *
 * Ownership:
 *   - db_set() copies key and val into heap memory owned by struct db.
 *   - db_get() returns a borrowed pointer into the entry inside struct db; it
 *     remains valid until the next mutation (db_set / db_del / db_destroy).
 */

#define DB_NO_EXPIRE (-1)

struct db;

/*
 * Allocates an empty db. Caller owns the returned pointer and must free
 * it by db_destroy(). Returns NULL on out-of-memory.
 */
struct db *db_create(void);

/* Frees all entries and the db itself. Safe to call with NULL. */
void db_destroy(struct db *d);

/* Returns # of keys currently stored. */
size_t db_size(const struct db *d);

/* Returns 1 if an incremental rehash is currently in progress, 0 otherwise. */
int db_is_rehashing(const struct db *d);

/*
 * Inserts or overwrites key with val and sets its deadline to expire_at_ms
 * (or DB_NO_EXPIRE for no TTL). Both key and val are copied.
 * On allocation failure, the database is left unchanged, -1 is returned.
 */
int db_set(struct db *d, const char *key, size_t key_len, const char *val, size_t val_len,
           int64_t expire_at_ms);

/*
 * Looks up key, lazily removing it if its deadline has passed at now_ms.
 *   - If found: sets *val_out (borrowed pointer) and *val_len_out, returns 1.
 *   - If not found (or expired): returns 0 (*val_out and *val_len_out are untouched).
 */
int db_get(struct db *d, const char *key, size_t key_len, int64_t now_ms, const char **val_out,
           size_t *val_len_out);

/*
 * Removes key if present and not already expired at now_ms.
 * Returns 1 if a live key existed and was removed, 0 otherwise.
 */
int db_del(struct db *d, const char *key, size_t key_len, int64_t now_ms);

/*
 * Sets or updates the absolute deadline on an existing live key.
 * If expire_at_ms <= now_ms, the key is removed immediately.
 * Returns 1 if the key was live at now_ms, 0 if it did not exist.
 */
int db_expire_at(struct db *d, const char *key, size_t key_len, int64_t expire_at_ms,
                 int64_t now_ms);

/*
 * Clears any TTL on an existing live key.
 * Returns 1 if the key existed and had a TTL removed, 0 otherwise.
 */
int db_persist(struct db *d, const char *key, size_t key_len, int64_t now_ms);

/*
 * Returns remaining time-to-live in milliseconds at now_ms:
 *   -2 if the key does not exist (or was lazily expired),
 *   -1 if the key exists and has no expiry,
 *   >= 0 remaining milliseconds otherwise.
 */
int64_t db_pttl(struct db *d, const char *key, size_t key_len, int64_t now_ms);

/*
 * Bounded active-expiry step called periodically from the event loop timer.
 * Samples a small batch of buckets and removes entries expired at now_ms,
 * plus advances incremental rehashing if active. Returns # of expired keys removed.
 */
size_t db_expire_cycle(struct db *d, int64_t now_ms);

#endif
