#include "cmd.h"
#include "db.h"
#include "proto.h"

#include <stdint.h>
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

static int str_eq(struct proto_str s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.data, lit, n) == 0;
}

static void test_simple_commands(void)
{
    const char *ping = "*1\r\n$4\r\nPING\r\n";
    const char *set = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    const char *inl = "  PING   hello\tworld  \r\n";
    struct proto_cmd cmd;
    size_t consumed = 0;
    char err[128];

    CHECK(proto_parse(ping, strlen(ping), &cmd, &consumed, err, sizeof err) == PROTO_OK);
    CHECK(consumed == strlen(ping));
    CHECK(cmd.argc == 1);
    CHECK(cmd.argv == cmd.inline_argv);
    CHECK(str_eq(cmd.argv[0], "PING"));
    proto_cmd_reset(&cmd);

    CHECK(proto_parse(set, strlen(set), &cmd, &consumed, err, sizeof err) == PROTO_OK);
    CHECK(consumed == strlen(set));
    CHECK(cmd.argc == 3);
    CHECK(str_eq(cmd.argv[0], "SET"));
    CHECK(str_eq(cmd.argv[1], "foo"));
    CHECK(str_eq(cmd.argv[2], "bar"));
    proto_cmd_reset(&cmd);

    CHECK(proto_parse(inl, strlen(inl), &cmd, &consumed, err, sizeof err) == PROTO_OK);
    CHECK(consumed == strlen(inl));
    CHECK(cmd.argc == 3);
    CHECK(str_eq(cmd.argv[0], "PING"));
    CHECK(str_eq(cmd.argv[1], "hello"));
    CHECK(str_eq(cmd.argv[2], "world"));
    proto_cmd_reset(&cmd);

    printf("ok:   simple and inline commands (PING, SET, inline)\n");
}

static void test_empty_and_binary_bulk(void)
{
    /* "SET" "" "\0\r\n\0X" — embedded NULs and CRLF inside the payload */
    static const char raw[] = "*3\r\n"
                              "$3\r\nSET\r\n"
                              "$0\r\n\r\n"
                              "$5\r\n\0\r\n\0X\r\n";
    const size_t raw_len = (sizeof raw) - 1U;
    struct proto_cmd cmd;
    size_t consumed = 0;

    CHECK(proto_parse(raw, raw_len, &cmd, &consumed, NULL, 0) == PROTO_OK);
    CHECK(consumed == raw_len);
    CHECK(cmd.argc == 3);
    CHECK(str_eq(cmd.argv[0], "SET"));
    CHECK(cmd.argv[1].len == 0);
    CHECK(cmd.argv[2].len == 5);
    CHECK(memcmp(cmd.argv[2].data, "\0\r\n\0X", 5) == 0);
    proto_cmd_reset(&cmd);

    printf("ok:   empty and binary bulk strings (embedded NUL and CRLF)\n");
}

static void test_every_split_point(void)
{
    const char *msg = "*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$5\r\nworld\r\n";
    const size_t total = strlen(msg);
    struct proto_cmd cmd;
    size_t consumed = 999;
    char err[128];

    /* Every strict prefix (including 0 bytes) must report PROTO_INCOMPLETE. */
    for (size_t prefix = 0; prefix < total; prefix++) {
        consumed = 999;
        CHECK(proto_parse(msg, prefix, &cmd, &consumed, err, sizeof err) == PROTO_INCOMPLETE);
        CHECK(consumed == 0);
        CHECK(cmd.argc == 0);
        CHECK(cmd.argv == NULL);
    }

    CHECK(proto_parse(msg, total, &cmd, &consumed, err, sizeof err) == PROTO_OK);
    CHECK(consumed == total);
    CHECK(cmd.argc == 3);
    proto_cmd_reset(&cmd);

    printf("ok:   every split point from 0..%zu returns PROTO_INCOMPLETE\n", total - 1U);
}

