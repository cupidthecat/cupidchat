#ifndef SERVER_DISPATCH_H
#define SERVER_DISPATCH_H

#include "server/state.h"
#include "server/conn.h"
#include "proto/frame.h"

int dispatch_frame(conn_t *c, const frame_t *f, server_state_t *s);

#endif
