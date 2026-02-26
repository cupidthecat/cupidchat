/*
 * src/server/main.c -  cupid-chatd entry point
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include "server/config.h"
#include "server/state.h"
#include "server/loop.h"
#include "server/log.h"

/* forward declaration from loop.c helpers (we need make_listen_socket) */
static int make_listen_socket(const char *host, uint16_t port);
static int make_timerfd(int interval_secs);

/* Duplicate of the static helpers from loop.c so main.c can setup state.
   In a larger project these would live in a net_util.c; kept here for clarity. */
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int make_listen_socket(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, SOMAXCONN) < 0) {
        perror("bind/listen"); close(fd); return -1;
    }
    return fd;
}

static int make_timerfd(int interval_secs) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) { perror("timerfd_create"); return -1; }
    struct itimerspec its = {
        .it_interval = {.tv_sec = interval_secs, .tv_nsec = 0},
        .it_value    = {.tv_sec = interval_secs, .tv_nsec = 0},
    };
    timerfd_settime(tfd, 0, &its, NULL);
    return tfd;
}

int main(int argc, char **argv) {
    server_config_t cfg;
    int rc = config_parse(argc, argv, &cfg);
    if (rc == 1)  { config_usage(argv[0]); return 0; }
    if (rc == -1) { config_usage(argv[0]); return 1; }

    if (cfg.verbose) log_set_level(LOG_DEBUG);

    server_state_t s;
    if (server_state_init(&s, &cfg) < 0) {
        fprintf(stderr, "server_state_init failed\n");
        return 1;
    }

    /* epoll fd */
    s.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (s.epoll_fd < 0) {
        perror("epoll_create1");
        server_state_free(&s);
        return 1;
    }

    /* plain listen socket */
    s.listen_fd_plain = make_listen_socket(cfg.host, cfg.port_plain);
    if (s.listen_fd_plain < 0) {
        server_state_free(&s);
        return 1;
    }
    LOG_INFO("listening on %s:%u (plain)", cfg.host, cfg.port_plain);

    /* TLS listen socket */
    s.listen_fd_tls = -1;
    if (cfg.tls_enabled) {
        s.listen_fd_tls = make_listen_socket(cfg.host, cfg.port_tls);
        if (s.listen_fd_tls < 0) {
            server_state_free(&s);
            return 1;
        }
        LOG_INFO("listening on %s:%u (TLS)", cfg.host, cfg.port_tls);
    }

    /* timerfd -  fires every second for keepalive bookkeeping */
    s.timer_fd = make_timerfd(1);
    if (s.timer_fd < 0) {
        server_state_free(&s);
        return 1;
    }

    rc = server_run(&s);

    server_state_free(&s);
    return rc;
}
