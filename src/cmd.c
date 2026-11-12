#include "cmd.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

struct cmd {
    struct db *db;
};

static int64_t now_realtime_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
}

/*
 * Case-insensitive comparison of a binary-safe slice against an uppercase ASCII
 * literal (e.g. "PING").
 */
static int cmd_name_eq(struct proto_str s, const char *upper_lit)
{
    size_t i = 0;

    while (upper_lit[i] != '\0') {
        unsigned char c;

        if (i >= s.len) {
            return 0;
        }
        c = (unsigned char)s.data[i];
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - ('a' - 'A'));
        }
        if (c != (unsigned char)upper_lit[i]) {
            return 0;
        }
        i++;
    }
    return i == s.len;
}

/*
 * Strict signed 64-bit decimal parser over a binary-safe slice.
 * Rejects empty strings, leading '+', whitespace, and trailing garbage.
 */
static int parse_i64(struct proto_str s, int64_t *out)
{
    uint64_t acc = 0;
    uint64_t limit;
    size_t i = 0;
    int neg = 0;

    if (s.len == 0) {
        return -1;
    }
    if (s.data[0] == '-') {
        neg = 1;
        i = 1;
        if (s.len == 1U) {
            return -1;
        }
    }
    limit = neg ? ((uint64_t)INT64_MAX + 1ULL) : (uint64_t)INT64_MAX;

    for (; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        uint64_t d;

        if (c < '0' || c > '9') {
            return -1;
        }
        d = (uint64_t)(c - '0');
        if (acc > (limit - d) / 10ULL) {
            return -1;
        }
        acc = acc * 10ULL + d;
    }

    if (neg) {
        if (acc == (uint64_t)INT64_MAX + 1ULL) {
            *out = INT64_MIN;
        } else {
            *out = -(int64_t)acc;
        }
    } else {
        *out = (int64_t)acc;
    }
    return 0;
}

/*
 * Converts a positive relative duration (in seconds or milliseconds) into an
 * absolute millisecond deadline at now_ms. Returns -1 if amount <= 0 or if
 * the conversion overflows int64_t.
 */
static int compute_deadline_ms(int64_t now_ms, int64_t amount, int is_seconds,
                               int64_t *deadline_out)
{
    int64_t delta_ms = amount;

    if (amount <= 0) {
        return -1;
    }
    if (is_seconds) {
        if (amount > INT64_MAX / 1000) {
            return -1;
        }
        delta_ms = amount * 1000;
    }
    if (now_ms > INT64_MAX - delta_ms) {
        return -1;
    }
    *deadline_out = now_ms + delta_ms;
    return 0;
}

struct cmd *cmd_create(struct db *db)
{
    struct cmd *c = calloc(1U, sizeof *c);

    if (c == NULL) {
        return NULL;
    }
    c->db = db;
    return c;
}

void cmd_destroy(struct cmd *c)
{
    free(c);
}

void cmd_tick(struct cmd *c)
{
    (void)db_expire_cycle(c->db, now_realtime_ms());
}

static int exec_ping(const struct proto_cmd *req, struct proto_buf *out)
{
    if (req->argc == 1U) {
        return proto_reply_simple(out, "PONG");
    }
    if (req->argc == 2U) {
        return proto_reply_bulk(out, req->argv[1].data, req->argv[1].len);
    }
    return proto_reply_error(out, "ERR wrong number of arguments for 'ping' command");
}

static int exec_echo(const struct proto_cmd *req, struct proto_buf *out)
{
    if (req->argc != 2U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'echo' command");
    }
    return proto_reply_bulk(out, req->argv[1].data, req->argv[1].len);
}

static int exec_set(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    int64_t expire_at_ms = DB_NO_EXPIRE;

    if (req->argc < 3U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'set' command");
    }
    if (req->argc == 5U) {
        int is_sec = cmd_name_eq(req->argv[3], "EX");
        int is_ms = cmd_name_eq(req->argv[3], "PX");
        int64_t amount = 0;

        if (!is_sec && !is_ms) {
            return proto_reply_error(out, "ERR syntax error");
        }
        if (parse_i64(req->argv[4], &amount) != 0) {
            return proto_reply_error(out, "ERR value is not an integer or out of range");
        }
        if (compute_deadline_ms(now_realtime_ms(), amount, is_sec, &expire_at_ms) != 0) {
            return proto_reply_error(out, "ERR invalid expire time in 'set' command");
        }
    } else if (req->argc != 3U) {
        return proto_reply_error(out, "ERR syntax error");
    }

    if (db_set(c->db, req->argv[1].data, req->argv[1].len, req->argv[2].data, req->argv[2].len,
               expire_at_ms) != 0) {
        return proto_reply_error(out, "ERR out of memory");
    }
    return proto_reply_simple(out, "OK");
}

