/* CupidChat public interface. */
#pragma once
#include "server/conn.h"

bool bp_should_disconnect(const conn_t *c, const server_config_t *cfg);
void bp_send_slow_client_error(conn_t *c, const server_config_t *cfg);
