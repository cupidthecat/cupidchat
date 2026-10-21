/* CupidChat public interface. */
#pragma once
#include "proto/frame.h"
#include "server/state.h"

int dispatch_frame(conn_t *c, const frame_t *f, server_state_t *s);
