#ifndef MINIKV_LOG_H
#define MINIKV_LOG_H

/*
 * Minimal logging to stderr. Every log-call writes exactly one line:
 *
 *   2026-11-08 23:01:46.123 [INFO] message
 *
 * Messages longer than the internal buffer are always truncated.
 * May be used by every module (except proto which must stay free of I/O).
 * Not async-signal-safe: never call from a sighandler.
 */

void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
