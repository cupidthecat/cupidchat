/* CupidChat public interface. */
#pragma once
#include "server/config.h"
#include "server/conn.h"
#include "server/db.h"
#include "server/room.h"

typedef struct {
    const server_config_t *cfg;
    uint32_t next_uid, next_seq;
    int epoll_fd, listen_fd_plain, listen_fd_tls, timer_fd;
    conn_t **conns;
    int conns_size, conn_count;
    room_t **rooms;
    int room_count;
    cupid_db_t *db;
} server_state_t;

int server_state_init(server_state_t *s, const server_config_t *cfg);
void server_state_free(server_state_t *s);
void server_register_conn(server_state_t *s, conn_t *c);
void server_unregister_conn(server_state_t *s, conn_t *c);
conn_t *server_find_by_nick(server_state_t *s, const char *nick);
conn_t *server_find_by_uid(server_state_t *s, uint32_t uid);
int server_find_room(server_state_t *s, const char *name);
int server_get_or_create_room(server_state_t *s, const char *name, uint32_t owner_uid,
                              uint64_t owner_account_id);
uint32_t server_room_owner_uid(server_state_t *s, const room_t *room);
conn_t *server_find_by_account(server_state_t *s, uint64_t account_id);
void server_broadcast_room(server_state_t *s, int room_idx, int skip_fd, uint8_t *buf, size_t len,
                           uint16_t flags);
uint32_t server_next_uid(server_state_t *s);
uint32_t server_next_seq(server_state_t *s);
int server_delete_room(server_state_t *s, int ri);
