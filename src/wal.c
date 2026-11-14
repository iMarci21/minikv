#include "wal.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WAL_MAGIC "MKVWAL"
#define WAL_MAGIC_LEN 6U
#define WAL_VERSION 1U
#define WAL_HEADER_LEN 8U
#define WAL_RECORD_HDR_LEN 8U
#define WAL_MAX_PAYLOAD (128U * 1024U * 1024U)
#define WAL_SLOW_DISK_LIMIT_MS 2000

struct wal {
    int fd;
    enum wal_fsync policy;
    off_t synced_offset;

    uint8_t *buf;
    size_t buf_len;
    size_t buf_cap;

    int write_failed;
    int fsync_failed;
    int dirty_since_fsync;
    int64_t last_fsync_tick_ms;
    int64_t postponed_since_ms;

    int thread_started;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int fsync_requested;
    int fsync_in_progress;
    int fsync_error;
    int shutdown;
};

static int64_t now_monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

static void put_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8);
}

static void put_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFU);
    dst[1] = (uint8_t)((v >> 8) & 0xFFU);
    dst[2] = (uint8_t)((v >> 16) & 0xFFU);
    dst[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void put_u64_le(uint8_t *dst, uint64_t v)
{
    for (size_t i = 0; i < 8U; i++) {
        dst[i] = (uint8_t)((v >> (i * 8U)) & 0xFFU);
    }
}

static uint64_t get_u64_le(const uint8_t *src)
{
    uint64_t v = 0;

    for (size_t i = 0; i < 8U; i++) {
        v |= ((uint64_t)src[i]) << (i * 8U);
    }
    return v;
}

uint32_t wal_crc32c(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t nw = write(fd, data + off, len - off);

        if (nw > 0) {
            off += (size_t)nw;
            continue;
        }
        if (nw < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static ssize_t read_full(int fd, uint8_t *dst, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t nr = read(fd, dst + off, len - off);

        if (nr > 0) {
            off += (size_t)nr;
            continue;
        }
        if (nr == 0) {
            return (ssize_t)off;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return (ssize_t)off;
}

/*
 * Fsyncs the parent directory of path so a newly created WAL directory entry
 * survives a power failure.
 */
static int fsync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir_path = NULL;
    const char *target = ".";
    int dfd;

    if (slash != NULL) {
        size_t dir_len = (slash == path) ? 1U : (size_t)(slash - path);

        dir_path = malloc(dir_len + 1U);
        if (dir_path == NULL) {
            return -1;
        }
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';
        target = dir_path;
    }

    dfd = open(target, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(dir_path);
    if (dfd < 0) {
        log_error("open(parent dir of %s): %s", path, strerror(errno));
        return -1;
    }
    if (fsync(dfd) != 0) {
        log_error("fsync(parent dir of %s): %s", path, strerror(errno));
        (void)close(dfd);
        return -1;
    }
    (void)close(dfd);
    return 0;
}

static int decode_payload(const uint8_t *payload, size_t len, struct wal_record *out)
{
    uint8_t op;
    uint32_t klen;

    if (len < 1U) {
        return -1;
    }
    op = payload[0];
    memset(out, 0, sizeof *out);

    switch (op) {
    case WAL_OP_SET: {
        uint32_t vlen;
        size_t pos = 1U;

        if (len < 1U + 8U + 4U) {
            return -1;
        }
        out->op = WAL_OP_SET;
        out->expire_at_ms = (int64_t)get_u64_le(payload + pos);
        pos += 8U;
        klen = get_u32_le(payload + pos);
        pos += 4U;
        if (klen > len - pos || len - pos - klen < 4U) {
            return -1;
        }
        out->key = (const char *)(payload + pos);
        out->key_len = klen;
        pos += klen;
        vlen = get_u32_le(payload + pos);
        pos += 4U;
        if (vlen != len - pos) {
            return -1;
        }
        out->val = (const char *)(payload + pos);
        out->val_len = vlen;
        return 0;
    }
    case WAL_OP_DEL:
    case WAL_OP_PERSIST: {
        if (len < 1U + 4U) {
            return -1;
        }
        out->op = (enum wal_op)op;
        klen = get_u32_le(payload + 1U);
        if (klen != len - 5U) {
            return -1;
        }
        out->key = (const char *)(payload + 5U);
        out->key_len = klen;
        return 0;
    }
    case WAL_OP_EXPIRE: {
        if (len < 1U + 8U + 4U) {
            return -1;
        }
        out->op = WAL_OP_EXPIRE;
        out->expire_at_ms = (int64_t)get_u64_le(payload + 1U);
        klen = get_u32_le(payload + 9U);
        if (klen != len - 13U) {
            return -1;
        }
        out->key = (const char *)(payload + 13U);
        out->key_len = klen;
        return 0;
    }
    default:
        return -1;
    }
}

static int truncate_corrupt_tail(struct wal *w, const char *path, off_t valid_offset,
                                 const char *reason)
{
    log_warn("wal %s: %s at offset %lld, truncating tail", path, reason, (long long)valid_offset);
    if (ftruncate(w->fd, valid_offset) != 0) {
        log_error("ftruncate(%s): %s", path, strerror(errno));
        return -1;
    }
    if (lseek(w->fd, valid_offset, SEEK_SET) < 0) {
        log_error("lseek(%s): %s", path, strerror(errno));
        return -1;
    }
    w->synced_offset = valid_offset;
    return 0;
}

static int replay_existing(struct wal *w, const char *path, wal_replay_fn apply_fn, void *ctx)
{
    uint8_t hdr[WAL_HEADER_LEN];
    ssize_t nr;
    off_t valid_offset = (off_t)WAL_HEADER_LEN;
    uint8_t *payload = NULL;
    size_t payload_cap = 0;

    if (lseek(w->fd, 0, SEEK_SET) < 0) {
        log_error("lseek(%s): %s", path, strerror(errno));
        return -1;
    }

    nr = read_full(w->fd, hdr, WAL_HEADER_LEN);
    if (nr < 0) {
        log_error("read(%s header): %s", path, strerror(errno));
        return -1;
    }
    if ((size_t)nr < WAL_HEADER_LEN || memcmp(hdr, WAL_MAGIC, WAL_MAGIC_LEN) != 0 ||
        get_u16_le(hdr + WAL_MAGIC_LEN) != WAL_VERSION) {
        log_error("wal %s: invalid file header or unsupported version", path);
        return -1;
    }

    for (;;) {
        uint8_t rec_hdr[WAL_RECORD_HDR_LEN];
        uint32_t len;
        uint32_t want_crc;
        uint32_t got_crc;
        struct wal_record rec;

        nr = read_full(w->fd, rec_hdr, WAL_RECORD_HDR_LEN);
        if (nr < 0) {
            log_error("read(%s): %s", path, strerror(errno));
            free(payload);
            return -1;
        }
        if (nr == 0) {
            /* Clean EOF at record boundary. */
            break;
        }
        if ((size_t)nr < WAL_RECORD_HDR_LEN) {
            free(payload);
            return truncate_corrupt_tail(w, path, valid_offset, "truncated record header");
        }

        len = get_u32_le(rec_hdr);
        want_crc = get_u32_le(rec_hdr + 4U);
        if (len == 0 || len > WAL_MAX_PAYLOAD) {
            free(payload);
            return truncate_corrupt_tail(w, path, valid_offset, "invalid record length");
        }

        if (len > payload_cap) {
            uint8_t *tmp = realloc(payload, len);
            if (tmp == NULL) {
                log_error("wal replay realloc: out of memory");
                free(payload);
                return -1;
            }
            payload = tmp;
            payload_cap = len;
        }

        nr = read_full(w->fd, payload, len);
        if (nr < 0) {
            log_error("read(%s payload): %s", path, strerror(errno));
            free(payload);
            return -1;
        }
        if ((size_t)nr < len) {
            free(payload);
            return truncate_corrupt_tail(w, path, valid_offset, "truncated record payload");
        }

        got_crc = wal_crc32c(payload, len);
        if (got_crc != want_crc) {
            free(payload);
            return truncate_corrupt_tail(w, path, valid_offset, "crc32c mismatch");
        }

        if (decode_payload(payload, len, &rec) != 0) {
            free(payload);
            return truncate_corrupt_tail(w, path, valid_offset, "malformed record payload");
        }

        if (apply_fn != NULL && apply_fn(&rec, ctx) != 0) {
            log_error("wal %s: failed applying record at offset %lld", path,
                      (long long)valid_offset);
            free(payload);
            return -1;
        }

        valid_offset += (off_t)WAL_RECORD_HDR_LEN + (off_t)len;
    }

    free(payload);
    w->synced_offset = valid_offset;
    return 0;
}

static void *wal_fsync_thread(void *arg)
{
    struct wal *w = arg;

    pthread_mutex_lock(&w->mu);
    for (;;) {
        while (!w->fsync_requested && !w->shutdown) {
            pthread_cond_wait(&w->cv, &w->mu);
        }
        if (w->fsync_requested) {
            int rc;
            int err = 0;

            w->fsync_requested = 0;
            w->fsync_in_progress = 1;
            pthread_mutex_unlock(&w->mu);

            rc = fdatasync(w->fd);
            if (rc != 0) {
                err = errno;
            }

            pthread_mutex_lock(&w->mu);
            w->fsync_in_progress = 0;
            if (rc != 0 && w->fsync_error == 0) {
                w->fsync_error = err;
            }
        }
        if (w->shutdown && !w->fsync_requested) {
            break;
        }
    }
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

struct wal *wal_open(const char *path, enum wal_fsync policy, wal_replay_fn apply_fn, void *ctx)
{
    struct wal *w;
    struct stat st;
    int existed;

    w = calloc(1U, sizeof *w);
    if (w == NULL) {
        log_error("calloc(wal): out of memory");
        return NULL;
    }
    w->fd = -1;
    w->policy = policy;
    w->last_fsync_tick_ms = now_monotonic_ms();

    existed = (stat(path, &st) == 0 && st.st_size > 0);
    w->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (w->fd < 0) {
        log_error("open(%s): %s", path, strerror(errno));
        free(w);
        return NULL;
    }

    if (!existed) {
        uint8_t hdr[WAL_HEADER_LEN];

        memcpy(hdr, WAL_MAGIC, WAL_MAGIC_LEN);
        put_u16_le(hdr + WAL_MAGIC_LEN, WAL_VERSION);
        if (write_all(w->fd, hdr, WAL_HEADER_LEN) != 0) {
            log_error("write(%s header): %s", path, strerror(errno));
            (void)close(w->fd);
            free(w);
            return NULL;
        }
        if (fdatasync(w->fd) != 0) {
            log_error("fdatasync(%s header): %s", path, strerror(errno));
            (void)close(w->fd);
            free(w);
            return NULL;
        }
        if (fsync_parent_dir(path) != 0) {
            (void)close(w->fd);
            free(w);
            return NULL;
        }
        w->synced_offset = (off_t)WAL_HEADER_LEN;
    } else {
        if (replay_existing(w, path, apply_fn, ctx) != 0) {
            (void)close(w->fd);
            free(w);
            return NULL;
        }
    }

    if (policy == WAL_FSYNC_EVERYSEC) {
        if (pthread_mutex_init(&w->mu, NULL) != 0) {
            log_error("pthread_mutex_init: out of memory");
            (void)close(w->fd);
            free(w);
            return NULL;
        }
        if (pthread_cond_init(&w->cv, NULL) != 0) {
            log_error("pthread_cond_init: out of memory");
            pthread_mutex_destroy(&w->mu);
            (void)close(w->fd);
            free(w);
            return NULL;
        }
        if (pthread_create(&w->thread, NULL, wal_fsync_thread, w) != 0) {
            log_error("pthread_create(wal_fsync): %s", strerror(errno));
            pthread_cond_destroy(&w->cv);
            pthread_mutex_destroy(&w->mu);
            (void)close(w->fd);
            free(w);
            return NULL;
        }
        w->thread_started = 1;
    }

    return w;
}

static void check_bg_fsync_error(struct wal *w)
{
    if (!w->thread_started || w->fsync_failed) {
        return;
    }
    pthread_mutex_lock(&w->mu);
    if (w->fsync_error != 0) {
        int err = w->fsync_error;
        w->fsync_failed = 1;
        pthread_mutex_unlock(&w->mu);
        log_error("wal background fdatasync failed (%s); entering permanent read-only mode",
                  strerror(err));
        return;
    }
    pthread_mutex_unlock(&w->mu);
}

int wal_is_readonly(struct wal *w)
{
    if (w == NULL) {
        return 0;
    }
    check_bg_fsync_error(w);
    return w->write_failed || w->fsync_failed;
}

static int buf_reserve(struct wal *w, size_t need)
{
    size_t min_cap;
    size_t new_cap;
    uint8_t *tmp;

    if (need > SIZE_MAX - w->buf_len) {
        return -1;
    }
    min_cap = w->buf_len + need;
    if (min_cap <= w->buf_cap) {
        return 0;
    }

    new_cap = w->buf_cap == 0 ? 4096U : w->buf_cap;
    while (new_cap < min_cap) {
        if (new_cap > SIZE_MAX / 2U) {
            new_cap = min_cap;
            break;
        }
        new_cap *= 2U;
    }

    tmp = realloc(w->buf, new_cap);
    if (tmp == NULL) {
        return -1;
    }
    w->buf = tmp;
    w->buf_cap = new_cap;
    return 0;
}

static void finalize_record_frame(uint8_t *frame, uint32_t payload_len)
{
    uint32_t crc = wal_crc32c(frame + WAL_RECORD_HDR_LEN, payload_len);

    put_u32_le(frame, payload_len);
    put_u32_le(frame + 4U, crc);
}

int wal_append_set(struct wal *w, const char *key, size_t key_len, const char *val, size_t val_len,
                   int64_t expire_at_ms)
{
    size_t payload_len;
    uint8_t *frame;
    uint8_t *p;

    if (wal_is_readonly(w)) {
        return -1;
    }
    if (key_len > UINT32_MAX || val_len > UINT32_MAX) {
        return -1;
    }
    payload_len = 1U + 8U + 4U + key_len + 4U + val_len;
    if (payload_len > WAL_MAX_PAYLOAD) {
        return -1;
    }
    if (buf_reserve(w, WAL_RECORD_HDR_LEN + payload_len) != 0) {
        return -1;
    }

    frame = w->buf + w->buf_len;
    p = frame + WAL_RECORD_HDR_LEN;
    *p++ = (uint8_t)WAL_OP_SET;
    put_u64_le(p, (uint64_t)expire_at_ms);
    p += 8U;
    put_u32_le(p, (uint32_t)key_len);
    p += 4U;
    if (key_len > 0) {
        memcpy(p, key, key_len);
        p += key_len;
    }
    put_u32_le(p, (uint32_t)val_len);
    p += 4U;
    if (val_len > 0) {
        memcpy(p, val, val_len);
    }

    finalize_record_frame(frame, (uint32_t)payload_len);
    w->buf_len += WAL_RECORD_HDR_LEN + payload_len;
    return 0;
}

static int append_key_only(struct wal *w, enum wal_op op, const char *key, size_t key_len)
{
    size_t payload_len;
    uint8_t *frame;
    uint8_t *p;

    if (wal_is_readonly(w)) {
        return -1;
    }
    if (key_len > UINT32_MAX) {
        return -1;
    }
    payload_len = 1U + 4U + key_len;
    if (payload_len > WAL_MAX_PAYLOAD) {
        return -1;
    }
    if (buf_reserve(w, WAL_RECORD_HDR_LEN + payload_len) != 0) {
        return -1;
    }

    frame = w->buf + w->buf_len;
    p = frame + WAL_RECORD_HDR_LEN;
    *p++ = (uint8_t)op;
    put_u32_le(p, (uint32_t)key_len);
    p += 4U;
    if (key_len > 0) {
        memcpy(p, key, key_len);
    }

    finalize_record_frame(frame, (uint32_t)payload_len);
    w->buf_len += WAL_RECORD_HDR_LEN + payload_len;
    return 0;
}

int wal_append_del(struct wal *w, const char *key, size_t key_len)
{
    return append_key_only(w, WAL_OP_DEL, key, key_len);
}

int wal_append_persist(struct wal *w, const char *key, size_t key_len)
{
    return append_key_only(w, WAL_OP_PERSIST, key, key_len);
}

int wal_append_expire(struct wal *w, const char *key, size_t key_len, int64_t expire_at_ms)
{
    size_t payload_len;
    uint8_t *frame;
    uint8_t *p;

    if (wal_is_readonly(w)) {
        return -1;
    }
    if (key_len > UINT32_MAX) {
        return -1;
    }
    payload_len = 1U + 8U + 4U + key_len;
    if (payload_len > WAL_MAX_PAYLOAD) {
        return -1;
    }
    if (buf_reserve(w, WAL_RECORD_HDR_LEN + payload_len) != 0) {
        return -1;
    }

    frame = w->buf + w->buf_len;
    p = frame + WAL_RECORD_HDR_LEN;
    *p++ = (uint8_t)WAL_OP_EXPIRE;
    put_u64_le(p, (uint64_t)expire_at_ms);
    p += 8U;
    put_u32_le(p, (uint32_t)key_len);
    p += 4U;
    if (key_len > 0) {
        memcpy(p, key, key_len);
    }

    finalize_record_frame(frame, (uint32_t)payload_len);
    w->buf_len += WAL_RECORD_HDR_LEN + payload_len;
    return 0;
}

int wal_flush(struct wal *w)
{
    if (w == NULL) {
        return 0;
    }
    check_bg_fsync_error(w);
    if (w->buf_len == 0 || w->fsync_failed) {
        return 0;
    }

    /*
     * Slow-disk guard (everysec): if a background fdatasync() is currently in
     * flight, write() on the same fd can block on some filesystems. Defer the
     * write() for up to 2 seconds while keeping records in w->buf.
     */
    if (w->policy == WAL_FSYNC_EVERYSEC && w->thread_started) {
        int in_flight;

        pthread_mutex_lock(&w->mu);
        in_flight = w->fsync_in_progress;
        pthread_mutex_unlock(&w->mu);

        if (in_flight) {
            int64_t now = now_monotonic_ms();
            if (w->postponed_since_ms == 0) {
                w->postponed_since_ms = now;
            }
            if (now - w->postponed_since_ms < WAL_SLOW_DISK_LIMIT_MS) {
                return 0;
            }
        } else {
            w->postponed_since_ms = 0;
        }
    }

    if (write_all(w->fd, w->buf, w->buf_len) != 0) {
        int err = errno;
        log_error("wal write failed (%s); truncating back to %lld and entering read-only mode",
                  strerror(err), (long long)w->synced_offset);
        (void)ftruncate(w->fd, w->synced_offset);
        (void)lseek(w->fd, w->synced_offset, SEEK_SET);
        w->write_failed = 1;
        return 0;
    }

    w->synced_offset += (off_t)w->buf_len;
    w->buf_len = 0;
    w->write_failed = 0;
    w->postponed_since_ms = 0;
    w->dirty_since_fsync = 1;

    if (w->policy == WAL_FSYNC_ALWAYS) {
        if (fdatasync(w->fd) != 0) {
            int err = errno;
            log_error("wal fdatasync(always) failed (%s); refusing batch and shutting down",
                      strerror(err));
            w->fsync_failed = 1;
            return -1;
        }
        w->dirty_since_fsync = 0;
    }
    return 0;
}

void wal_tick(struct wal *w)
{
    int64_t now;

    if (w == NULL || w->policy != WAL_FSYNC_EVERYSEC || !w->thread_started) {
        return;
    }
    if (w->buf_len > 0) {
        (void)wal_flush(w);
    }
    check_bg_fsync_error(w);
    if (w->fsync_failed || !w->dirty_since_fsync) {
        return;
    }

    now = now_monotonic_ms();
    if (now - w->last_fsync_tick_ms < 1000) {
        return;
    }

    pthread_mutex_lock(&w->mu);
    if (!w->fsync_in_progress && w->fsync_error == 0) {
        w->fsync_requested = 1;
        w->dirty_since_fsync = 0;
        w->last_fsync_tick_ms = now;
        pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&w->mu);
}

void wal_close(struct wal *w)
{
    if (w == NULL) {
        return;
    }

    if (w->buf_len > 0 && !w->fsync_failed) {
        if (write_all(w->fd, w->buf, w->buf_len) == 0) {
            w->synced_offset += (off_t)w->buf_len;
            w->buf_len = 0;
        } else {
            (void)ftruncate(w->fd, w->synced_offset);
        }
    }

    if (w->thread_started) {
        pthread_mutex_lock(&w->mu);
        w->shutdown = 1;
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);

        (void)pthread_join(w->thread, NULL);
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
    }

    if (w->fd >= 0) {
        if (!w->fsync_failed && fdatasync(w->fd) != 0) {
            log_error("wal final fdatasync: %s", strerror(errno));
        }
        (void)close(w->fd);
    }
    free(w->buf);
    free(w);
}
