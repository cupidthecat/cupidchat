#ifndef SERVER_BACKPRESSURE_H
#define SERVER_BACKPRESSURE_H

#include "server/conn.h"
#include "server/config.h"

bool bp_should_disconnect(const conn_t *c, const server_config_t *cfg);
void bp_send_slow_client_error(conn_t *c, const server_config_t *cfg);

#endif
