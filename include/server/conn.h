#ifndef SERVER_CONN_H
#define SERVER_CONN_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "proto/frame.h"
#include "server/rate_limit.h"
#include "server/config.h"
#include "net/transport.h"

#define MAX_ROOMS_PER_USER 64
#define FRAME_FLAG_PRIORITY_LOW  0x01


typedef enum {
    CONN_HANDSHAKE = 0,
    CONN_AUTHED,
    CONN_CLOSING
} conn_state_t;

typedef struct obuf_node_t {
    uint8_t *data;
    size_t len;
    size_t sent;
    uint16_t flags;
    struct obuf_node_t *next;
} obuf_node_t;

typedef struct conn_t {
    int fd;
    transport_t *transport;
    int epoll_fd;
    conn_state_t state;
    uint32_t user_id;
    char nick[33];
    char base_nick[33];
    bool is_away;
    char away_msg[64];
    bool registered;
    
    frame_parser_t parser;
    rate_bucket_t rate;
    time_t last_active;
    bool ping_pending;
    time_t ping_sent_at;
    
    obuf_node_t *obuf_head;
    obuf_node_t *obuf_tail;
    size_t obuf_bytes;
    bool want_write;
    
    int rooms[MAX_ROOMS_PER_USER];
    int room_count;
} conn_t;

conn_t *conn_create(int fd, transport_t *t, int epoll_fd, const server_config_t *cfg);
void conn_destroy(conn_t *c);
int conn_enqueue(conn_t *c, uint8_t *buf, size_t len, uint16_t flags, const server_config_t *cfg);
int conn_flush(conn_t *c);
int conn_send_frame(conn_t *c, uint16_t type, uint16_t flags, uint32_t seq,
                    const uint8_t *payload, uint32_t plen, const server_config_t *cfg);
bool conn_is_room_member(const conn_t *c, int room_idx);
void conn_add_room(conn_t *c, int room_idx);
void conn_remove_room(conn_t *c, int room_idx);

#endif
