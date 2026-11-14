#include "cmd.h"
#include "db.h"
#include "wal.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failed;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
            g_failed = 1;                                                                          \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void make_temp_wal_path(char *buf, size_t buf_len)
{
    int fd;

    (void)snprintf(buf, buf_len, "/tmp/minikv_test_wal_XXXXXX");
    fd = mkstemp(buf);
    if (fd >= 0) {
        (void)close(fd);
        (void)unlink(buf);
    }
}

static off_t file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }
    return st.st_size;
}

static void test_crc32c_vector(void)
{
    /* Standard iSCSI / Castagnoli check value for ASCII "123456789". */
    const char *vec = "123456789";

    CHECK(wal_crc32c(vec, 9U) == 0xE3069283U);
    CHECK(wal_crc32c("", 0U) == 0x00000000U);
    printf("ok:   wal_crc32c matches standard Castagnoli test vector\n");
}

static void test_encode_flush_and_replay(void)
{
    char path[128];
    struct wal *w;
    struct db *d;
    const char *val = NULL;
    size_t val_len = 0;
    const int64_t future_ms = 4102444800000LL; /* 2100-01-01 UTC */
    const int64_t past_ms = 1000LL;            /* 1970-01-01 + 1s (already expired) */

    make_temp_wal_path(path, sizeof path);

    w = wal_open(path, WAL_FSYNC_ALWAYS, NULL, NULL);
    CHECK(w != NULL);
    CHECK(wal_is_readonly(w) == 0);

    /* Binary key/val, overwrites, DEL, EXPIRE, PERSIST, and an already-expired SET */
    CHECK(wal_append_set(w, "k1", 2U, "v1", 2U, DB_NO_EXPIRE) == 0);
    CHECK(wal_append_set(w, "k1", 2U, "v1_new", 6U, DB_NO_EXPIRE) == 0);
    CHECK(wal_append_set(w, "b\0k", 3U, "b\0v\r\n", 5U, future_ms) == 0);
    CHECK(wal_append_set(w, "gone", 4U, "temp", 4U, DB_NO_EXPIRE) == 0);
    CHECK(wal_append_del(w, "gone", 4U) == 0);
    CHECK(wal_append_set(w, "persisted", 9U, "keep", 4U, future_ms) == 0);
    CHECK(wal_append_persist(w, "persisted", 9U) == 0);
    CHECK(wal_append_set(w, "exp_key", 7U, "live", 4U, DB_NO_EXPIRE) == 0);
    CHECK(wal_append_expire(w, "exp_key", 7U, future_ms) == 0);
    CHECK(wal_append_set(w, "stale", 5U, "old", 3U, past_ms) == 0);

    CHECK(wal_flush(w) == 0);
    wal_close(w);

    /* Reopen and replay into a fresh db via cmd_apply_wal */
    d = db_create();
    CHECK(d != NULL);
    w = wal_open(path, WAL_FSYNC_NO, cmd_apply_wal, d);
    CHECK(w != NULL);

    CHECK(db_size(d) == 4U);

    CHECK(db_get(d, "k1", 2U, 2000LL, &val, &val_len) == 1);
    CHECK(val_len == 6U && memcmp(val, "v1_new", 6U) == 0);
    CHECK(db_pttl(d, "k1", 2U, 2000LL) == -1);

    CHECK(db_get(d, "b\0k", 3U, 2000LL, &val, &val_len) == 1);
    CHECK(val_len == 5U && memcmp(val, "b\0v\r\n", 5U) == 0);
    CHECK(db_pttl(d, "b\0k", 3U, 2000LL) == future_ms - 2000LL);

    CHECK(db_get(d, "gone", 4U, 2000LL, &val, &val_len) == 0);
    CHECK(db_get(d, "stale", 5U, 2000LL, &val, &val_len) == 0);

    CHECK(db_get(d, "persisted", 9U, 2000LL, &val, &val_len) == 1);
    CHECK(db_pttl(d, "persisted", 9U, 2000LL) == -1);

    CHECK(db_get(d, "exp_key", 7U, 2000LL, &val, &val_len) == 1);
    CHECK(db_pttl(d, "exp_key", 7U, 2000LL) == future_ms - 2000LL);

    wal_close(w);
    db_destroy(d);
    (void)unlink(path);
    printf("ok:   wal encode, flush, and replay (SET, DEL, EXPIRE, PERSIST, expired skip)\n");
}

