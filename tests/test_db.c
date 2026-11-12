#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            g_failed = 1;                                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_set_get_overwrite_del(void)
{
    struct db *d = db_create();
    const char *val = NULL;
    size_t val_len = 0;

    CHECK(d != NULL);
    CHECK(db_size(d) == 0);

    /* Missing key */
    CHECK(db_get(d, "foo", 3, 0, &val, &val_len) == 0);
    CHECK(db_del(d, "foo", 3, 0) == 0);

    /* Insert */
    CHECK(db_set(d, "foo", 3, "bar", 3, DB_NO_EXPIRE) == 0);
    CHECK(db_size(d) == 1);
    CHECK(db_get(d, "foo", 3, 0, &val, &val_len) == 1);
    CHECK(val_len == 3 && memcmp(val, "bar", 3) == 0);

    /* Overwrite same key -> size stays 1, value updates */
    CHECK(db_set(d, "foo", 3, "updated_longer_value", 20, DB_NO_EXPIRE) == 0);
    CHECK(db_size(d) == 1);
    CHECK(db_get(d, "foo", 3, 0, &val, &val_len) == 1);
    CHECK(val_len == 20 && memcmp(val, "updated_longer_value", 20) == 0);

    /* Delete existing and re-delete missing */
    CHECK(db_del(d, "foo", 3, 0) == 1);
    CHECK(db_size(d) == 0);
    CHECK(db_get(d, "foo", 3, 0, &val, &val_len) == 0);
    CHECK(db_del(d, "foo", 3, 0) == 0);

    db_destroy(d);
    printf("ok:   db set / get / overwrite / del\n");
}

static void test_empty_and_binary_keys_and_values(void)
{
    struct db *d = db_create();
    const char *val = NULL;
    size_t val_len = 99;

    CHECK(d != NULL);

    /* Empty key with empty value */
    CHECK(db_set(d, "", 0, "", 0, DB_NO_EXPIRE) == 0);
    CHECK(db_get(d, "", 0, 0, &val, &val_len) == 1);
    CHECK(val != NULL && val_len == 0);

    /* Two keys that share the same C-string prefix before an embedded NUL must be distinct! */
    CHECK(db_set(d, "a\0one", 5, "v\01", 3, DB_NO_EXPIRE) == 0);
    CHECK(db_set(d, "a\0two", 5, "v\02", 3, DB_NO_EXPIRE) == 0);
    CHECK(db_size(d) == 3);

    CHECK(db_get(d, "a\0one", 5, 0, &val, &val_len) == 1);
    CHECK(val_len == 3 && memcmp(val, "v\01", 3) == 0);

    CHECK(db_get(d, "a\0two", 5, 0, &val, &val_len) == 1);
    CHECK(val_len == 3 && memcmp(val, "v\02", 3) == 0);

    db_destroy(d);
    printf("ok:   db empty and binary keys/values with embedded NULs\n");
}

