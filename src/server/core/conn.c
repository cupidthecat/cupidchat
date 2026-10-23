/*
 * src/server/core/conn.c -  per-client connection management
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "proto/frame.h"
#include "server/conn.h"
#include "server/log.h"

struct conn_shared_frame {
    /* All references belong to the server's event loop. */
    size_t refs, len;
    uint8_t data[];
};

conn_shared_frame_t *conn_shared_frame_new(const uint8_t *buf, size_t len) {
    if (len > SIZE_MAX - sizeof(conn_shared_frame_t) || (len && !buf))
        return NULL;
    conn_shared_frame_t *frame = malloc(sizeof(*frame) + len);
    if (!frame)
        return NULL;
    frame->refs = 1;
    frame->len = len;
    if (len)
        memcpy(frame->data, buf, len);
    return frame;
}

void conn_shared_frame_release(conn_shared_frame_t *frame) {
    if (frame && --frame->refs == 0)
        free(frame);
}

static void release_data(uint8_t *data, conn_shared_frame_t *shared) {
    if (shared)
        conn_shared_frame_release(shared);
    else
        free(data);
}

static void release_node(obuf_node_t *node) {
    release_data(node->data, node->shared);
    free(node);
}

/* Lifecycle */

conn_t *conn_create(int fd, transport_t *t, int epoll_fd, const server_config_t *cfg) {
    conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    c->fd = fd;
    c->transport = t;
    c->epoll_fd = epoll_fd;
    c->state = CONN_HANDSHAKE;
    c->user_id = 0;

    if (frame_parser_init(&c->parser) < 0) {
        free(c);
        return NULL;
    }

    /* init rate bucket */
    c->rate.capacity = cfg->rate_burst;
    c->rate.tokens = cfg->rate_burst;
    c->rate.rate = cfg->rate_msgs_per_sec;
    c->rate.last_refill = time(NULL);

    c->last_active = time(NULL);
    c->ping_pending = false;

    return c;
}

void conn_destroy(conn_t *c) {
    if (!c)
        return;

    /* free output queue */
    obuf_node_t *n = c->obuf_head;
    while (n) {
        obuf_node_t *nx = n->next;
        release_node(n);
        n = nx;
    }

    frame_parser_free(&c->parser);
    transport_free(c->transport);
    free(c);
}

/* Output queue */

