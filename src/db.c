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
 * Doubles the bucket array and re-links existing entries using their cached
 * 64-bit hashes. Returns 0 on success, -1 on OOM leaving old table intact.
 */
static int db_grow(struct db *d)
{
    size_t new_cap;
    size_t mask;
    struct db_entry **new_buckets;

    if (d->cap > SIZE_MAX / (2U * sizeof *new_buckets)) {
        return -1;
    }
    new_cap = d->cap * 2U;
    new_buckets = calloc(new_cap, sizeof *new_buckets);
    if (new_buckets == NULL) {
        return -1;
    }

    mask = new_cap - 1U;
    for (size_t i = 0; i < d->cap; i++) {
        struct db_entry *e = d->buckets[i];

        while (e != NULL) {
            struct db_entry *next = e->next;
            size_t idx = (size_t)(e->hash & mask);

            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }

    free(d->buckets);
    d->buckets = new_buckets;
    d->cap = new_cap;
    return 0;
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
    for (size_t i = 0; i < d->cap; i++) {
        struct db_entry *e = d->buckets[i];

        while (e != NULL) {
            struct db_entry *next = e->next;
            free(e->val);
            free(e);
            e = next;
        }
    }
    free(d->buckets);
    free(d);
}

size_t db_size(const struct db *d)
{
    return d->size;
}

int db_set(struct db *d, const char *key, size_t key_len, const char *val, size_t val_len)
{
    uint64_t h = hash_bytes(key, key_len);
    size_t idx = (size_t)(h & (d->cap - 1U));
    char *val_copy = NULL;

    /* Alloc new value copy first so incase of OOM never mutates exisitng state */
    if (val_len > 0) {
        val_copy = malloc(val_len);
        if (val_copy == NULL) {
            return -1;
        }
        memcpy(val_copy, val, val_len);
    }

    for (struct db_entry *e = d->buckets[idx]; e != NULL; e = e->next) {
        if (key_matches(e, h, key, key_len)) {
            free(e->val);
            e->val = val_copy;
            e->val_len = val_len;
            return 0;
        }
    }

    if (d->size >= d->cap) {
        /* If growing fails with OOM, we can still insert into the current table. */
        if (db_grow(d) == 0) {
            idx = (size_t)(h & (d->cap - 1U));
        }
    }

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

int db_get(const struct db *d, const char *key, size_t key_len, const char **val_out,
           size_t *val_len_out)
{
    uint64_t h = hash_bytes(key, key_len);
    size_t idx = (size_t)(h & (d->cap - 1U));

    for (const struct db_entry *e = d->buckets[idx]; e != NULL; e = e->next) {
        if (key_matches(e, h, key, key_len)) {
            /* Never hand out a NULL pointer when val_len == 0. */
            *val_out = (e->val != NULL) ? e->val : "";
            *val_len_out = e->val_len;
            return 1;
        }
    }
    return 0;
}

int db_del(struct db *d, const char *key, size_t key_len)
{
    uint64_t h = hash_bytes(key, key_len);
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
    return 0;
}
