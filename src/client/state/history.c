/*
 * src/client/state/history.c -  ring-buffer iteration helpers for the UI
 *
 * The ring buffers (cli_room_t.msgs, cli_dm_t.msgs) store messages with
 * msgs_head pointing at the *next write slot*.  The oldest message is at
 * (msgs_head - msgs_len + msgs_cap) % msgs_cap.
 *
 * history_iter_init() + history_iter_next() walk the ring in chronological
 * order so the render code can simply iterate forward.
 */

#include <string.h>
#include "client/history.h"

/* Initialise an iterator over a cli_room_t or cli_dm_t message ring.
   cap / head / len / msgs are the ring fields; scroll is lines from bottom. */
void history_iter_init(history_iter_t *it,
                       const cli_msg_t *buf, int cap, int head, int len,
                       int scroll, int visible_lines) {
    it->buf          = buf;
    it->cap          = cap;
    it->total        = len;
    /* start index in chronological order */
    it->oldest_idx   = (cap == 0 || len == 0) ? 0
                     : (head - len + cap * 2) % cap;
    /* apply scroll: skip newest messages from the bottom */
    int skip_from_top = (len > visible_lines + scroll)
                      ? (len - visible_lines - scroll) : 0;
    it->pos          = skip_from_top;
    it->end          = (len - scroll > visible_lines)
                     ? (skip_from_top + visible_lines) : len;
    if (it->end > len) it->end = len;
}

/* Return next message in chronological order, or NULL when exhausted. */
const cli_msg_t *history_iter_next(history_iter_t *it) {
    if (it->pos >= it->end) return NULL;
    int ring_idx = (it->oldest_idx + it->pos) % it->cap;
    it->pos++;
    return &it->buf[ring_idx];
}
