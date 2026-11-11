#include "net.h"

#include "log.h"
#include "proto.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_EPOLL_BATCH 64
#define NET_READ_CHUNK 16384U

struct client {
    int fd;
    size_t slot; /* index in net->clients[] for O(1) removal */
    struct proto_buf rbuf;
    size_t rpos; /* bytes of rbuf already consumed by proto_parse */
    struct proto_buf wbuf;
    size_t wpos; /* bytes of wbuf already written to fd */
    int epoll_out;
};

struct net {
    int listen_fd;
    int epfd;
    struct cmd *cmd;
    struct client *clients[NET_MAX_CLIENTS];
    size_t num_clients;
};

static void close_fd(int fd)
{
    if (fd >= 0) {
        (void)close(fd);
    }
}

static void client_close(struct net *n, struct client *c)
{
    size_t idx = c->slot;
    size_t last = n->num_clients - 1U;

    (void)epoll_ctl(n->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close_fd(c->fd);
    proto_buf_destroy(&c->rbuf);
    proto_buf_destroy(&c->wbuf);

    if (idx != last) {
        n->clients[idx] = n->clients[last];
        n->clients[idx]->slot = idx;
    }
    n->clients[last] = NULL;
    n->num_clients--;
    free(c);
}

static int client_update_epoll(struct net *n, struct client *c, int want_out)
{
    struct epoll_event ev;

    if (c->epoll_out == want_out) {
        return 0;
    }
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | (want_out ? EPOLLOUT : 0U);
    ev.data.ptr = c;
    if (epoll_ctl(n->epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0) {
        log_error("epoll_ctl(MOD): %s", strerror(errno));
        return -1;
    }
    c->epoll_out = want_out;
    return 0;
}

/*
 * Writes as much of c->wbuf as the socket accepts right now.
 * Returns 0 if the client stays alive, or -1 if the caller should close it.
 */
static int client_flush(struct net *n, struct client *c)
{
    while (c->wpos < c->wbuf.len) {
        ssize_t nw = send(c->fd, c->wbuf.data + c->wpos, c->wbuf.len - c->wpos, MSG_NOSIGNAL);

        if (nw > 0) {
            c->wpos += (size_t)nw;
            continue;
        }
        if (nw < 0 && errno == EINTR) {
            continue;
        }
        if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Kernel send buffer is full; wait for EPOLLOUT. */
            return client_update_epoll(n, c, 1);
        }
        return -1;
    }

    /* All pending reply bytes were sent; reset buffer and disable EPOLLOUT. */
    proto_buf_clear(&c->wbuf);
    c->wpos = 0;
    return client_update_epoll(n, c, 0);
}

/*
 * Sends a fatal protocol error reply best-effort and closes the client.
 */
static void client_protocol_error(struct net *n, struct client *c, const char *reason)
{
    char msg[192];

    (void)snprintf(msg, sizeof msg, "ERR Protocol error: %s", reason);
    if (proto_reply_error(&c->wbuf, msg) == 0) {
        (void)client_flush(n, c);
    }
    client_close(n, c);
}

static void client_compact_rbuf(struct client *c)
{
    size_t unread;

    if (c->rpos == 0) {
        return;
    }
    unread = c->rbuf.len - c->rpos;
    if (unread == 0) {
        proto_buf_clear(&c->rbuf);
        c->rpos = 0;
        return;
    }
    /* Compact once after parsing a batch, or when consumed prefix is large. */
    if (c->rpos >= 4096U || c->rpos >= unread) {
        memmove(c->rbuf.data, c->rbuf.data + c->rpos, unread);
        c->rbuf.len = unread;
        c->rpos = 0;
    }
}

/*
 * Parses and executes all complete commands currently in c->rbuf.
 * Returns 0 to keep the client open, -1 if the client was closed.
 */
static int client_process_rbuf(struct net *n, struct client *c)
{
    while (c->rpos < c->rbuf.len) {
        struct proto_cmd cmd;
        size_t consumed = 0;
        char err[128];
        enum proto_result r;

        r = proto_parse(c->rbuf.data + c->rpos, c->rbuf.len - c->rpos, &cmd, &consumed, err,
                        sizeof err);
        if (r == PROTO_INCOMPLETE) {
            break;
        }
        if (r == PROTO_ERROR) {
            client_protocol_error(n, c, err[0] != '\0' ? err : "malformed request");
            return -1;
        }

        if (cmd_exec(n->cmd, &cmd, &c->wbuf) != 0) {
            proto_cmd_reset(&cmd);
            client_close(n, c);
            return -1;
        }
        proto_cmd_reset(&cmd);
        c->rpos += consumed;
    }

    client_compact_rbuf(c);
    return 0;
}

static void client_on_readable(struct net *n, struct client *c)
{
    for (;;) {
        ssize_t nr;
        size_t unread;

        if (proto_buf_reserve(&c->rbuf, NET_READ_CHUNK) != 0) {
            log_error("out of memory growing client read buffer");
            client_close(n, c);
            return;
        }

        nr = read(c->fd, c->rbuf.data + c->rbuf.len, c->rbuf.cap - c->rbuf.len);
        if (nr > 0) {
            c->rbuf.len += (size_t)nr;
            unread = c->rbuf.len - c->rpos;
            if (unread > NET_MAX_RBUF) {
                client_protocol_error(n, c, "input buffer limit exceeded");
                return;
            }
            continue;
        }
        if (nr == 0) {
            /* Peer closed its write half cleanly. */
            client_close(n, c);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        /* Connection reset or other socket error. */
        client_close(n, c);
        return;
    }

    if (client_process_rbuf(n, c) != 0) {
        return;
    }
    if (c->wpos < c->wbuf.len && client_flush(n, c) != 0) {
        client_close(n, c);
    }
}

static void accept_clients(struct net *n)
{
    for (;;) {
        int cfd = accept4(n->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        struct client *c;
        struct epoll_event ev;

        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            log_error("accept4: %s", strerror(errno));
            return;
        }

        if (n->num_clients >= NET_MAX_CLIENTS) {
            static const char busy[] = "-ERR max number of clients reached\r\n";
            (void)send(cfd, busy, sizeof busy - 1U, MSG_NOSIGNAL);
            close_fd(cfd);
            continue;
        }

        c = calloc(1U, sizeof *c);
        if (c == NULL) {
            log_error("calloc(client): out of memory");
            close_fd(cfd);
            return;
        }
        c->fd = cfd;
        c->slot = n->num_clients;
        proto_buf_init(&c->rbuf);
        proto_buf_init(&c->wbuf);

        memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        if (epoll_ctl(n->epfd, EPOLL_CTL_ADD, cfd, &ev) != 0) {
            log_error("epoll_ctl(ADD client): %s", strerror(errno));
            close_fd(cfd);
            free(c);
            return;
        }

        n->clients[n->num_clients++] = c;
    }
}

struct net *net_create(uint16_t port, struct cmd *cmd)
{
    struct net *n;
    int opt = 1;
    struct sockaddr_in addr;
    struct epoll_event ev;

    n = calloc(1U, sizeof *n);
    if (n == NULL) {
        log_error("calloc(net): out of memory");
        return NULL;
    }
    n->listen_fd = -1;
    n->epfd = -1;
    n->cmd = cmd;

    n->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (n->listen_fd < 0) {
        log_error("socket: %s", strerror(errno));
        net_destroy(n);
        return NULL;
    }

    if (setsockopt(n->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) != 0) {
        log_error("setsockopt(SO_REUSEADDR): %s", strerror(errno));
        net_destroy(n);
        return NULL;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(n->listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        log_error("bind(port %u): %s", (unsigned)port, strerror(errno));
        net_destroy(n);
        return NULL;
    }

    if (listen(n->listen_fd, SOMAXCONN) != 0) {
        log_error("listen: %s", strerror(errno));
        net_destroy(n);
        return NULL;
    }

    n->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (n->epfd < 0) {
        log_error("epoll_create1: %s", strerror(errno));
        net_destroy(n);
        return NULL;
    }

    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; /* NULL sentinel distinguishes listen_fd from struct client * */
    if (epoll_ctl(n->epfd, EPOLL_CTL_ADD, n->listen_fd, &ev) != 0) {
        log_error("epoll_ctl(ADD listen): %s", strerror(errno));
        net_destroy(n);
        return NULL;
    }

    return n;
}

int net_run(struct net *n, const volatile sig_atomic_t *stop_flag, const sigset_t *wait_mask)
{
    struct epoll_event events[NET_EPOLL_BATCH];

    while (!*stop_flag) {
        int nev = epoll_pwait(n->epfd, events, NET_EPOLL_BATCH, -1, wait_mask);

        if (nev < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_pwait: %s", strerror(errno));
            return -1;
        }

        for (int i = 0; i < nev; i++) {
            uint32_t mask = events[i].events;
            struct client *c = events[i].data.ptr;

            if (c == NULL) {
                accept_clients(n);
                continue;
            }

            if (mask & (EPOLLERR | EPOLLHUP)) {
                client_close(n, c);
                continue;
            }
            if (mask & EPOLLIN) {
                client_on_readable(n, c);
                /* client_on_readable may have closed and freed c; skip EPOLLOUT in that turn. */
                continue;
            }
            if (mask & EPOLLOUT) {
                if (client_flush(n, c) != 0) {
                    client_close(n, c);
                }
            }
        }
    }
    return 0;
}

void net_destroy(struct net *n)
{
    if (n == NULL) {
        return;
    }
    while (n->num_clients > 0) {
        client_close(n, n->clients[n->num_clients - 1U]);
    }
    close_fd(n->listen_fd);
    close_fd(n->epfd);
    free(n);
}
