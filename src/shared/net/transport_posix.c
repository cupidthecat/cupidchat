/*
 * src/shared/net/transport_posix.c -  plain-TCP transport implementation
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>

#include "net/transport.h"

/* vtable implementations */

static ssize_t plain_read(transport_t *t, void *buf, size_t n) {
    ssize_t r = read(t->fd, buf, n);
    return r;   /* caller checks EAGAIN / EWOULDBLOCK */
}

static ssize_t plain_write(transport_t *t, const void *buf, size_t n) {
    ssize_t r = write(t->fd, buf, n);
    return r;
}

static int plain_handshake(transport_t *t) {
    (void)t;
    return 0;  /* no-op for plain TCP */
}

static void plain_close(transport_t *t) {
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

/* Factory */

transport_t *transport_plain_new(int fd) {
    transport_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fd        = fd;
    t->mode      = TRANSPORT_PLAIN;
    t->read      = plain_read;
    t->write     = plain_write;
    t->handshake = plain_handshake;
    t->close     = plain_close;
    t->tls_ctx   = NULL;
    return t;
}

void transport_free(transport_t *t) {
    if (!t) return;
    if (t->close) t->close(t);
    free(t);
}

/* Utility: set fd to non-blocking */
int transport_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
