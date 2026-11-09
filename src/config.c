#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 6379
#define DEFAULT_WAL_PATH "minikv.wal"

__attribute__((format(printf, 3, 4))) static void set_err(char *err, size_t err_len,
                                                          const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    /* Truncating an overlong message is fine, hence the ignored return value. */
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static int parse_port(const char *s, uint16_t *out)
{
    char *end;
    long v;

    /* strtol alone would accept leading whitespace and a sign (" 80", "+80", "-1"). */
    if (s[0] < '0' || s[0] > '9') {
        return -1;
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < 1 || v > 65535) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int parse_fsync(const char *s, enum fsync_policy *out)
{
    if (strcmp(s, "always") == 0) {
        *out = FSYNC_ALWAYS;
    } else if (strcmp(s, "everysec") == 0) {
        *out = FSYNC_EVERYSEC;
    } else if (strcmp(s, "no") == 0) {
        *out = FSYNC_NO;
    } else {
        return -1;
    }
    return 0;
}

/*
 * Returns 1 if arg is "--<name>" or "--<name>=<value>". In the second form *inline_value
 * points at <value> (possibly empty), otherwise it is set to NULL.
 */
static int match_option(const char *arg, const char *name, const char **inline_value)
{
    const size_t n = strlen(name);

    if (strncmp(arg, "--", 2) != 0 || strncmp(arg + 2, name, n) != 0) {
        return 0;
    }
    if (arg[2 + n] == '\0') {
        *inline_value = NULL;
        return 1;
    }
    if (arg[2 + n] == '=') {
        *inline_value = arg + 2 + n + 1;
        return 1;
    }
    return 0; /* e.g. "--portx" */
}

/*
 * Hand-rolled instead of getopt_long: getopt keeps state in globals (optind, optarg),
 * prints its own error messages and permutes argv, none of which fits a pure function.
 */
enum config_result config_parse(int argc, char *const argv[], struct config *out, char *err,
                                size_t err_len)
{
    struct config cfg = {
        .port = DEFAULT_PORT,
        .wal_path = DEFAULT_WAL_PATH,
        .fsync = FSYNC_EVERYSEC,
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        enum { OPT_PORT, OPT_WAL, OPT_FSYNC } opt;

        if (strcmp(arg, "--help") == 0) {
            return CONFIG_HELP;
        } else if (match_option(arg, "port", &value)) {
            opt = OPT_PORT;
        } else if (match_option(arg, "wal", &value)) {
            opt = OPT_WAL;
        } else if (match_option(arg, "appendfsync", &value)) {
            opt = OPT_FSYNC;
        } else if (arg[0] == '-') {
            set_err(err, err_len, "unknown option '%s'", arg);
            return CONFIG_ERROR;
        } else {
            set_err(err, err_len, "unexpected argument '%s'", arg);
            return CONFIG_ERROR;
        }

        if (value == NULL) {
            if (i + 1 >= argc) {
                set_err(err, err_len, "option '%s' requires a value", arg);
                return CONFIG_ERROR;
            }
            value = argv[++i];
        }

        switch (opt) {
        case OPT_PORT:
            if (parse_port(value, &cfg.port) != 0) {
                set_err(err, err_len, "invalid port '%s' (expected 1-65535)", value);
                return CONFIG_ERROR;
            }
            break;
        case OPT_WAL:
            if (value[0] == '\0') {
                set_err(err, err_len, "WAL path must not be empty");
                return CONFIG_ERROR;
            }
            cfg.wal_path = value;
            break;
        case OPT_FSYNC:
            if (parse_fsync(value, &cfg.fsync) != 0) {
                set_err(err, err_len, "invalid appendfsync '%s' (expected always, everysec or no)",
                        value);
                return CONFIG_ERROR;
            }
            break;
        }
    }

    *out = cfg;
    return CONFIG_OK;
}

int config_usage(FILE *out, const char *prog)
{
    return fprintf(out,
                   "Usage: %s [options]\n"
                   "\n"
                   "Options:\n"
                   "  --port N                          TCP port (1-65535, default %d)\n"
                   "  --wal PATH                        write-ahead log file (default %s)\n"
                   "  --appendfsync always|everysec|no  WAL fsync policy (default everysec)\n"
                   "  --help                            show this help and exit\n"
                   "\n"
                   "Options also accept the --name=value form.\n",
                   prog, DEFAULT_PORT, DEFAULT_WAL_PATH);
}

const char *config_fsync_name(enum fsync_policy p)
{
    switch (p) {
    case FSYNC_ALWAYS:
        return "always";
    case FSYNC_EVERYSEC:
        return "everysec";
    case FSYNC_NO:
        return "no";
    }
    return "unknown";
}
