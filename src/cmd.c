#include "cmd.h"

#include <stdlib.h>

struct cmd {
    /* Placeholder for struct db * and struct wal * in steps 4 and 7. */
    int unused;
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

struct cmd *cmd_create(void)
{
    struct cmd *c = calloc(1U, sizeof *c);

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

int cmd_exec(struct cmd *c, const struct proto_cmd *req, struct proto_buf *out)
{
    (void)c;

    if (req->argc == 0U) {
        return proto_reply_error(out, "ERR empty command");
    }
    if (cmd_name_eq(req->argv[0], "PING")) {
        return exec_ping(req, out);
    }
    if (cmd_name_eq(req->argv[0], "ECHO")) {
        return exec_echo(req, out);
    }
    return proto_reply_error(out, "ERR unknown command");
}