static int exec_setex(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out,
                      int is_seconds)
{
    int64_t amount = 0;
    int64_t expire_at_ms = DB_NO_EXPIRE;

    if (req->argc != 4U) {
        return proto_reply_error(out, is_seconds
                                          ? "ERR wrong number of arguments for 'setex' command"
                                          : "ERR wrong number of arguments for 'psetex' command");
    }
    if (parse_i64(req->argv[2], &amount) != 0) {
        return proto_reply_error(out, "ERR value is not an integer or out of range");
    }
    if (compute_deadline_ms(now_realtime_ms(), amount, is_seconds, &expire_at_ms) != 0) {
        return proto_reply_error(out, is_seconds ? "ERR invalid expire time in 'setex' command"
                                                 : "ERR invalid expire time in 'psetex' command");
    }
    if (db_set(c->db, req->argv[1].data, req->argv[1].len, req->argv[3].data, req->argv[3].len,
               expire_at_ms) != 0) {
        return proto_reply_error(out, "ERR out of memory");
    }
    return proto_reply_simple(out, "OK");
}

static int exec_get(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    const char *val = NULL;
    size_t val_len = 0;

    if (req->argc != 2U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'get' command");
    }
    if (!db_get(c->db, req->argv[1].data, req->argv[1].len, now_realtime_ms(), &val, &val_len)) {
        return proto_reply_null(out);
    }
    return proto_reply_bulk(out, val, val_len);
}

static int exec_del(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    int64_t removed = 0;
    int64_t now_ms;

    if (req->argc < 2U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'del' command");
    }
    now_ms = now_realtime_ms();
    for (size_t i = 1U; i < req->argc; i++) {
        removed += db_del(c->db, req->argv[i].data, req->argv[i].len, now_ms);
    }
    return proto_reply_int(out, removed);
}

static int exec_strlen(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    const char *val = NULL;
    size_t val_len = 0;

    if (req->argc != 2U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'strlen' command");
    }
    if (!db_get(c->db, req->argv[1].data, req->argv[1].len, now_realtime_ms(), &val, &val_len)) {
        return proto_reply_int(out, 0);
    }
    return proto_reply_int(out, (int64_t)val_len);
}

static int exec_expire(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out,
                       int is_seconds)
{
    int64_t amount = 0;
    int64_t now_ms;
    int64_t expire_at_ms = 0;
    int updated;

    if (req->argc != 3U) {
        return proto_reply_error(out, is_seconds
                                          ? "ERR wrong number of arguments for 'expire' command"
                                          : "ERR wrong number of arguments for 'pexpire' command");
    }
    if (parse_i64(req->argv[2], &amount) != 0) {
        return proto_reply_error(out, "ERR value is not an integer or out of range");
    }
    now_ms = now_realtime_ms();
    if (amount > 0) {
        if (compute_deadline_ms(now_ms, amount, is_seconds, &expire_at_ms) != 0) {
            return proto_reply_error(out, is_seconds
                                              ? "ERR invalid expire time in 'expire' command"
                                              : "ERR invalid expire time in 'pexpire' command");
        }
    }
    updated = db_expire_at(c->db, req->argv[1].data, req->argv[1].len, expire_at_ms, now_ms);
    return proto_reply_int(out, updated);
}

static int exec_ttl(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out,
                    int is_seconds)
{
    int64_t pttl;

    if (req->argc != 2U) {
        return proto_reply_error(out, is_seconds
                                          ? "ERR wrong number of arguments for 'ttl' command"
                                          : "ERR wrong number of arguments for 'pttl' command");
    }
    pttl = db_pttl(c->db, req->argv[1].data, req->argv[1].len, now_realtime_ms());
    if (pttl < 0 || !is_seconds) {
        return proto_reply_int(out, pttl);
    }
    return proto_reply_int(out, (pttl + 500) / 1000);
}

static int exec_persist(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    int persisted;

    if (req->argc != 2U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'persist' command");
    }
    persisted = db_persist(c->db, req->argv[1].data, req->argv[1].len, now_realtime_ms());
    return proto_reply_int(out, persisted);
}

int cmd_exec(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    if (req->argc == 0U) {
        return proto_reply_error(out, "ERR empty command");
    }
    if (cmd_name_eq(req->argv[0], "PING")) {
        return exec_ping(req, out);
    }
    if (cmd_name_eq(req->argv[0], "ECHO")) {
        return exec_echo(req, out);
    }
    if (cmd_name_eq(req->argv[0], "SET")) {
        return exec_set(c, req, out);
    }
    if (cmd_name_eq(req->argv[0], "SETEX")) {
        return exec_setex(c, req, out, 1);
    }
    if (cmd_name_eq(req->argv[0], "PSETEX")) {
        return exec_setex(c, req, out, 0);
    }
    if (cmd_name_eq(req->argv[0], "GET")) {
        return exec_get(c, req, out);
    }
    if (cmd_name_eq(req->argv[0], "DEL")) {
        return exec_del(c, req, out);
    }
    if (cmd_name_eq(req->argv[0], "STRLEN")) {
        return exec_strlen(c, req, out);
    }
    if (cmd_name_eq(req->argv[0], "EXPIRE")) {
        return exec_expire(c, req, out, 1);
    }
    if (cmd_name_eq(req->argv[0], "PEXPIRE")) {
        return exec_expire(c, req, out, 0);
    }
    if (cmd_name_eq(req->argv[0], "TTL")) {
        return exec_ttl(c, req, out, 1);
    }
    if (cmd_name_eq(req->argv[0], "PTTL")) {
        return exec_ttl(c, req, out, 0);
    }
    if (cmd_name_eq(req->argv[0], "PERSIST")) {
        return exec_persist(c, req, out);
    }
    return proto_reply_error(out, "ERR unknown command");
}
