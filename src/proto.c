#include "proto.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTO_BUF_INIT_CAP 64U
#define PROTO_MAX_INLINE_LINE 65536U

__attribute__((format(printf, 3, 4))) static void set_err(char *err, size_t err_len,
                                                          const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || err_len == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/*
 * Scans a CRLF-terminated line starting at buf[*pos .. len).
 *
 * Returns:
 *   PROTO_OK         : found "\r\n"; *line_start..*line_end is the line content
 *                      (excluding "\r\n") and *pos is advanced past "\r\n".
 *   PROTO_INCOMPLETE : no '\r' or '\n' seen yet (or buffer ends right after '\r').
 *   PROTO_ERROR      : bare '\r' (not followed by '\n') or bare '\n' inside the line.
 */
static enum proto_result read_line(const char *buf, size_t len, size_t *pos, size_t *line_start,
                                   size_t *line_end, char *err, size_t err_len)
{
    size_t i = *pos;

    while (i < len) {
        if (buf[i] == '\r') {
            if (i + 1 >= len) {
                return PROTO_INCOMPLETE;
            }
            if (buf[i + 1] != '\n') {
                set_err(err, err_len, "expected LF after CR in header");
                return PROTO_ERROR;
            }
            *line_start = *pos;
            *line_end = i;
            *pos = i + 2;
            return PROTO_OK;
        }
        if (buf[i] == '\n') {
            set_err(err, err_len, "unexpected bare LF in header");
            return PROTO_ERROR;
        }
        i++;
    }
    return PROTO_INCOMPLETE;
}

/*
 * Parses an unsigned decimal integer from buf[start .. end) with overflow check.
 * Rejects empty strings, signs, whitespace, and non-digits. Returns 0 or -1 on fail.
 */
static int parse_u64(const char *buf, size_t start, size_t end, uint64_t *out)
{
    uint64_t v = 0;

    if (start >= end) {
        return -1;
    }
    for (size_t i = start; i < end; i++) {
        unsigned char c = (unsigned char)buf[i];
        uint64_t d;

        if (c < '0' || c > '9') {
            return -1;
        }
        d = (uint64_t)(c - '0');
        if (v > (UINT64_MAX - d) / 10U) {
            return -1;
        }
        v = v * 10U + d;
    }
    *out = v;
    return 0;
}

void proto_cmd_reset(struct proto_cmd *cmd)
{
    if (cmd == NULL) {
        return;
    }
    if (cmd->argv != NULL && cmd->argv != cmd->inline_argv) {
        free(cmd->argv);
    }
    memset(cmd, 0, sizeof *cmd);
}

static int init_cmd_argv(struct proto_cmd *cmd, size_t count, char *err, size_t err_len)
{
    struct proto_str *argv;

    if (count <= PROTO_INLINE_ARGS) {
        argv = cmd->inline_argv;
    } else {
        argv = calloc(count, sizeof *argv);
        if (argv == NULL) {
            set_err(err, err_len, "out of memory");
            return -1;
        }
    }
    cmd->argv = argv;
    cmd->argc = count;
    return 0;
}

/*
 * Parses a space-separated inline command line (e.g. "PING\r\n" or "ECHO hi\r\n")
 * used by redis-benchmark (PING_INLINE) and manual nc/telnet sessions.
 */
static enum proto_result parse_inline(const char *buf, size_t len, struct proto_cmd *cmd,
                                      size_t *consumed, char *err, size_t err_len)
{
    size_t pos = 0;
    size_t line_start = 0;
    size_t line_end = 0;
    enum proto_result r;
    size_t count = 0;
    size_t i;
    size_t idx = 0;

    r = read_line(buf, len, &pos, &line_start, &line_end, err, err_len);
    if (r == PROTO_INCOMPLETE) {
        if (len > PROTO_MAX_INLINE_LINE) {
            set_err(err, err_len, "inline command line too long");
            return PROTO_ERROR;
        }
        return PROTO_INCOMPLETE;
    }
    if (r != PROTO_OK) {
        return r;
    }
    if (line_end - line_start > PROTO_MAX_INLINE_LINE) {
        set_err(err, err_len, "inline command line too long");
        return PROTO_ERROR;
    }

    i = line_start;
    while (i < line_end) {
        while (i < line_end && (buf[i] == ' ' || buf[i] == '\t')) {
            i++;
        }
        if (i >= line_end) {
            break;
        }
        count++;
        while (i < line_end && buf[i] != ' ' && buf[i] != '\t') {
            i++;
        }
    }

    if (count == 0 || count > PROTO_MAX_ARGS) {
        set_err(err, err_len, "invalid inline command");
        return PROTO_ERROR;
    }
    if (init_cmd_argv(cmd, count, err, err_len) != 0) {
        return PROTO_ERROR;
    }

    i = line_start;
    while (i < line_end) {
        size_t tok_start;

        while (i < line_end && (buf[i] == ' ' || buf[i] == '\t')) {
            i++;
        }
        if (i >= line_end) {
            break;
        }
        tok_start = i;
        while (i < line_end && buf[i] != ' ' && buf[i] != '\t') {
            i++;
        }
        cmd->argv[idx].data = buf + tok_start;
        cmd->argv[idx].len = i - tok_start;
        idx++;
    }

    *consumed = pos;
    return PROTO_OK;
}

enum proto_result proto_parse(const char *buf, size_t len, struct proto_cmd *cmd, size_t *consumed,
                              char *err, size_t err_len)
{
    size_t pos = 0;
    size_t line_start = 0;
    size_t line_end = 0;
    enum proto_result r;
    uint64_t count = 0;

    memset(cmd, 0, sizeof *cmd);
    *consumed = 0;
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }

    if (len == 0) {
        return PROTO_INCOMPLETE;
    }

    /* Non-array lines that start with a valid command byte are parsed as inline commands. */
    if (buf[0] != '*') {
        if (buf[0] == '+' || buf[0] == '-' || buf[0] == ':' || buf[0] == '$') {
            set_err(err, err_len, "expected '*' at start of command");
            return PROTO_ERROR;
        }
        return parse_inline(buf, len, cmd, consumed, err, err_len);
    }
    pos = 1;

    r = read_line(buf, len, &pos, &line_start, &line_end, err, err_len);
    if (r != PROTO_OK) {
        return r;
    }
    if (parse_u64(buf, line_start, line_end, &count) != 0 || count == 0 || count > PROTO_MAX_ARGS) {
        set_err(err, err_len, "invalid multibulk length");
        return PROTO_ERROR;
    }

    if (init_cmd_argv(cmd, (size_t)count, err, err_len) != 0) {
        return PROTO_ERROR;
    }

    for (size_t i = 0; i < (size_t)count; i++) {
        uint64_t bulk_len = 0;
        size_t b_len;

        if (pos >= len) {
            proto_cmd_reset(cmd);
            return PROTO_INCOMPLETE;
        }
        if (buf[pos] != '$') {
            set_err(err, err_len, "expected '$' for bulk string argument");
            proto_cmd_reset(cmd);
            return PROTO_ERROR;
        }
        pos++;

        r = read_line(buf, len, &pos, &line_start, &line_end, err, err_len);
        if (r != PROTO_OK) {
            proto_cmd_reset(cmd);
            return r;
        }
        /*
         * Check the bulk length against PROTO_MAX_BULK before checking whether
         * the payload bytes have arrived, so "$999999999\r\n" is rejected
         * immediately instead of buffering all data first.
         */
        if (parse_u64(buf, line_start, line_end, &bulk_len) != 0 || bulk_len > PROTO_MAX_BULK) {
            set_err(err, err_len, "invalid bulk length");
            proto_cmd_reset(cmd);
            return PROTO_ERROR;
        }
        b_len = (size_t)bulk_len;

        /* Need b_len payload bytes plus trailing "\r\n". b_len <= 64 MiB so b_len + 2 cannot
         * overflow. */
        if (len - pos < b_len + 2U) {
            proto_cmd_reset(cmd);
            return PROTO_INCOMPLETE;
        }
        if (buf[pos + b_len] != '\r' || buf[pos + b_len + 1U] != '\n') {
            set_err(err, err_len, "missing CRLF after bulk string payload");
            proto_cmd_reset(cmd);
            return PROTO_ERROR;
        }

        cmd->argv[i].data = buf + pos;
        cmd->argv[i].len = b_len;
        pos += b_len + 2U;
    }

    *consumed = pos;
    return PROTO_OK;
}

