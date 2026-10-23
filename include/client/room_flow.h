#pragma once
#include "client/client_conn.h"
#include "client/ui.h"

void room_control_open(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const char *room);
void room_control_send(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const char *room,
                       const char *action, const char *account, const char *reason, bool enabled,
                       bool from_profile);
void room_control_action(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, int action);
bool room_control_frame(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const frame_t *f);
bool room_control_command(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const char *command);
