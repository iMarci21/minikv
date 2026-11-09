#ifndef MINIKV_CONFIG_H
#define MINIKV_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum fsync_policy {
    FSYNC_ALWAYS,
    FSYNC_EVERYSEC,
    FSYNC_NO,
};

struct config {
    uint16_t port;
    /* Borrowed: points into argv or at a string literal. Valid for the whole process. */
    const char *wal_path;
    enum fsync_policy fsync;
};

enum config_result {
    CONFIG_OK,    /* *out is filled in */
    CONFIG_HELP,  /* --help was given; *out is untouched */
    CONFIG_ERROR, /* invalid arguments; reason in err, *out is untouched */
};

/*
 * Parses command-line arguments into *out. Pure: no I/O, no globals, never exits, and
 * argv is not modified. On CONFIG_ERROR a one-line, NUL-terminated reason is written to
 * err (truncated to err_len bytes).
 */
enum config_result config_parse(int argc, char *const argv[], struct config *out, char *err,
                                size_t err_len);

/* Writes the usage text to out. Returns a negative value on write error. */
int config_usage(FILE *out, const char *prog);

/* Returns the command-line spelling of p, e.g. "everysec". Static string, never NULL. */
const char *config_fsync_name(enum fsync_policy p);

#endif
