#ifndef SERVER_STATE_H
#define SERVER_STATE_H

#include "server/config.h"
#include "server/conn.h"
#include "server/room.h"
#include "server/db.h"

#define MAX_ROOMS 1024

typedef struct {
    const server_config_t *cfg;
    uint32_t next_uid;
    uint32_t next_seq;
    int epoll_fd;
    int listen_fd_plain;
    int listen_fd_tls;
    int timer_fd;
    int conns_size;
    conn_t **conns;
    room_t **rooms;
    cupid_db_t *db;
    int room_count;
    int conn_count;
} server_state_t;

int server_state_init(server_state_t *s, const server_config_t *cfg);
void server_state_free(server_state_t *s);

void server_register_conn(server_state_t *s, conn_t *c);
void server_unregister_conn(server_state_t *s, conn_t *c);

conn_t *server_find_by_nick(server_state_t *s, const char *nick);
conn_t *server_find_by_uid(server_state_t *s, uint32_t uid);

int server_find_room(server_state_t *s, const char *name);
int server_get_or_create_room(server_state_t *s, const char *name, uint32_t owner_uid);
void server_broadcast_room(server_state_t *s, int room_idx, int skip_fd,
                           uint8_t *buf, size_t len, uint16_t flags);
void server_delete_room(server_state_t *s, int ri);

uint32_t server_next_uid(server_state_t *s);
uint32_t server_next_seq(server_state_t *s);

#endif
