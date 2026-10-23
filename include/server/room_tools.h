#pragma once
#include "server/state.h"

int server_room_role(server_state_t *s, const room_t *room, uint64_t account, uint8_t *role);
int server_room_info(server_state_t *s, conn_t *c, int ri, uint32_t request, const char *message,
                     bool details);
int server_room_command(server_state_t *s, conn_t *c, const frame_t *f);
void server_room_invites(server_state_t *s, conn_t *c);
