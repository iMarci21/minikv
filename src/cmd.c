#include "cmd.h"

#include <stdint.h>
#include <stdlib.h>

struct cmd {
    struct db *db;
};

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
    if (req->argc != 3U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'set' command");
    }
    if (db_set(c->db, req->argv[1].data, req->argv[1].len, req->argv[2].data, req->argv[2].len) !=
        0) {
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
    if (!db_get(c->db, req->argv[1].data, req->argv[1].len, &val, &val_len)) {
        return proto_reply_null(out);
    }
    return proto_reply_bulk(out, val, val_len);
}

static int exec_del(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    int64_t removed = 0;

    if (req->argc < 2U) {
        return proto_reply_error(out, "ERR wrong number of arguments for 'del' command");
    }
    for (size_t i = 1U; i < req->argc; i++) {
        removed += db_del(c->db, req->argv[i].data, req->argv[i].len);
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
    if (!db_get(c->db, req->argv[1].data, req->argv[1].len, &val, &val_len)) {
        return proto_reply_int(out, 0);
    }
    return proto_reply_int(out, (int64_t)val_len);
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
    if (cmd_name_eq(req->argv[0], "GET")) {
        return exec_get(c, req, out);
    }
    if (cmd_name_eq(req->argv[0], "DEL")) {
        return exec_del(c, req, out);
    }
    if (cmd_name_eq(req->argv[0], "STRLEN")) {
        return exec_strlen(c, req, out);
    }
    return proto_reply_error(out, "ERR unknown command");
}
