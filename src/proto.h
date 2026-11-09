#ifndef MINIKV_PROTO_H
#define MINIKV_PROTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * RESP  parser and reply serializer.
 *
 * Pure Module. No logging or I/O. Takes bytes and produces parsed
 * command slices or serialized bytes out.
 */

/* Hard limits on client command input. Violations return PROTO_ERROR. */
#define PROTO_MAX_ARGS 1024U
#define PROTO_MAX_BULK (64U * 1024U * 1024U) /* 64 MiB */

/* Commands up to this many arguments avoid any heap allocation. */
#define PROTO_INLINE_ARGS 8U

/*
 * Binary-safe byte slice. NOT NUL-terminated; may contain '\0', '\r', '\n'.
 * Points directly into the buffer passed to proto_parse and remains valid only
 * until the caller mutates or discards that buffer region.
 */
struct proto_str {
    const char *data;
    size_t len;
};

/*
 * Parsed client command (a flat RESP array of bulk strings).
 *
 * Ownership: argv points either to inline_argv (when argc <= PROTO_INLINE_ARGS)
 * or to a heap array owned by this struct. Always call proto_cmd_reset(&cmd)
 * when done with a PROTO_OK result so the caller never has to care which path
 * was taken.
 */
struct proto_cmd {
    struct proto_str *argv;
    size_t argc;
    struct proto_str inline_argv[PROTO_INLINE_ARGS];
};

enum proto_result {
    PROTO_OK,         /* *cmd and *consumed are filled in */
    PROTO_INCOMPLETE, /* valid prefix so far; need more bytes (*consumed = 0) */
    PROTO_ERROR,      /* malformed input or limit exceeded (*consumed = 0) */
};

/*
 * Parses a single RESP command from [buf, buf + len).
 *
 * On PROTO_OK:
 *   - cmd->argv[0 .. cmd->argc - 1] hold zero-copy slices into buf.
 *   - *consumed is the exact byte length of the command (> 0). The caller must
 *     eventually call proto_cmd_reset(cmd) and advance its input buffer by
 *     *consumed bytes.
 *
 * On PROTO_INCOMPLETE:
 *   - *consumed is 0 and *cmd is zeroed. Leave the input buffer untouched and
 *     call proto_parse again once more bytes have been read.
 *
 * On PROTO_ERROR:
 *   - *consumed is 0 and *cmd is zeroed. If err != NULL and err_len > 0, a
 *     one-line NUL-terminated reason (without "-ERR " prefix or "\r\n") is
 *     written to err.
 */
enum proto_result proto_parse(const char *buf, size_t len, struct proto_cmd *cmd, size_t *consumed,
                              char *err, size_t err_len);

/*
 * Releases any heap memory owned by cmd and zeros the struct. Safe to call on
 * a zero-initialized struct or after any proto_parse return value.
 */
void proto_cmd_reset(struct proto_cmd *cmd);

/*
 * Growable byte buffer used to assemble serialized RESP replies.
 *
 * Ownership: data is a heap buffer owned by the struct (NULL when cap = 0).
 * Free with proto_buf_destroy().
 */
struct proto_buf {
    char *data;
    size_t len;
    size_t cap;
};

/* Initializes *b to an empty buffer (no alloc until first append). */
void proto_buf_init(struct proto_buf *b);

/* Frees b->data and resets *b to empty. Safe to call. */
void proto_buf_destroy(struct proto_buf *b);

/* Resets b->len to 0 while keeping the allocated capacity for reuse. */
void proto_buf_clear(struct proto_buf *b);

/* Ensures at least avail unused bytes (b->cap - b->len >= avail). Returns 0 or -1 on OOM. */
int proto_buf_reserve(struct proto_buf *b, size_t avail);

/* Appends n raw bytes from src to b. Returns 0 on success, -1 on OOM. */
int proto_buf_append(struct proto_buf *b, const void *src, size_t n);

/*
 * Reply serializers. Each appends one complete RESP reply to b and returns 0 on
 * success or -1 on failure (OOM or invalid input such as '\r'/'\n' inside a
 * simple string or error message). On failure, b->len is left unchanged.
 *
 *   proto_reply_simple(b, "OK")       -> "+OK\r\n"
 *   proto_reply_error(b, "ERR ...")   -> "-ERR ...\r\n"
 *   proto_reply_int(b, -42)           -> ":-42\r\n"
 *   proto_reply_null(b)               -> "$-1\r\n"
 *   proto_reply_bulk(b, p, 3)         -> "$3\r\n<3 bytes>\r\n"
 */
int proto_reply_simple(struct proto_buf *b, const char *s);
int proto_reply_error(struct proto_buf *b, const char *msg);
int proto_reply_int(struct proto_buf *b, int64_t v);
int proto_reply_null(struct proto_buf *b);
int proto_reply_bulk(struct proto_buf *b, const char *data, size_t len);

#endif