static int enqueue(conn_t *c, uint8_t *buf, size_t len, uint16_t flags, conn_shared_frame_t *shared,
                   const server_config_t *cfg) {
    /* backpressure: drop oldest low-priority frames until we are under limit */
    while (len > cfg->obuf_limit || c->obuf_bytes > cfg->obuf_limit - len) {
        obuf_node_t *prev = NULL, *cur = c->obuf_head;
        bool dropped = false;
        while (cur) {
            /* Once bytes reach TCP, the remainder must stay in order. */
            if ((cur->flags & FRAME_FLAG_PRIORITY_LOW) && cur->sent == 0) {
                obuf_node_t *drop = cur;
                size_t freed = drop->len - drop->sent;
                if (prev)
                    prev->next = drop->next;
                else
                    c->obuf_head = drop->next;
                if (c->obuf_tail == drop)
                    c->obuf_tail = prev;
                c->obuf_bytes -= freed;
                release_node(drop);
                dropped = true;
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        if (!dropped) {
            /* no low-priority frames remain -  refuse the incoming frame */
            release_data(buf, shared);
            if (!(flags & FRAME_FLAG_PRIORITY_LOW) && !c->output_failed) {
                c->output_failed = true;
                LOG_WARN("fd=%d critical output queue full", c->fd);
            }
            return -1;
        }
    }

    obuf_node_t *node = malloc(sizeof(*node));
    if (!node) {
        release_data(buf, shared);
        if (!(flags & FRAME_FLAG_PRIORITY_LOW))
            c->output_failed = true;
        return -1;
    }
    node->data = buf;
    node->shared = shared;
    node->len = len;
    node->sent = 0;
    node->flags = flags;
    node->next = NULL;

    if (c->obuf_tail)
        c->obuf_tail->next = node;
    else
        c->obuf_head = node;
    c->obuf_tail = node;
    c->obuf_bytes += len;

    /* register EPOLLOUT interest if not already */
    if (!c->want_write) {
        struct epoll_event ev = {0};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
        ev.data.fd = c->fd;
        epoll_ctl(c->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
        c->want_write = true;
    }
    return 0;
}

int conn_enqueue(conn_t *c, uint8_t *buf, size_t len, uint16_t flags, const server_config_t *cfg) {
    return enqueue(c, buf, len, flags, NULL, cfg);
}

int conn_enqueue_shared(conn_t *c, conn_shared_frame_t *frame, uint16_t flags,
                        const server_config_t *cfg) {
    frame->refs++;
    return enqueue(c, frame->data, frame->len, flags, frame, cfg);
}

int conn_flush(conn_t *c) {
    while (c->obuf_head) {
        obuf_node_t *node = c->obuf_head;
        size_t remaining = node->len - node->sent;

        ssize_t n;
        if (c->transport->writev && node->next) {
            /* Keep frame buffers in queue order, including any partial head. */
            struct iovec iov[32];
            int count = 0;
            size_t bytes = 0;
            for (obuf_node_t *part = node; part && count < 32 && bytes < 65536; part = part->next) {
                size_t length = part->len - part->sent;
                if (length > 65536 - bytes)
                    length = 65536 - bytes;
                iov[count++] = (struct iovec){part->data + part->sent, length};
                bytes += length;
            }
            n = c->transport->writev(c->transport, iov, count);
        } else {
            /* TLS and transports without scatter/gather keep their write path. */
            n = c->transport->write(c->transport, node->data + node->sent, remaining);
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return -1; /* error -> close */
        }
        if (n == 0)
            return -1; /* EOF */

        c->obuf_bytes -= (size_t)n;
        size_t written = (size_t)n;
        while (written && c->obuf_head) {
            node = c->obuf_head;
            remaining = node->len - node->sent;
            size_t sent = written < remaining ? written : remaining;
            node->sent += sent;
            written -= sent;
            if (node->sent == node->len) {
                c->obuf_head = node->next;
                if (!c->obuf_head)
                    c->obuf_tail = NULL;
                release_node(node);
            }
        }
    }

    /* if queue drained, remove EPOLLOUT interest */
    if (!c->obuf_head && c->want_write) {
        struct epoll_event ev = {0};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = c->fd;
        epoll_ctl(c->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
        c->want_write = false;
    }
    return 0;
}

int conn_send_frame(conn_t *c, uint16_t type, uint16_t flags, uint32_t seq, const uint8_t *payload,
                    uint32_t plen, const server_config_t *cfg) {
    uint8_t *buf;
    ssize_t blen = frame_encode(type, flags, seq, payload, plen, &buf);
    if (blen < 0) {
        if (!(flags & FRAME_FLAG_PRIORITY_LOW))
            c->output_failed = true;
        return -1;
    }
    return conn_enqueue(c, buf, (size_t)blen, flags, cfg);
}

/* Room membership */

bool conn_is_room_member(const conn_t *c, int room_idx) {
    for (int i = 0; i < c->room_count; i++)
        if (c->rooms[i] == room_idx)
            return true;
    return false;
}

void conn_add_room(conn_t *c, int room_idx) {
    if (conn_is_room_member(c, room_idx))
        return;
    if (c->room_count < MAX_ROOMS_PER_USER)
        c->rooms[c->room_count++] = room_idx;
}

void conn_remove_room(conn_t *c, int room_idx) {
    for (int i = 0; i < c->room_count; i++) {
        if (c->rooms[i] == room_idx) {
            c->rooms[i] = c->rooms[--c->room_count];
            return;
        }
    }
}