void proto_buf_init(struct proto_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void proto_buf_destroy(struct proto_buf *b)
{
    if (b == NULL) {
        return;
    }
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void proto_buf_clear(struct proto_buf *b)
{
    b->len = 0;
}

int proto_buf_reserve(struct proto_buf *b, size_t avail)
{
    size_t needed;
    size_t new_cap;
    char *p;

    if (avail > SIZE_MAX - b->len) {
        return -1;
    }
    needed = b->len + avail;
    if (needed <= b->cap) {
        return 0;
    }

    new_cap = (b->cap == 0) ? PROTO_BUF_INIT_CAP : b->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2U) {
            new_cap = needed;
            break;
        }
        new_cap *= 2U;
    }

    p = realloc(b->data, new_cap);
    if (p == NULL) {
        return -1;
    }
    b->data = p;
    b->cap = new_cap;
    return 0;
}

int proto_buf_append(struct proto_buf *b, const void *src, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (proto_buf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

/*
 * Appends a single-line reply "<prefix><s>\r\n" to b. Rejects strings containing
 * '\r' or '\n' so a message can never inject a second fake reply line.
 */
static int reply_line(struct proto_buf *b, char prefix, const char *s)
{
    size_t n;
    size_t saved;

    if (s == NULL) {
        return -1;
    }
    n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r' || s[i] == '\n') {
            return -1;
        }
    }

    saved = b->len;
    if (proto_buf_append(b, &prefix, 1U) != 0 || proto_buf_append(b, s, n) != 0 ||
        proto_buf_append(b, "\r\n", 2U) != 0) {
        b->len = saved;
        return -1;
    }
    return 0;
}

int proto_reply_simple(struct proto_buf *b, const char *s)
{
    return reply_line(b, '+', s);
}

int proto_reply_error(struct proto_buf *b, const char *msg)
{
    return reply_line(b, '-', msg);
}

int proto_reply_int(struct proto_buf *b, int64_t v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, ":%" PRId64 "\r\n", v);

    if (n < 0 || (size_t)n >= sizeof tmp) {
        return -1;
    }
    return proto_buf_append(b, tmp, (size_t)n);
}

int proto_reply_null(struct proto_buf *b)
{
    return proto_buf_append(b, "$-1\r\n", 5U);
}

int proto_reply_bulk(struct proto_buf *b, const char *data, size_t len)
{
    char hdr[32];
    int hdr_len;
    size_t saved;

    if (len > 0 && data == NULL) {
        return -1;
    }
    hdr_len = snprintf(hdr, sizeof hdr, "$%zu\r\n", len);
    if (hdr_len < 0 || (size_t)hdr_len >= sizeof hdr) {
        return -1;
    }

    saved = b->len;
    if (proto_buf_append(b, hdr, (size_t)hdr_len) != 0 || proto_buf_append(b, data, len) != 0 ||
        proto_buf_append(b, "\r\n", 2U) != 0) {
        b->len = saved;
        return -1;
    }
    return 0;
}
