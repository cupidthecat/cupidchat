#pragma once
#include "client/client_conn.h"
#include "client/ui.h"
void board_action(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, int action);
bool board_frame(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const frame_t *f);