static void test_pipelining(void)
{
    const char *pipe = "*1\r\n$4\r\nPING\r\n"
                       "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
                       "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
                       "*2\r\n$3\r\nGET"; /* trailing partial 4th command */
    const size_t total = strlen(pipe);
    size_t off = 0;
    size_t consumed = 0;
    struct proto_cmd cmd;

    CHECK(proto_parse(pipe + off, total - off, &cmd, &consumed, NULL, 0) == PROTO_OK);
    CHECK(cmd.argc == 1 && str_eq(cmd.argv[0], "PING"));
    proto_cmd_reset(&cmd);
    off += consumed;

    CHECK(proto_parse(pipe + off, total - off, &cmd, &consumed, NULL, 0) == PROTO_OK);
    CHECK(cmd.argc == 3 && str_eq(cmd.argv[0], "SET") && str_eq(cmd.argv[2], "1"));
    proto_cmd_reset(&cmd);
    off += consumed;

    CHECK(proto_parse(pipe + off, total - off, &cmd, &consumed, NULL, 0) == PROTO_OK);
    CHECK(cmd.argc == 2 && str_eq(cmd.argv[0], "GET") && str_eq(cmd.argv[1], "a"));
    proto_cmd_reset(&cmd);
    off += consumed;

    /* Remaining bytes are the incomplete 4th command. */
    CHECK(proto_parse(pipe + off, total - off, &cmd, &consumed, NULL, 0) == PROTO_INCOMPLETE);
    CHECK(consumed == 0);

    printf("ok:   pipelined commands + trailing partial command\n");
}

static void test_many_args_and_large_bulk(void)
{
    /* 12 arguments (> PROTO_INLINE_ARGS) exercises the heap argv path. */
    const char *many = "*12\r\n"
                       "$4\r\nMSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n$2\r\nk2\r\n$2\r\nv2\r\n"
                       "$2\r\nk3\r\n$2\r\nv3\r\n$2\r\nk4\r\n$2\r\nv4\r\n$2\r\nk5\r\n"
                       "$2\r\nv5\r\n$2\r\nk6\r\n";
    struct proto_cmd cmd;
    size_t consumed = 0;

    CHECK(proto_parse(many, strlen(many), &cmd, &consumed, NULL, 0) == PROTO_OK);
    CHECK(cmd.argc == 12);
    CHECK(cmd.argv != cmd.inline_argv);
    CHECK(str_eq(cmd.argv[0], "MSET"));
    CHECK(str_eq(cmd.argv[11], "k6"));
    proto_cmd_reset(&cmd);

    /* 100 KB value (matches tests/integration.sh). */
    {
        const size_t val_len = 100000U;
        const char *hdr = "*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$100000\r\n";
        const size_t hdr_len = strlen(hdr);
        const size_t total = hdr_len + val_len + 2U;
        char *buf = malloc(total);

        CHECK(buf != NULL);
        memcpy(buf, hdr, hdr_len);
        memset(buf + hdr_len, 'x', val_len);
        buf[hdr_len + val_len] = '\r';
        buf[hdr_len + val_len + 1U] = '\n';

        CHECK(proto_parse(buf, total, &cmd, &consumed, NULL, 0) == PROTO_OK);
        CHECK(consumed == total);
        CHECK(cmd.argc == 3);
        CHECK(cmd.argv[2].len == val_len);
        CHECK(cmd.argv[2].data == buf + hdr_len); /* zero-copy pointer check */
        proto_cmd_reset(&cmd);
        free(buf);
    }

    printf("ok:   >8 args (heap argv) and 100 KB zero-copy bulk string\n");
}