static void test_corrupt_and_truncated_tail_recovery(void)
{
    char path[128];
    struct wal *w;
    struct db *d;
    off_t good_size;
    int fd;
    const char *val = NULL;
    size_t val_len = 0;

    make_temp_wal_path(path, sizeof path);

    w = wal_open(path, WAL_FSYNC_ALWAYS, NULL, NULL);
    CHECK(w != NULL);
    CHECK(wal_append_set(w, "a", 1U, "1", 1U, DB_NO_EXPIRE) == 0);
    CHECK(wal_append_set(w, "b", 1U, "2", 1U, DB_NO_EXPIRE) == 0);
    CHECK(wal_flush(w) == 0);
    wal_close(w);

    good_size = file_size(path);
    CHECK(good_size > 8);

    /* Case 1: Append a 3-byte truncated record header at the tail. */
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, "\x10\x00\x00", 3U) == 3);
    (void)close(fd);

    d = db_create();
    w = wal_open(path, WAL_FSYNC_NO, cmd_apply_wal, d);
    CHECK(w != NULL);
    CHECK(db_size(d) == 2U);
    CHECK(file_size(path) == good_size);
    wal_close(w);
    db_destroy(d);

    /* Case 2: Append a valid 8-byte record header claiming 50 bytes, but only 4 payload bytes. */
    fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, "\x32\x00\x00\x00\x11\x22\x33\x44XXXX", 12U) == 12);
    (void)close(fd);

    d = db_create();
    w = wal_open(path, WAL_FSYNC_NO, cmd_apply_wal, d);
    CHECK(w != NULL);
    CHECK(db_size(d) == 2U);
    CHECK(file_size(path) == good_size);
    wal_close(w);
    db_destroy(d);

    /* Case 3: Append a 3rd record, corrupt 1 byte in its payload -> CRC mismatch truncates it,
     * and subsequent appends after recovery still work cleanly. */
    w = wal_open(path, WAL_FSYNC_ALWAYS, NULL, NULL);
    CHECK(w != NULL);
    CHECK(wal_append_set(w, "corrupt_me", 10U, "bad", 3U, DB_NO_EXPIRE) == 0);
    CHECK(wal_flush(w) == 0);
    wal_close(w);
    CHECK(file_size(path) > good_size);

    fd = open(path, O_RDWR);
    CHECK(fd >= 0);
    CHECK(lseek(fd, -1, SEEK_END) >= 0);
    CHECK(write(fd, "Z", 1U) == 1);
    (void)close(fd);

    d = db_create();
    w = wal_open(path, WAL_FSYNC_ALWAYS, cmd_apply_wal, d);
    CHECK(w != NULL);
    CHECK(db_size(d) == 2U);
    CHECK(file_size(path) == good_size);
    CHECK(db_get(d, "corrupt_me", 10U, 0, &val, &val_len) == 0);

    /* Append a new valid record after truncation and verify it replays. */
    CHECK(wal_append_set(w, "c", 1U, "3", 1U, DB_NO_EXPIRE) == 0);
    CHECK(wal_flush(w) == 0);
    wal_close(w);
    db_destroy(d);

    d = db_create();
    w = wal_open(path, WAL_FSYNC_NO, cmd_apply_wal, d);
    CHECK(w != NULL);
    CHECK(db_size(d) == 3U);
    CHECK(db_get(d, "c", 1U, 0, &val, &val_len) == 1);
    CHECK(val_len == 1U && memcmp(val, "3", 1U) == 0);
    wal_close(w);
    db_destroy(d);

    (void)unlink(path);
    printf("ok:   wal corrupt / truncated tail recovery and ftruncate\n");
}

static void test_invalid_header_and_everysec_thread(void)
{
    char path[128];
    int fd;
    struct wal *w;
    struct db *d;

    make_temp_wal_path(path, sizeof path);

    /* Invalid magic header must be rejected on open. */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "NOTWAL\x01\x00", 8U) == 8);
    (void)close(fd);

    CHECK(wal_open(path, WAL_FSYNC_NO, NULL, NULL) == NULL);
    (void)unlink(path);

    /* Exercise WAL_FSYNC_EVERYSEC background thread lifecycle and wal_tick. */
    w = wal_open(path, WAL_FSYNC_EVERYSEC, NULL, NULL);
    CHECK(w != NULL);
    CHECK(wal_append_set(w, "k", 1U, "v", 1U, DB_NO_EXPIRE) == 0);
    CHECK(wal_flush(w) == 0);
    wal_tick(w);
    wal_close(w);

    d = db_create();
    w = wal_open(path, WAL_FSYNC_EVERYSEC, cmd_apply_wal, d);
    CHECK(w != NULL);
    CHECK(db_size(d) == 1U);
    wal_close(w);
    db_destroy(d);

    (void)unlink(path);
    printf("ok:   wal invalid header rejection and everysec fsync thread lifecycle\n");
}

int main(void)
{
    test_crc32c_vector();
    test_encode_flush_and_replay();
    test_corrupt_and_truncated_tail_recovery();
    test_invalid_header_and_everysec_thread();

    if (g_failed) {
        fprintf(stderr, "SOME WAL UNIT TESTS FAILED\n");
        return 1;
    }
    printf("ALL WAL UNIT TESTS PASSED\n");
    return 0;
}
