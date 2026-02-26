/*
 * src/server/core/loop.c -  single-threaded epoll event loop
 *
 * Design:
 *  - Level-triggered epoll for correctness (no missed events).
 *  - timerfd fires every second for keepalive ticks.
 *  - Read path: read until EAGAIN, parse frames, dispatch.
 *  - Write path: flush output queue on EPOLLOUT.
 *  - Disconnected / error connections are placed in CONN_CLOSING and cleaned
 *     up at the end of each event iteration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

#include "server/loop.h"
#include "server/state.h"
#include "server/conn.h"
#include "server/dispatch.h"
#include "server/keepalive.h"
#include "server/backpressure.h"
#include "server/log.h"
#include "net/transport.h"
#include "proto/frame.h"
#include "proto/tlv.h"
#include "proto/types.h"

#define MAX_EVENTS  256
#define READ_BUF    65536

static volatile int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

static void epoll_add(int epfd, int fd, uint32_t events) {
    struct epoll_event ev = {.events = events, .data.fd = fd};
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

/* Accept new connection */

static void accept_client(server_state_t *s, int listen_fd, bool tls) {
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int cfd = accept4(listen_fd, (struct sockaddr *)&addr, &addrlen,
                          SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_WARN("accept4 failed: %s", strerror(errno));
            break;
        }

        if (s->conn_count >= s->cfg->max_clients) {
            LOG_WARN("max clients (%d) reached, rejecting fd=%d",
                     s->cfg->max_clients, cfd);
            close(cfd);
            continue;
        }
        if (cfd >= s->conns_size) {
            LOG_ERROR("fd=%d exceeds conns table size (%d), rejecting",
                      cfd, s->conns_size);
            close(cfd);
            continue;
        }

        /* TCP_NODELAY for low-latency chat */
        int one = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,   &one, sizeof(one));
        /* SO_KEEPALIVE lets the kernel detect dead connections even when
         * our application-level ping/pong is disabled or very slow. */
        setsockopt(cfd, SOL_SOCKET,  SO_KEEPALIVE,  &one, sizeof(one));

        transport_t *t;
        if (tls) {
            t = transport_tls_new(cfd, true,
                                  s->cfg->tls_cert,
                                  s->cfg->tls_key,
                                  s->cfg->tls_ca);
            if (!t) {
                LOG_WARN("TLS setup failed for fd=%d, closing", cfd);
                close(cfd);
                continue;
            }
        } else {
            t = transport_plain_new(cfd);
            if (!t) { close(cfd); continue; }
        }

        conn_t *c = conn_create(cfd, t, s->epoll_fd, s->cfg);
        if (!c) { transport_free(t); close(cfd); continue; }

        server_register_conn(s, c);
        epoll_add(s->epoll_fd, cfd, EPOLLIN | EPOLLRDHUP);

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        LOG_INFO("accepted fd=%d from %s:%u%s",
                 cfd, ip, ntohs(addr.sin_port), tls ? " (TLS)" : "");
    }
}

/* Read path */

static void read_client(server_state_t *s, conn_t *c) {
    uint8_t buf[READ_BUF];

    while (c->state != CONN_CLOSING) {
        ssize_t n = c->transport->read(c->transport, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_INFO("fd=%d read error: %s, closing", c->fd, strerror(errno));
            c->state = CONN_CLOSING;
            break;
        }
        if (n == 0) {
            LOG_INFO("fd=%d client disconnected", c->fd);
            c->state = CONN_CLOSING;
            break;
        }

        if (frame_parser_push(&c->parser, buf, (size_t)n) < 0) {
            LOG_WARN("fd=%d frame_parser_push OOM", c->fd);
            c->state = CONN_CLOSING;
            break;
        }

        frame_t frame;
        parse_result_t pr;
        while ((pr = frame_parser_next(&c->parser, &frame)) == PARSE_FRAME_OK) {
            if (s->cfg->verbose) frame_dump(&frame, STDERR_FILENO);
            int rc = dispatch_frame(c, &frame, s);
            frame_free(&frame);
            if (rc < 0) { c->state = CONN_CLOSING; break; }
        }
        if (pr == PARSE_ERROR) {
            LOG_WARN("fd=%d bad frame, closing", c->fd);
            c->state = CONN_CLOSING;
        }
    }
}

