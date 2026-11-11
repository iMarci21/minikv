#ifndef MINIKV_DB_H
#define MINIKV_DB_H

#include <stddef.h>

/*
 * In-memory key-value hash table.
 *
 * Pure storage module: knows nothing about the rest.
 * Keys and values are binary-safe (ptr, len) pairs and may contain '\0'.
 *
 * Ownership:
 *   - db_set() copies key and val into heap memory owned by struct db.
 *   - db_get() returns a borrowed pointer into the entry inside struct db; it
 *     remains valid until the next mutation (db_set / db_del / db_destroy).
 */

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

/*
 * Inserts or overwrites key with val. Both key and val are copied.
 * On allocation failure, the database is left unchanged, -1 is returned.
 */
int db_set(struct db *d, const char *key, size_t key_len, const char *val, size_t val_len);

/*
 * Looks up key.
 *   - If found: sets *val_out (borrowed pointer) and *val_len_out, returns 1.
 *   - If not found: returns 0 (*val_out and *val_len_out are untouched).
 */
int db_get(const struct db *d, const char *key, size_t key_len, const char **val_out,
           size_t *val_len_out);

/*
 * Removes key if present.
 * Returns 1 if the key existed and was removed, 0 if it did not exist.
 */
int db_del(struct db *d, const char *key, size_t key_len);

#endif
