#ifndef MINIKV_NET_H
#define MINIKV_NET_H

#include "cmd.h"

#include <signal.h>
#include <stdint.h>

/*
 * Non-blocking TCP server and level-triggered epoll event loop.
 *
 * Owns the listening socket, the epoll instance, and all active client
 * connections and their read/write buffers.
 */

#define NET_MAX_CLIENTS 1024U
#define NET_MAX_RBUF (PROTO_MAX_BULK + 65536U)

struct net;

/*
 * Binds a non-blocking TCP listening socket on port and creates the epoll
 * instance. cmd is borrowed and must outlive the returned struct net.
 * Caller owns the returned pointer and must free it with net_destroy().
 * Returns NULL on failure (already logged).
 */
struct net *net_create(uint16_t port, struct cmd *cmd);

/*
 * Runs the event loop until *stop_flag becomes non-zero. wait_mask is passed to
 * epoll_pwait() so SIGINT/SIGTERM are unblocked atomically while waiting.
 * Returns 0 on clean signal stop, -1 on unrecoverable epoll error.
 */
int net_run(struct net *n, const volatile sig_atomic_t *stop_flag, const sigset_t *wait_mask);

/*
 * Closes the listening socket, all open client connections and their buffers,
 * the epoll instance, and frees n. Safe to call with NULL.
 */
void net_destroy(struct net *n);

#endif