/* Close a connection and remove from epoll */

static void close_conn(server_state_t *s, conn_t *c) {
    LOG_INFO("fd=%d nick=%s closing connection", c->fd, c->nick);

    /* notify rooms */
    for (int ri = 0; ri < c->room_count; ri++) {
        int ridx = c->rooms[ri];
        room_t *r = (ridx < s->room_count) ? s->rooms[ridx] : NULL;
        if (!r) continue;

        tlv_builder_t b; tlv_builder_init(&b);
        tlv_put_str(&b, TAG_ROOM,    r->name);
        tlv_put_str(&b, TAG_NICK,    c->nick);
        tlv_put_u32(&b, TAG_USER_ID, c->user_id);
        uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
        uint8_t *raw; ssize_t rlen =
            frame_encode(SMSG_USER_LEFT, FRAME_FLAG_PRIORITY_HIGH,
                         server_next_seq(s), p, plen, &raw);
        free(p);
        if (rlen > 0) {
            server_broadcast_room(s, ridx, c->fd, raw, (size_t)rlen,
                                  FRAME_FLAG_PRIORITY_HIGH);
            free(raw);
        }
    }

    epoll_ctl(s->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    server_unregister_conn(s, c);
    conn_destroy(c);
}

/* Timer tick */

static void drain_timerfd(int tfd) {
    uint64_t expirations;
    if (read(tfd, &expirations, sizeof(expirations)) != (ssize_t)sizeof(expirations)) (void)0;
}

/* Main loop */

int server_run(server_state_t *s) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    struct epoll_event events[MAX_EVENTS];

    /* add listen sockets and timerfd to epoll */
    epoll_add(s->epoll_fd, s->listen_fd_plain, EPOLLIN);
    if (s->listen_fd_tls >= 0)
        epoll_add(s->epoll_fd, s->listen_fd_tls, EPOLLIN);
    epoll_add(s->epoll_fd, s->timer_fd, EPOLLIN);

    LOG_INFO("event loop started (plain_fd=%d tls_fd=%d)",
             s->listen_fd_plain, s->listen_fd_tls);

    /* collect fds to close after each iteration */
    int closing[MAX_EVENTS];
    int nclosing;

    while (g_running) {
        int n = epoll_wait(s->epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait: %s", strerror(errno));
            break;
        }

        nclosing = 0;

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == s->listen_fd_plain) {
                accept_client(s, fd, false);
                continue;
            }
            if (fd == s->listen_fd_tls) {
                accept_client(s, fd, true);
                continue;
            }
            if (fd == s->timer_fd) {
                drain_timerfd(fd);
                keepalive_tick(s);
                continue;
            }

            /* client fd */
            conn_t *c = (fd >= 0 && fd < s->conns_size) ? s->conns[fd] : NULL;
            if (!c) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                c->state = CONN_CLOSING;
            }

            if (c->state != CONN_CLOSING) {
                if (events[i].events & EPOLLIN)  read_client(s, c);
                if (events[i].events & EPOLLOUT) {
                    if (conn_flush(c) < 0) c->state = CONN_CLOSING;
                }
            } else {
                /* Still drain pending output (e.g. an error frame sent just
                 * before the close decision) so the client actually receives
                 * the reason for disconnection. */
                if (c->obuf_head && (events[i].events & EPOLLOUT))
                    conn_flush(c);
            }

            /* check backpressure */
            if (c->state != CONN_CLOSING && bp_should_disconnect(c, s->cfg)) {
                bp_send_slow_client_error(c, s->cfg);
                c->state = CONN_CLOSING;
            }

            /* Only close once the output buffer has been fully drained so
             * that error frames (e.g. ERR_NICK_TAKEN) reach the client. */
            if (c->state == CONN_CLOSING && !c->obuf_head) {
                closing[nclosing++] = fd;
            }
        }

        /* close all connections marked CONN_CLOSING this iteration */
        for (int i = 0; i < nclosing; i++) {
            int fd  = closing[i];
            conn_t *c = (fd >= 0 && fd < s->conns_size) ? s->conns[fd] : NULL;
            if (c) close_conn(s, c);
        }
    }

    LOG_INFO("%s", "event loop exiting");
    return 0;
}
