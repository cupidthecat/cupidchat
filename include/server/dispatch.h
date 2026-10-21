/* CupidChat public interface. */
#pragma once
#include "server/state.h"
#include "proto/frame.h"

int dispatch_frame(conn_t *c, const frame_t *f, server_state_t *s);
