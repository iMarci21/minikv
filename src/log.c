#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#define LOG_MSG_MAX 1024

__attribute__((format(printf, 2, 0))) static void log_write(const char *level, const char *fmt,
                                                            va_list ap)
{
    struct timespec now = {0};
    struct tm tm;
    char ts[32] = "0000-00-00 00:00:00";
    char msg[LOG_MSG_MAX];

    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && localtime_r(&now.tv_sec, &tm) != NULL) {
        (void)strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    }

    /*
     * Format into a local buffer first and emit the whole line with a single fprintf.
     * stdio locks the stream per call, so lines from different threads (the future
     * fsync thread) can never interleave mid-line. Truncating of overlong messages is
     * fine, so the return value is just voided.
     */
    (void)vsnprintf(msg, sizeof msg, fmt, ap);

    (void)fprintf(stderr, "%s.%03ld [%s] %s\n", ts, now.tv_nsec / 1000000L, level, msg);
}

void log_info(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}
