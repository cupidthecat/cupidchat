/*
 * src/server/core/keepalive.c -  ping/pong keepalive and idle timeout
 *
 * Called from the timerfd tick in the main event loop.
 * For each AUTHED connection:
 *  - if idle > ping_interval and no ping pending -> send PING
 *  - if ping pending and (now - ping_sent_at) > ping_timeout -> disconnect
 */

#include <time.h>
#include <stdlib.h>

#include "server/keepalive.h"
#include "server/state.h"
#include "server/conn.h"
#include "server/log.h"
#include "proto/tlv.h"
#include "proto/types.h"

static void send_ping(conn_t *c, server_state_t *s) {
    uint32_t seq = server_next_seq(s);
    conn_send_frame(c, SMSG_PING, FRAME_FLAG_PRIORITY_HIGH,
                    seq, NULL, 0, s->cfg);
    c->ping_pending  = true;
    c->ping_sent_at  = time(NULL);
    LOG_DEBUG("fd=%d PING seq=%u", c->fd, seq);
}

void keepalive_tick(server_state_t *s) {
    time_t now = time(NULL);

    for (int fd = 0; fd < s->conns_size; fd++) {
        conn_t *c = s->conns[fd];
        if (!c || c->state != CONN_AUTHED) continue;

        int idle = (int)(now - c->last_active);

        if (c->ping_pending) {
            int wait = (int)(now - c->ping_sent_at);
            if (wait >= s->cfg->ping_timeout) {
                LOG_INFO("fd=%d nick=%s ping timeout, disconnecting",
                         c->fd, c->nick);
                c->state = CONN_CLOSING;
            }
        } else if (idle >= s->cfg->ping_interval) {
            send_ping(c, s);
        }
    }
}
