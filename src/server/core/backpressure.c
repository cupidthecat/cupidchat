/*
 * src/server/core/backpressure.c -  output-queue backpressure policy
 *
 * When a client's output queue exceeds obuf_limit bytes the server must
 * make a deterministic decision: drop low-priority frames or disconnect.
 * The low-priority drop is handled inside conn_enqueue(); this module
 * provides the higher-level "should we disconnect a slow client?" check
 * and the slow-client error frame builder used before disconnecting.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include "server/backpressure.h"
#include "server/conn.h"
#include "server/log.h"
#include "proto/tlv.h"
#include "proto/types.h"

/*
 * Returns true when the connection's output queue is so full that it
 * should be forcibly disconnected rather than accumulate more memory.
 * Threshold: 2× obuf_limit (after low-priority drops, if still filling up).
 */
bool bp_should_disconnect(const conn_t *c, const server_config_t *cfg) {
    return c->obuf_bytes > cfg->obuf_limit * 2;
}

/*
 * Build and enqueue an ERR_SLOW_CLIENT error frame for the connection.
 * The write loop will flush it and then the caller should close.
 */
void bp_send_slow_client_error(conn_t *c, const server_config_t *cfg) {
    tlv_builder_t b;
    if (tlv_builder_init(&b) < 0) return;
    tlv_put_u16(&b, TAG_ERROR_CODE, ERR_SLOW_CLIENT);
    tlv_put_str(&b, TAG_ERROR_MSG,  "client too slow -  disconnecting");
    uint32_t plen;
    uint8_t *payload = tlv_builder_take(&b, &plen);
    conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH,
                    0, payload, plen, cfg);
    free(payload);
    LOG_WARN("fd=%d nick=%s disconnecting slow client", c->fd, c->nick);
}