static void test_malformed_inputs(void)
{
    static const char *const bad[] = {
        "\r\n",                            /* empty inline command */
        "   \t  \r\n",                     /* whitespace-only inline command */
        "+OK\r\n",                         /* simple string reply, not a command */
        "-ERR\r\n",                        /* error reply, not a command */
        ":1\r\n",                          /* integer reply, not a command */
        "$4\r\nPING\r\n",                  /* bare bulk string without array */
        "*\r\n",                           /* empty array count */
        "*0\r\n",                          /* zero args */
        "*-1\r\n",                         /* negative array count */
        "*+1\r\n$4\r\nPING\r\n",           /* leading '+' in count */
        "* 1\r\n$4\r\nPING\r\n",           /* leading space in count */
        "*1a\r\n$4\r\nPING\r\n",           /* trailing junk in count */
        "*1025\r\n",                       /* > PROTO_MAX_ARGS */
        "*18446744073709551616\r\n",       /* u64 overflow in array count */
        "*1\n$4\r\nPING\r\n",              /* bare LF in array header */
        "*1\rX$4\r\nPING\r\n",             /* CR not followed by LF */
        "*1\r\n:1\r\n",                    /* integer element instead of bulk string */
        "*1\r\n$\r\n",                     /* empty bulk length */
        "*1\r\n$-1\r\n",                   /* null bulk string in request */
        "*1\r\n$67108865\r\n",             /* > PROTO_MAX_BULK (rejected before payload!) */
        "*1\r\n$18446744073709551616\r\n", /* u64 overflow in bulk length */
        "*1\r\n$4\nPING\r\n",              /* bare LF in bulk header */
        "*1\r\n$4\r\nPINGxx",              /* payload not followed by CRLF */
        "*1\r\n$4\r\nPING\rX",             /* payload CR not followed by LF */
        "*1\r\n$3\r\nPING\r\n",            /* bulk length too short for payload */
        /* >8 args so heap argv is allocated, then fail on arg 2 -> LSan checks cleanup */
        "*9\r\n$1\r\na\r\nBAD\r\n",
    };
    struct proto_cmd cmd;
    size_t consumed = 0;
    char err[128];

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        consumed = 999;
        err[0] = '\0';
        CHECK(proto_parse(bad[i], strlen(bad[i]), &cmd, &consumed, err, sizeof err) == PROTO_ERROR);
        CHECK(consumed == 0);
        CHECK(cmd.argc == 0 && cmd.argv == NULL);
        CHECK(err[0] != '\0');
    }

    printf("ok:   %zu malformed / over-limit inputs rejected with PROTO_ERROR\n",
           sizeof bad / sizeof bad[0]);
}

static void test_proto_buf(void)
{
    struct proto_buf b;

    proto_buf_init(&b);
    CHECK(b.data == NULL && b.len == 0 && b.cap == 0);

    for (int i = 0; i < 200; i++) {
        CHECK(proto_buf_append(&b, "abcd", 4) == 0);
    }
    CHECK(b.len == 800);
    CHECK(b.cap >= 800);
    CHECK(memcmp(b.data, "abcdabcd", 8) == 0);

    proto_buf_clear(&b);
    CHECK(b.len == 0 && b.cap >= 800);

    /* Overflow guard in reserve. */
    b.len = 16;
    CHECK(proto_buf_reserve(&b, SIZE_MAX - 8U) == -1);

    proto_buf_destroy(&b);
    CHECK(b.data == NULL && b.len == 0 && b.cap == 0);

    printf("ok:   proto_buf init / append / grow / clear / overflow / destroy\n");
}

static void test_reply_serializers(void)
{
    struct proto_buf b;

    proto_buf_init(&b);

    CHECK(proto_reply_simple(&b, "OK") == 0);
    CHECK(b.len == 5 && memcmp(b.data, "+OK\r\n", 5) == 0);

    proto_buf_clear(&b);
    CHECK(proto_reply_error(&b, "ERR unknown command") == 0);
    CHECK(b.len == 22 && memcmp(b.data, "-ERR unknown command\r\n", 22) == 0);

    /* Simple strings and errors must reject embedded CR or LF (reply injection guard). */
    proto_buf_clear(&b);
    CHECK(proto_reply_simple(&b, "OK\r\n+FAKE") == -1 && b.len == 0);
    CHECK(proto_reply_error(&b, "ERR\nbad") == -1 && b.len == 0);

    proto_buf_clear(&b);
    CHECK(proto_reply_int(&b, 0) == 0);
    CHECK(proto_reply_int(&b, -42) == 0);
    CHECK(proto_reply_int(&b, INT64_MIN) == 0);
    CHECK(proto_reply_int(&b, INT64_MAX) == 0);
    {
        const char *want = ":0\r\n:-42\r\n:-9223372036854775808\r\n:9223372036854775807\r\n";
        CHECK(b.len == strlen(want) && memcmp(b.data, want, b.len) == 0);
    }

    proto_buf_clear(&b);
    CHECK(proto_reply_null(&b) == 0);
    CHECK(b.len == 5 && memcmp(b.data, "$-1\r\n", 5) == 0);

    proto_buf_clear(&b);
    CHECK(proto_reply_bulk(&b, "", 0) == 0);
    CHECK(b.len == 6 && memcmp(b.data, "$0\r\n\r\n", 6) == 0);

    proto_buf_clear(&b);
    CHECK(proto_reply_bulk(&b, "a\0b\r\n", 5) == 0);
    CHECK(b.len == 11 && memcmp(b.data, "$5\r\na\0b\r\n\r\n", 11) == 0);

    proto_buf_destroy(&b);
    printf("ok:   reply serializers (simple, error, int, null, bulk)\n");
}