static void test_incremental_rehash_operations(void)
{
    struct db *d = db_create();
    const char *val = NULL;
    size_t val_len = 0;

    CHECK(d != NULL);
    CHECK(db_is_rehashing(d) == 0);

    /* Fill initial capacity (64 keys): should not trigger rehash yet. */
    for (int i = 0; i < 64; i++) {
        char k[32];
        char v[32];
        int kl = snprintf(k, sizeof k, "init:%d", i);
        int vl = snprintf(v, sizeof v, "val:%d", i);

        CHECK(db_set(d, k, (size_t)kl, v, (size_t)vl, DB_NO_EXPIRE) == 0);
    }
    CHECK(db_size(d) == 64);
    CHECK(db_is_rehashing(d) == 0);

    /* 65th insert starts incremental rehashing rather than moving all buckets at once. */
    CHECK(db_set(d, "init:64", 7, "val:64", 6, DB_NO_EXPIRE) == 0);
    CHECK(db_size(d) == 65);
    CHECK(db_is_rehashing(d) == 1);

    /*
     * While rehashing is in progress:
     * 1. Overwrite keys in the old table and ensure size does not grow (no duplicates).
     * 2. Delete a key in the old table and a key in the new table.
     * 3. Insert a brand new key into the new table.
     */
    CHECK(db_set(d, "init:50", 7, "overwritten:50", 14, DB_NO_EXPIRE) == 0);
    CHECK(db_size(d) == 65);
    CHECK(db_is_rehashing(d) == 1);

    CHECK(db_get(d, "init:50", 7, 0, &val, &val_len) == 1);
    CHECK(val_len == 14 && memcmp(val, "overwritten:50", 14) == 0);

    CHECK(db_del(d, "init:55", 7, 0) == 1);
    CHECK(db_del(d, "init:55", 7, 0) == 0);
    CHECK(db_get(d, "init:55", 7, 0, &val, &val_len) == 0);
    CHECK(db_size(d) == 64);

    CHECK(db_del(d, "init:64", 7, 0) == 1);
    CHECK(db_get(d, "init:64", 7, 0, &val, &val_len) == 0);
    CHECK(db_size(d) == 63);
    CHECK(db_is_rehashing(d) == 1);

    /* Drive the rehash to completion via lookups and verify every surviving key. */
    for (int step = 0; step < 128 && db_is_rehashing(d); step++) {
        CHECK(db_get(d, "init:0", 6, 0, &val, &val_len) == 1);
    }
    CHECK(db_is_rehashing(d) == 0);
    CHECK(db_size(d) == 63);

    for (int i = 0; i < 64; i++) {
        char k[32];
        char want[32];
        int kl = snprintf(k, sizeof k, "init:%d", i);
        int wl;

        if (i == 55) {
            CHECK(db_get(d, k, (size_t)kl, 0, &val, &val_len) == 0);
            continue;
        }
        if (i == 50) {
            wl = snprintf(want, sizeof want, "overwritten:50");
        } else {
            wl = snprintf(want, sizeof want, "val:%d", i);
        }
        CHECK(db_get(d, k, (size_t)kl, 0, &val, &val_len) == 1);
        CHECK(val_len == (size_t)wl && memcmp(val, want, val_len) == 0);
    }

    db_destroy(d);

    /* Also verify destroying a db mid-rehash frees both tables cleanly (checked by LSan). */
    d = db_create();
    CHECK(d != NULL);
    for (int i = 0; i < 70; i++) {
        char k[32];
        int kl = snprintf(k, sizeof k, "mid:%d", i);
        CHECK(db_set(d, k, (size_t)kl, "x", 1, DB_NO_EXPIRE) == 0);
    }
    CHECK(db_is_rehashing(d) == 1);
    db_destroy(d);

    printf("ok:   db incremental rehashing (get / overwrite / del / destroy mid-rehash)\n");
}

