/* Backpressure must never truncate a frame already started on TCP. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "server/conn.h"

static int calls;
static ssize_t partial_write(transport_t *t, const void *buf, size_t length) {
    if (!calls++) return 2000;
    errno = EAGAIN;
    return -1;
}

static uint8_t *buffer(size_t length) {
    uint8_t *data = calloc(1, length);
    assert(data);
    return data;
}

static void clear(conn_t *c) {
    while (c->obuf_head) {
        obuf_node_t *node = c->obuf_head;
        c->obuf_head = node->next;
        free(node->data); free(node);
    }
    c->obuf_tail = NULL;
    c->obuf_bytes = 0;
}

int main(void) {
    server_config_t cfg = {.obuf_limit = 4096};
    transport_t transport = {.write = partial_write};
    conn_t c = {.fd = -1, .epoll_fd = -1, .want_write = true, .transport = &transport};
    assert(conn_enqueue(&c, buffer(4090), 4090, FRAME_FLAG_PRIORITY_LOW, &cfg) == 0);
    assert(conn_flush(&c) == 0);
    assert(c.obuf_head->sent == 2000 && c.obuf_bytes == 2090);
    assert(conn_enqueue(&c, buffer(3000), 3000, FRAME_FLAG_PRIORITY_HIGH, &cfg) == -1);
    assert(c.obuf_head == c.obuf_tail && c.obuf_head->sent == 2000 && c.obuf_bytes == 2090);
    clear(&c);
    puts("  PASS  partially sent low priority frame survives pressure");

    calls = 0;
    assert(conn_enqueue(&c, buffer(3000), 3000, FRAME_FLAG_PRIORITY_LOW, &cfg) == 0);
    assert(conn_flush(&c) == 0);
    assert(conn_enqueue(&c, buffer(2000), 2000, FRAME_FLAG_PRIORITY_LOW, &cfg) == 0);
    assert(conn_enqueue(&c, buffer(1500), 1500, FRAME_FLAG_PRIORITY_HIGH, &cfg) == 0);
    assert(c.obuf_head->sent == 2000 && c.obuf_head->next == c.obuf_tail);
    assert(c.obuf_tail->flags == FRAME_FLAG_PRIORITY_HIGH && c.obuf_bytes == 2500);
    clear(&c);
    puts("  PASS  unsent low priority frames can be evicted behind a partial frame");

    assert(conn_enqueue(&c, buffer(3000), 3000, FRAME_FLAG_PRIORITY_LOW, &cfg) == 0);
    assert(conn_enqueue(&c, buffer(2000), 2000, FRAME_FLAG_PRIORITY_HIGH, &cfg) == 0);
    assert(c.obuf_head == c.obuf_tail && c.obuf_bytes == 2000);
    clear(&c);
    puts("  PASS  normal eviction of an unsent frame still works");
    return 0;
}