static void run_one_cmd(struct cmd *c, const char *raw, size_t raw_len, struct proto_buf *out)
{
    struct proto_cmd req;
    size_t consumed = 0;

    proto_buf_clear(out);
    CHECK(proto_parse(raw, raw_len, &req, &consumed, NULL, 0) == PROTO_OK);
    CHECK(consumed == raw_len);
    CHECK(cmd_exec(c, &req, out) == 0);
    proto_cmd_reset(&req);
}

static void test_cmd_dispatch(void)
{
    struct db *d = db_create();
    struct cmd *c = cmd_create(d);
    struct proto_buf out;

    CHECK(d != NULL && c != NULL);
    proto_buf_init(&out);

    /* PING (case-insensitive, 0 args) -> +PONG */
    run_one_cmd(c, "*1\r\n$4\r\npInG\r\n", 14U, &out);
    CHECK(out.len == 7U && memcmp(out.data, "+PONG\r\n", 7U) == 0);

    /* PING with 1 arg -> bulk reply */
    run_one_cmd(c, "*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n", 25U, &out);
    CHECK(out.len == 11U && memcmp(out.data, "$5\r\nhello\r\n", 11U) == 0);

    /* PING with 2 args -> -ERR wrong number of arguments */
    run_one_cmd(c, "*3\r\n$4\r\nPING\r\n$1\r\na\r\n$1\r\nb\r\n", 28U, &out);
    CHECK(out.len > 5U && memcmp(out.data, "-ERR ", 5U) == 0);

    /* ECHO with binary payload containing NUL */
    {
        static const char echo_bin[] = "*2\r\n$4\r\necho\r\n$3\r\nx\0y\r\n";
        run_one_cmd(c, echo_bin, sizeof echo_bin - 1U, &out);
        CHECK(out.len == 9U && memcmp(out.data, "$3\r\nx\0y\r\n", 9U) == 0);
    }

    /* ECHO with wrong arity */
    run_one_cmd(c, "*1\r\n$4\r\nECHO\r\n", 14U, &out);
    CHECK(out.len > 5U && memcmp(out.data, "-ERR ", 5U) == 0);

    /* SET / GET / STRLEN / DEL */
    run_one_cmd(c, "SET foo bar\r\n", 13U, &out);
    CHECK(out.len == 5U && memcmp(out.data, "+OK\r\n", 5U) == 0);

    run_one_cmd(c, "GET foo\r\n", 9U, &out);
    CHECK(out.len == 9U && memcmp(out.data, "$3\r\nbar\r\n", 9U) == 0);

    run_one_cmd(c, "STRLEN foo\r\n", 12U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":3\r\n", 4U) == 0);

    run_one_cmd(c, "DEL foo missing\r\n", 17U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":1\r\n", 4U) == 0);

    run_one_cmd(c, "GET foo\r\n", 9U, &out);
    CHECK(out.len == 5U && memcmp(out.data, "$-1\r\n", 5U) == 0);

    run_one_cmd(c, "STRLEN foo\r\n", 12U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":0\r\n", 4U) == 0);

    /* Expiry commands: SET EX/PX, SETEX, PSETEX, EXPIRE, PEXPIRE, TTL, PTTL, PERSIST */
    run_one_cmd(c, "TTL missing\r\n", 13U, &out);
    CHECK(out.len == 5U && memcmp(out.data, ":-2\r\n", 5U) == 0);

    run_one_cmd(c, "SET k1 v1\r\n", 11U, &out);
    run_one_cmd(c, "TTL k1\r\n", 8U, &out);
    CHECK(out.len == 5U && memcmp(out.data, ":-1\r\n", 5U) == 0);

    run_one_cmd(c, "EXPIRE k1 10\r\n", 14U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":1\r\n", 4U) == 0);

    run_one_cmd(c, "TTL k1\r\n", 8U, &out);
    CHECK(out.len == 5U && memcmp(out.data, ":10\r\n", 5U) == 0);

    run_one_cmd(c, "PERSIST k1\r\n", 12U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":1\r\n", 4U) == 0);

    run_one_cmd(c, "TTL k1\r\n", 8U, &out);
    CHECK(out.len == 5U && memcmp(out.data, ":-1\r\n", 5U) == 0);

    /* Non-positive EXPIRE deletes key immediately */
    run_one_cmd(c, "EXPIRE k1 0\r\n", 13U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":1\r\n", 4U) == 0);
    run_one_cmd(c, "GET k1\r\n", 8U, &out);
    CHECK(out.len == 5U && memcmp(out.data, "$-1\r\n", 5U) == 0);

    /* SET ... EX / PX and SETEX / PSETEX */
    run_one_cmd(c, "SET k2 v2 EX 20\r\n", 17U, &out);
    CHECK(out.len == 5U && memcmp(out.data, "+OK\r\n", 5U) == 0);
    run_one_cmd(c, "TTL k2\r\n", 8U, &out);
    CHECK(out.len == 5U && memcmp(out.data, ":20\r\n", 5U) == 0);

    run_one_cmd(c, "SETEX k3 30 v3\r\n", 16U, &out);
    CHECK(out.len == 5U && memcmp(out.data, "+OK\r\n", 5U) == 0);
    run_one_cmd(c, "TTL k3\r\n", 8U, &out);
    CHECK(out.len == 5U && memcmp(out.data, ":30\r\n", 5U) == 0);

    run_one_cmd(c, "PSETEX k4 5000 v4\r\n", 19U, &out);
    CHECK(out.len == 5U && memcmp(out.data, "+OK\r\n", 5U) == 0);
    run_one_cmd(c, "TTL k4\r\n", 8U, &out);
    CHECK(out.len == 4U && memcmp(out.data, ":5\r\n", 4U) == 0);

    /* Invalid SET EX/PX syntax and non-positive expire times */
    run_one_cmd(c, "SET k5 v5 XX 10\r\n", 17U, &out);
    CHECK(out.len > 5U && memcmp(out.data, "-ERR ", 5U) == 0);

    run_one_cmd(c, "SET k5 v5 EX 0\r\n", 16U, &out);
    CHECK(out.len > 5U && memcmp(out.data, "-ERR ", 5U) == 0);

    run_one_cmd(c, "SET k5 v5 EX notanint\r\n", 23U, &out);
    CHECK(out.len > 5U && memcmp(out.data, "-ERR ", 5U) == 0);

    /* Unknown command */
    run_one_cmd(c, "*1\r\n$4\r\nNOPE\r\n", 14U, &out);
    CHECK(out.len == 22U && memcmp(out.data, "-ERR unknown command\r\n", 22U) == 0);

    proto_buf_destroy(&out);
    cmd_destroy(c);
    db_destroy(d);
    printf(
        "ok:   cmd_exec (PING, ECHO, SET/EX/PX, SETEX, GET, DEL, STRLEN, EXPIRE, TTL, PERSIST)\n");
}

int main(void)
{
    test_simple_commands();
    test_empty_and_binary_bulk();
    test_every_split_point();
    test_pipelining();
    test_many_args_and_large_bulk();
    test_malformed_inputs();
    test_proto_buf();
    test_reply_serializers();
    test_cmd_dispatch();

    if (g_failed) {
        fprintf(stderr, "SOME UNIT TESTS FAILED\n");
        return 1;
    }
    printf("ALL UNIT TESTS PASSED\n");
    return 0;
}
