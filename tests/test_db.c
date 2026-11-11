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
    CHECK(db_get(d, "foo", 3, &val, &val_len) == 0);
    CHECK(db_del(d, "foo", 3) == 0);

    /* Insert */
    CHECK(db_set(d, "foo", 3, "bar", 3) == 0);
    CHECK(db_size(d) == 1);
    CHECK(db_get(d, "foo", 3, &val, &val_len) == 1);
    CHECK(val_len == 3 && memcmp(val, "bar", 3) == 0);

    /* Overwrite same key -> size stays 1, value updates */
    CHECK(db_set(d, "foo", 3, "updated_longer_value", 20) == 0);
    CHECK(db_size(d) == 1);
    CHECK(db_get(d, "foo", 3, &val, &val_len) == 1);
    CHECK(val_len == 20 && memcmp(val, "updated_longer_value", 20) == 0);

    /* Delete existing and re-delete missing */
    CHECK(db_del(d, "foo", 3) == 1);
    CHECK(db_size(d) == 0);
    CHECK(db_get(d, "foo", 3, &val, &val_len) == 0);
    CHECK(db_del(d, "foo", 3) == 0);

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
    CHECK(db_set(d, "", 0, "", 0) == 0);
    CHECK(db_get(d, "", 0, &val, &val_len) == 1);
    CHECK(val != NULL && val_len == 0);

    /* Two keys that share the same C-string prefix before an embedded NUL must be distinct! */
    CHECK(db_set(d, "a\0one", 5, "v\01", 3) == 0);
    CHECK(db_set(d, "a\0two", 5, "v\02", 3) == 0);
    CHECK(db_size(d) == 3);

    CHECK(db_get(d, "a\0one", 5, &val, &val_len) == 1);
    CHECK(val_len == 3 && memcmp(val, "v\01", 3) == 0);

    CHECK(db_get(d, "a\0two", 5, &val, &val_len) == 1);
    CHECK(val_len == 3 && memcmp(val, "v\02", 3) == 0);

    db_destroy(d);
    printf("ok:   db empty and binary keys/values with embedded NULs\n");
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

        CHECK(db_set(d, k, (size_t)kl, v, (size_t)vl) == 0);
    }
    CHECK(db_size(d) == (size_t)n);

    for (int i = 0; i < n; i++) {
        char k[32];
        char want[32];
        const char *got = NULL;
        size_t got_len = 0;
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int wl = snprintf(want, sizeof want, "val:%d", i * 7);

        CHECK(db_get(d, k, (size_t)kl, &got, &got_len) == 1);
        CHECK(got_len == (size_t)wl && memcmp(got, want, got_len) == 0);
    }

    /* Delete all even keys and verify odd keys remain intact. */
    for (int i = 0; i < n; i += 2) {
        char k[32];
        int kl = snprintf(k, sizeof k, "key:%d", i);
        CHECK(db_del(d, k, (size_t)kl) == 1);
    }
    CHECK(db_size(d) == (size_t)(n / 2));

    for (int i = 0; i < n; i++) {
        char k[32];
        const char *got = NULL;
        size_t got_len = 0;
        int kl = snprintf(k, sizeof k, "key:%d", i);

        CHECK(db_get(d, k, (size_t)kl, &got, &got_len) == (i % 2 != 0));
    }

    db_destroy(d);
    printf("ok:   db resize across 2000 keys + delete half\n");
}

int main(void)
{
    test_set_get_overwrite_del();
    test_empty_and_binary_keys_and_values();
    test_resize_many_keys();

    if (g_failed) {
        fprintf(stderr, "SOME DB UNIT TESTS FAILED\n");
        return 1;
    }
    printf("ALL DB UNIT TESTS PASSED\n");
    return 0;
}
