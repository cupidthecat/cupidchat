/* CupidChat public interface. */
#pragma once
#include "net/transport.h"
#include "proto/frame.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct client_output client_output_t;

typedef struct {
    int fd;
    uint32_t seq;
    transport_t *transport;
    frame_parser_t parser;
    bool connected;
    bool send_failed;
    client_output_t *output_head, *output_tail;
    size_t output_bytes;
} client_conn_t;

int client_connect(client_conn_t *cc, const char *host, uint16_t port, bool use_tls);
void client_disconnect(client_conn_t *cc);
int client_flush(client_conn_t *cc);
int client_send_frame(client_conn_t *cc, uint16_t type, const uint8_t *payload, uint32_t plen);
int client_send_hello(client_conn_t *cc, const char *nick);
int client_send_nick(client_conn_t *cc, const char *nick);
int client_send_register(client_conn_t *cc, const char *nick, const char *pass);
int client_send_login(client_conn_t *cc, const char *nick, const char *pass);
int client_send_logout(client_conn_t *cc);
int client_send_away(client_conn_t *cc, const char *msg);
int client_send_typing(client_conn_t *cc, const char *room);
int client_send_join(client_conn_t *cc, const char *room);
int client_send_leave(client_conn_t *cc, const char *room);
int client_send_room_msg(client_conn_t *cc, const char *room, const char *text);
int client_send_room_action(client_conn_t *cc, const char *room, const char *text);
int client_send_set_topic(client_conn_t *cc, const char *room, const char *topic);
int client_send_dm(client_conn_t *cc, const char *target_nick, const char *text);
int client_send_dm_to(client_conn_t *cc, const char *target_nick, const char *text, uint32_t uid);
int client_send_list_rooms(client_conn_t *cc);
int client_send_delete_room(client_conn_t *cc, const char *room);
int client_send_list_users(client_conn_t *cc, const char *room);
int client_send_pong(client_conn_t *cc);
int client_send_buddy(client_conn_t *cc, uint16_t type, const char *account);
int client_read_frames(client_conn_t *cc, void (*on_frame)(const frame_t *, void *ctx), void *ctx);
