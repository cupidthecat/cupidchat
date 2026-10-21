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

/* The iterator counts messages; the display helpers below count terminal rows. */
void history_iter_init(history_iter_t *it,
                       const cli_msg_t *buf, int cap, int head, int len,
                       int scroll, int visible_lines) {
    memset(it, 0, sizeof(*it));
    if (!buf || cap <= 0 || len <= 0 || visible_lines <= 0) return;
    if (len > cap) len = cap;
    if (scroll < 0) scroll = 0;
    if (scroll > len) scroll = len;
    it->buf = buf;
    it->cap = cap;
    it->total = len;
    /* start index in chronological order */
    it->oldest_idx   = (cap == 0 || len == 0) ? 0
                     : (head - len + cap * 2) % cap;
    /* apply scroll: skip newest messages from the bottom */
    int skip_from_top = (len > visible_lines + scroll)
                      ? (len - visible_lines - scroll) : 0;
    it->pos          = skip_from_top;
    it->end = len - scroll;
}

/* Return next message in chronological order, or NULL when exhausted. */
const cli_msg_t *history_iter_next(history_iter_t *it) {
    if (it->pos >= it->end) return NULL;
    int ring_idx = (it->oldest_idx + it->pos) % it->cap;
    it->pos++;
    return &it->buf[ring_idx];
}

bool history_same_day(uint64_t a, uint64_t b) {
    time_t ta = (time_t)(a / 1000), tb = (time_t)(b / 1000);
    struct tm da, db;
    localtime_r(&ta, &da);
    localtime_r(&tb, &db);
    return da.tm_year == db.tm_year && da.tm_yday == db.tm_yday;
}

int history_prefix_width(const cli_msg_t *msg, int width) {
    if (width <= 1) return 0;
    int nick = (int)strnlen(msg->nick, 16);
    int base = (int)strnlen(msg->base_nick, 16);
    int prefix;
    switch (msg->type) {
        case MSG_SYSTEM: case MSG_JOIN: case MSG_PART: case MSG_QUIT:
            prefix = 10; break;
        case MSG_ACTION: prefix = 9 + nick + (base ? base + 3 : 0); break;
        default: prefix = 8 + nick + (base ? base + 3 : 0); break;
    }
    if (prefix > width - 8) prefix = width / 2;
    return prefix;
}

int history_message_rows(const cli_msg_t *msg, int width, bool ignored) {
    if (ignored || width <= 0) return 1;
    int text_width = width - history_prefix_width(msg, width);
    int length = (int)strlen(msg->text);
    return length ? (length + text_width - 1) / text_width : 1;
}

static bool ignored_message(const cli_model_t *m, const cli_msg_t *msg) {
    return (msg->type == MSG_CHAT || msg->type == MSG_ACTION) &&
           model_is_ignored(m, msg->nick);
}

int history_total_rows(const cli_msg_t *buf, int cap, int head, int len,
                       int width, const cli_model_t *m) {
    if (!buf || cap <= 0 || len <= 0 || width <= 0) return 0;
    int oldest = (head - len + cap) % cap, rows = 0;
    const cli_msg_t *prev = NULL;
    for (int i = 0; i < len; i++) {
        const cli_msg_t *msg = &buf[(oldest + i) % cap];
        if (!prev || !history_same_day(prev->timestamp_ms, msg->timestamp_ms)) rows++;
        rows += history_message_rows(msg, width, ignored_message(m, msg));
        prev = msg;
    }
    return rows;
}

/* Keep the same message and text position at the bottom when wrapping changes. */
int history_reflow_scroll(const cli_msg_t *buf, int cap, int head, int len,
                          int old_width, int new_width, int scroll,
                          const cli_model_t *m) {
    if (scroll <= 0 || old_width <= 0 || old_width == new_width || !buf || len <= 0)
        return scroll;
    int old_total = history_total_rows(buf, cap, head, len, old_width, m);
    int new_total = history_total_rows(buf, cap, head, len, new_width, m);
    int target = old_total - scroll - 1;
    if (target < 0) target = 0;
    int oldest = (head - len + cap) % cap, old_row = 0, new_row = 0;
    const cli_msg_t *prev = NULL;
    for (int i = 0; i < len; i++) {
        const cli_msg_t *msg = &buf[(oldest + i) % cap];
        if (!prev || !history_same_day(prev->timestamp_ms, msg->timestamp_ms)) {
            if (target == old_row) return new_total - new_row - 1;
            old_row++; new_row++;
        }
        bool ignored = ignored_message(m, msg);
        int old_h = history_message_rows(msg, old_width, ignored);
        int new_h = history_message_rows(msg, new_width, ignored);
        if (target < old_row + old_h) {
            int bytes = (target - old_row) * (old_width - history_prefix_width(msg, old_width));
            int line = bytes / (new_width - history_prefix_width(msg, new_width));
            if (line >= new_h) line = new_h - 1;
            return new_total - new_row - line - 1;
        }
        old_row += old_h; new_row += new_h; prev = msg;
    }
    return 0;
}
