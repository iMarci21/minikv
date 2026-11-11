#include "cmd.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "net.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_USAGE 2

/*
 * Number of the signal that requested shutdown, 0 when running.
 */
static volatile sig_atomic_t g_stop_signal;

static void on_stop_signal(int sig)
{
    g_stop_signal = sig;
}

/*
 * SIGPIPE is ignored so a client that disconnects mid-write cant kill the server.
 *
 * SIGINT/SIGTERM only set g_stop_signal, and stay BLOCKED during normal execution. The
 * wait loop unblocks them (sigsuspend now, epoll_pwait later). This prevents
 * the race of a plain "check flag, then block" loop, where a signal arriving between the
 * check and the blocking call goes unnoticed. Blocking them before any thread exists also
 * means future threads (the WAL fsync thread) inherit the mask, so these signals are always
 * delivered to the main thread.
 *
 * On success, *wait_mask is the mask to install while waiting.
 */
static int setup_signals(sigset_t *wait_mask)
{
    struct sigaction sa;
    sigset_t stop_set;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        log_error("sigaction(SIGPIPE): %s", strerror(errno));
        return -1;
    }

    sigemptyset(&stop_set);
    sigaddset(&stop_set, SIGINT);
    sigaddset(&stop_set, SIGTERM);

    /* Block first, so a signal arriving right after the handler is installed stays in pending. */
    if (sigprocmask(SIG_BLOCK, &stop_set, wait_mask) != 0) {
        log_error("sigprocmask: %s", strerror(errno));
        return -1;
    }
    /* The wait mask is the inherited mask with our stop signals guaranteed unblocked. */
    sigdelset(wait_mask, SIGINT);
    sigdelset(wait_mask, SIGTERM);

    /* No SA_RESTART: blocking calls must return EINTR so the loop sees the flag. */
    sa.sa_handler = on_stop_signal;
    sa.sa_mask = stop_set;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        log_error("sigaction(SIGINT/SIGTERM): %s", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Returns the name of the given signal as string for
 * SIGINT and SIGTERM. Otherwise returns "unknown signal".
 */
static const char *stop_signal_name(int sig)
{
    return sig == SIGINT ? "SIGINT" : sig == SIGTERM ? "SIGTERM" : "unknown signal";
}

int main(int argc, char **argv)
{
    const char *progname = "minikv";
    struct config cfg;
    char err[256];
    sigset_t wait_mask;
    struct db *db = NULL;
    struct cmd *cmd = NULL;
    struct net *net = NULL;
    int exit_code = EXIT_FAILURE;

    /* 1. Configuration */
    switch (config_parse(argc, argv, &cfg, err, sizeof err)) {
    case CONFIG_OK:
        break;
    case CONFIG_HELP:
        if (config_usage(stdout, progname) < 0 || fflush(stdout) == EOF) {
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    case CONFIG_ERROR:
        (void)fprintf(stderr, "%s: %s\n", progname, err);
        (void)config_usage(stderr, progname);
        return EXIT_USAGE;
    }

    /* 2. Signals */
    if (setup_signals(&wait_mask) != 0) {
        return EXIT_FAILURE;
    }

    log_info("minikv starting (port=%u, wal=%s, appendfsync=%s)", (unsigned)cfg.port, cfg.wal_path,
             config_fsync_name(cfg.fsync));

    /* 3. In-memory database */
    db = db_create();
    if (db == NULL) {
        log_error("db_create: out of memory");
        goto cleanup;
    }

    /*
     * 4. wal_open + wal_replay into the db (roadmap step 7)
     */

    /* 5. Command dispatcher */
    cmd = cmd_create(db);
    if (cmd == NULL) {
        log_error("cmd_create: out of memory");
        goto cleanup;
    }

    /* 6. Network listener + epoll */
    net = net_create(cfg.port, cmd);
    if (net == NULL) {
        goto cleanup;
    }

    /* 7. Event loop until a stop signal arrives */
    log_info("ready, listening on port %u", (unsigned)cfg.port);
    if (net_run(net, &g_stop_signal, &wait_mask) != 0) {
        goto cleanup;
    }
    log_info("received %s, shutting down", stop_signal_name(g_stop_signal));
    exit_code = EXIT_SUCCESS;

cleanup:
    /* 8. Teardown, reverse of startup: net, cmd, wal (flush, join, fsync), db */
    net_destroy(net);
    cmd_destroy(cmd);
    db_destroy(db);

    if (exit_code == EXIT_SUCCESS) {
        log_info("shutdown complete");
    }
    return exit_code;
}
