/* CupidChat public interface. */
#pragma once
#include "net/transport.h"
#include "proto/frame.h"
#include "server/config.h"
#include "server/rate_limit.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MAX_ROOMS_PER_USER 64

typedef enum { CONN_HANDSHAKE, CONN_AUTHED, CONN_CLOSING } conn_state_t;

typedef struct obuf_node {
    uint8_t *data;
    size_t len, sent;
    uint16_t flags;
    struct obuf_node *next;
} obuf_node_t;

typedef struct {
    int fd, epoll_fd;
    transport_t *transport;
    conn_state_t state;
    uint32_t user_id;
    uint64_t account_id;
    char nick[MAX_NICK_LEN + 1], base_nick[MAX_NICK_LEN + 1];
    bool registered, is_away;
    char away_msg[256];
    frame_parser_t parser;
    rate_bucket_t rate;
    time_t last_active, ping_sent_at, close_deadline;
    bool ping_pending, want_write;
    obuf_node_t *obuf_head, *obuf_tail;
    size_t obuf_bytes;
    int rooms[MAX_ROOMS_PER_USER], room_count;
} conn_t;

conn_t *conn_create(int fd, transport_t *t, int epoll_fd, const server_config_t *cfg);
void conn_destroy(conn_t *c);
int conn_enqueue(conn_t *c, uint8_t *buf, size_t len, uint16_t flags, const server_config_t *cfg);
int conn_flush(conn_t *c);
int conn_send_frame(conn_t *c, uint16_t type, uint16_t flags, uint32_t seq, const uint8_t *payload,
                    uint32_t plen, const server_config_t *cfg);
bool conn_is_room_member(const conn_t *c, int room_idx);
void conn_add_room(conn_t *c, int room_idx);
void conn_remove_room(conn_t *c, int room_idx);
