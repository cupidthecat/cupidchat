#pragma once
#include "server/state.h"

int server_send_buddies(server_state_t *s, conn_t *c);
void server_buddy_presence(server_state_t *s, uint64_t account, bool offline);
int server_buddy_command(server_state_t *s, conn_t *c, const frame_t *f);
