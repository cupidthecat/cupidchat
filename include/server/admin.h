#pragma once
#include "server/state.h"

int server_admin_command(server_state_t *s, conn_t *c, const frame_t *f);
int server_poll_command(server_state_t *s, conn_t *c, const frame_t *f);
int server_account_flags(server_state_t *s, conn_t *c);
bool server_chat_allowed(server_state_t *s, conn_t *c);
