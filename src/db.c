#include "db.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DB_INIT_CAP 64U /* must be 2^n */

struct db_entry {
    struct db_entry *next;
    uint64_t hash;
    size_t key_len;
    size_t val_len;
    char *val;
    char key[]; /* key_len bytes stored inline with the entry allocation */
};

struct db {
    struct db_entry **buckets;
    size_t cap;  /* number of buckets, always 2^n */
    size_t size; /* number of entries stored */
    struct db_entry **old_buckets;
    size_t old_cap;
    size_t old_used;
    size_t rehash_idx;
};

/* 64-bit FNV-1a over raw bytes. */
static uint64_t hash_bytes(const char *data, size_t len)
{
    uint64_t h = 14695981039346656037ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int key_matches(const struct db_entry *e, uint64_t hash, const char *key, size_t key_len)
{
    if (e->hash != hash || e->key_len != key_len) {
        return 0;
    }
    if (key_len == 0) {
        return 1;
    }
    return memcmp(e->key, key, key_len) == 0;
}

/*
 * Allocates a 2x bucket array and moves the current table to old_buckets so
 * entries can be migrated incrementally. Returns 0 on success, -1 on OOM
 * leaving old table intact.
 */
static int db_start_rehash(struct db *d)
{
    size_t new_cap;
    struct db_entry **new_buckets;

    if (d->cap > SIZE_MAX / (2U * sizeof *new_buckets)) {
        return -1;
    }
    new_cap = d->cap * 2U;
    new_buckets = calloc(new_cap, sizeof *new_buckets);
    if (new_buckets == NULL) {
        return -1;
    }

    d->old_buckets = d->buckets;
    d->old_cap = d->cap;
    d->old_used = d->size;
    d->rehash_idx = 0;
    d->buckets = new_buckets;
    d->cap = new_cap;
    return 0;
}

/*
 * Migrates up to n non-empty buckets from old_buckets to buckets, scanning at
 * most 10*n empty slots so sparse tables do not stall a single operation.
 */
static void db_rehash_step(struct db *d, size_t n)
{
    size_t empty_visits = n * 10U;
    size_t mask;

    if (d->old_buckets == NULL) {
        return;
    }
    mask = d->cap - 1U;
    while (n > 0 && d->old_used > 0) {
        struct db_entry *e;

        while (d->old_buckets[d->rehash_idx] == NULL) {
            d->rehash_idx++;
            if (d->rehash_idx >= d->old_cap) {
                free(d->old_buckets);
                d->old_buckets = NULL;
                d->old_cap = 0;
                d->old_used = 0;
                d->rehash_idx = 0;
                return;
            }
            if (--empty_visits == 0) {
                return;
            }
        }

        e = d->old_buckets[d->rehash_idx];
        while (e != NULL) {
            struct db_entry *next = e->next;
            size_t idx = (size_t)(e->hash & mask);

            e->next = d->buckets[idx];
            d->buckets[idx] = e;
            e = next;
            d->old_used--;
        }
        d->old_buckets[d->rehash_idx] = NULL;
        d->rehash_idx++;
        n--;
    }

    if (d->old_used == 0) {
        free(d->old_buckets);
        d->old_buckets = NULL;
        d->old_cap = 0;
        d->rehash_idx = 0;
    }
}

static struct db_entry *db_find_entry(struct db *d, uint64_t h, const char *key, size_t key_len)
{
    if (d->old_buckets != NULL) {
        size_t old_idx = (size_t)(h & (d->old_cap - 1U));

        for (struct db_entry *e = d->old_buckets[old_idx]; e != NULL; e = e->next) {
            if (key_matches(e, h, key, key_len)) {
                return e;
            }
        }
    }

    {
        size_t idx = (size_t)(h & (d->cap - 1U));

        for (struct db_entry *e = d->buckets[idx]; e != NULL; e = e->next) {
            if (key_matches(e, h, key, key_len)) {
                return e;
            }
        }
    }
    return NULL;
}

static void free_bucket_array(struct db_entry **buckets, size_t cap)
{
    if (buckets == NULL) {
        return;
    }
    for (size_t i = 0; i < cap; i++) {
        struct db_entry *e = buckets[i];

        while (e != NULL) {
            struct db_entry *next = e->next;
            free(e->val);
            free(e);
            e = next;
        }
    }
    free(buckets);
}

struct db *db_create(void)
{
    struct db *d = calloc(1U, sizeof *d);

    if (d == NULL) {
        return NULL;
    }
    d->buckets = calloc(DB_INIT_CAP, sizeof *d->buckets);
    if (d->buckets == NULL) {
        free(d);
        return NULL;
    }
    d->cap = DB_INIT_CAP;
    d->size = 0;
    return d;
}

void db_destroy(struct db *d)
{
    if (d == NULL) {
        return;
    }
    free_bucket_array(d->old_buckets, d->old_cap);
    free_bucket_array(d->buckets, d->cap);
    free(d);
}

size_t db_size(const struct db *d)
{
    return d->size;
}

int db_is_rehashing(const struct db *d)
{
    return d->old_buckets != NULL;
}

int db_set(struct db *d, const char *key, size_t key_len, const char *val, size_t val_len)
{
    uint64_t h = hash_bytes(key, key_len);
    struct db_entry *existing;
    size_t idx;
    char *val_copy = NULL;

    /* Alloc new value copy first so incase of OOM never mutates exisitng state */
    if (val_len > 0) {
        val_copy = malloc(val_len);
        if (val_copy == NULL) {
            return -1;
        }
        memcpy(val_copy, val, val_len);
    }

    if (d->old_buckets != NULL) {
        db_rehash_step(d, 1U);
    }

    existing = db_find_entry(d, h, key, key_len);
    if (existing != NULL) {
        free(existing->val);
        existing->val = val_copy;
        existing->val_len = val_len;
        return 0;
    }

    if (d->old_buckets == NULL && d->size >= d->cap) {
        /* If growing fails with OOM, we can still insert into the current table. */
        (void)db_start_rehash(d);
    }
    idx = (size_t)(h & (d->cap - 1U));

    if (key_len > SIZE_MAX - sizeof(struct db_entry)) {
        free(val_copy);
        return -1;
    }

    {
        struct db_entry *e = malloc(sizeof *e + key_len);

        if (e == NULL) {
            free(val_copy);
            return -1;
        }
        e->hash = h;
        e->key_len = key_len;
        e->val_len = val_len;
        e->val = val_copy;
        if (key_len > 0) {
            memcpy(e->key, key, key_len);
        }
        e->next = d->buckets[idx];
        d->buckets[idx] = e;
        d->size++;
    }
    return 0;
}

int db_get(struct db *d, const char *key, size_t key_len, const char **val_out, size_t *val_len_out)
{
    uint64_t h = hash_bytes(key, key_len);
    const struct db_entry *e;

    if (d->old_buckets != NULL) {
        db_rehash_step(d, 1U);
    }

    e = db_find_entry(d, h, key, key_len);
    if (e != NULL) {
        /* Never hand out a NULL pointer when val_len == 0. */
        *val_out = (e->val != NULL) ? e->val : "";
        *val_len_out = e->val_len;
        return 1;
    }
    return 0;
}

int db_del(struct db *d, const char *key, size_t key_len)
{
    uint64_t h = hash_bytes(key, key_len);

    if (d->old_buckets != NULL) {
        db_rehash_step(d, 1U);
    }

    if (d->old_buckets != NULL) {
        size_t old_idx = (size_t)(h & (d->old_cap - 1U));
        struct db_entry **pp = &d->old_buckets[old_idx];

        while (*pp != NULL) {
            struct db_entry *e = *pp;

            if (key_matches(e, h, key, key_len)) {
                *pp = e->next;
                free(e->val);
                free(e);
                d->size--;
                d->old_used--;
                if (d->old_used == 0) {
                    free(d->old_buckets);
                    d->old_buckets = NULL;
                    d->old_cap = 0;
                    d->rehash_idx = 0;
                }
                return 1;
            }
            pp = &e->next;
        }
    }

    {
        size_t idx = (size_t)(h & (d->cap - 1U));
        struct db_entry **pp = &d->buckets[idx];

        while (*pp != NULL) {
            struct db_entry *e = *pp;

            if (key_matches(e, h, key, key_len)) {
                *pp = e->next;
                free(e->val);
                free(e);
                d->size--;
                return 1;
            }
            pp = &e->next;
        }
    }
    return 0;
}