static void test_expiry_lazy_and_active(void)
{
    struct db *d = db_create();
    const char *val = NULL;
    size_t val_len = 0;

    CHECK(d != NULL);

    /* Missing key -> PTTL -2, PERSIST 0, EXPIRE 0 */
    CHECK(db_pttl(d, "k", 1, 1000) == -2);
    CHECK(db_persist(d, "k", 1, 1000) == 0);
    CHECK(db_expire_at(d, "k", 1, 2000, 1000) == 0);

    /* Key without expiry -> PTTL -1, PERSIST 0 */
    CHECK(db_set(d, "k", 1, "v", 1, DB_NO_EXPIRE) == 0);
    CHECK(db_pttl(d, "k", 1, 1000) == -1);
    CHECK(db_persist(d, "k", 1, 1000) == 0);

    /* Attach deadline at t=2000 -> PTTL is 1000 at t=1000, 1 at t=1999 */
    CHECK(db_expire_at(d, "k", 1, 2000, 1000) == 1);
    CHECK(db_pttl(d, "k", 1, 1000) == 1000);
    CHECK(db_pttl(d, "k", 1, 1999) == 1);
    CHECK(db_get(d, "k", 1, 1999, &val, &val_len) == 1);

    /* At t=2000, key lazily expires on lookup */
    CHECK(db_get(d, "k", 1, 2000, &val, &val_len) == 0);
    CHECK(db_size(d) == 0);
    CHECK(db_pttl(d, "k", 1, 2000) == -2);

    /* PERSIST clears TTL; overwriting via db_set also replaces TTL */
    CHECK(db_set(d, "p", 1, "1", 1, 5000) == 0);
    CHECK(db_pttl(d, "p", 1, 1000) == 4000);
    CHECK(db_persist(d, "p", 1, 1000) == 1);
    CHECK(db_pttl(d, "p", 1, 9000) == -1);
    CHECK(db_get(d, "p", 1, 9000, &val, &val_len) == 1);

    CHECK(db_expire_at(d, "p", 1, 10000, 9000) == 1);
    CHECK(db_set(d, "p", 1, "2", 1, DB_NO_EXPIRE) == 0);
    CHECK(db_pttl(d, "p", 1, 20000) == -1);

    /* Setting an already-past deadline via db_expire_at deletes the key immediately */
    CHECK(db_expire_at(d, "p", 1, 20000, 20000) == 1);
    CHECK(db_size(d) == 0);

    /* DEL on an expired key reclaims it and returns 0 (not live) */
    CHECK(db_set(d, "dead", 4, "x", 1, 3000) == 0);
    CHECK(db_del(d, "dead", 4, 3000) == 0);
    CHECK(db_size(d) == 0);

    /* Active expiry cycle reclaims expired keys without touching live ones */
    for (int i = 0; i < 100; i++) {
        char k[32];
        int kl = snprintf(k, sizeof k, "exp:%d", i);
        int64_t deadline = (i % 2 == 0) ? 5000 : 9000;
        CHECK(db_set(d, k, (size_t)kl, "v", 1, deadline) == 0);
    }
    CHECK(db_size(d) == 100);

    for (int tick = 0; tick < 20 && db_size(d) > 50; tick++) {
        (void)db_expire_cycle(d, 6000);
    }
    CHECK(db_size(d) == 50);

    for (int i = 0; i < 100; i++) {
        char k[32];
        int kl = snprintf(k, sizeof k, "exp:%d", i);
        CHECK(db_get(d, k, (size_t)kl, 6000, &val, &val_len) == (i % 2 != 0));
    }

    db_destroy(d);
    printf("ok:   db lazy + active expiry, pttl, expire_at, persist\n");
}

static void test_resize_many_keys(void)
{
    struct db *d = db_create();
    const int n = 2000; /* triggers multiple table doublings from 64 -> 2048 */

    CHECK(d != NULL);

    for (int i = 0; i < n; i++) {
        char k[32];
        char v[32];
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int vl = snprintf(v, sizeof v, "val:%d", i * 7);

        CHECK(db_set(d, k, (size_t)kl, v, (size_t)vl, DB_NO_EXPIRE) == 0);
    }
    CHECK(db_size(d) == (size_t)n);

    for (int i = 0; i < n; i++) {
        char k[32];
        char want[32];
        const char *got = NULL;
        size_t got_len = 0;
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int wl = snprintf(want, sizeof want, "val:%d", i * 7);

        CHECK(db_get(d, k, (size_t)kl, 0, &got, &got_len) == 1);
        CHECK(got_len == (size_t)wl && memcmp(got, want, got_len) == 0);
    }

    /* Delete all even keys and verify odd keys remain intact. */
    for (int i = 0; i < n; i += 2) {
        char k[32];
        int kl = snprintf(k, sizeof k, "key:%d", i);
        CHECK(db_del(d, k, (size_t)kl, 0) == 1);
    }
    CHECK(db_size(d) == (size_t)(n / 2));

    for (int i = 0; i < n; i++) {
        char k[32];
        const char *got = NULL;
        size_t got_len = 0;
        int kl = snprintf(k, sizeof k, "key:%d", i);

        CHECK(db_get(d, k, (size_t)kl, 0, &got, &got_len) == (i % 2 != 0));
    }

    db_destroy(d);
    printf("ok:   db resize across 2000 keys + delete half\n");
}

int main(void)
{
    test_set_get_overwrite_del();
    test_empty_and_binary_keys_and_values();
    test_incremental_rehash_operations();
    test_expiry_lazy_and_active();
    test_resize_many_keys();

    if (g_failed) {
        fprintf(stderr, "SOME DB UNIT TESTS FAILED\n");
        return 1;
    }
    printf("ALL DB UNIT TESTS PASSED\n");
    return 0;
}
