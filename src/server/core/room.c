/*
 * src/server/core/room.c -  room lifecycle, membership, and history
 */

#include <stdlib.h>
#include <string.h>

#include "server/room.h"
#include "server/log.h"

room_t *room_create(const char *name, int history_cap) {
    room_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    strncpy(r->name, name, sizeof(r->name) - 1);
    r->active       = true;
    r->history_cap  = history_cap > 0 ? history_cap : 50;
    r->history_head = 0;
    r->history_len  = 0;

    r->history = calloc((size_t)r->history_cap, sizeof(history_entry_t));
    if (!r->history) { free(r); return NULL; }

    return r;
}

void room_destroy(room_t *r) {
    if (!r) return;
    free(r->history);
    free(r);
}

bool room_add_member(room_t *r, int fd) {
    if (r->member_count >= ROOM_MEMBER_SLOTS) return false;
    for (int i = 0; i < r->member_count; i++)
        if (r->member_fds[i] == fd) return true;  /* already in */
    r->member_fds[r->member_count++] = fd;
    return true;
}

bool room_remove_member(room_t *r, int fd) {
    for (int i = 0; i < r->member_count; i++) {
        if (r->member_fds[i] == fd) {
            r->member_fds[i] = r->member_fds[--r->member_count];
            return true;
        }
    }
    return false;
}

bool room_has_member(const room_t *r, int fd) {
    for (int i = 0; i < r->member_count; i++)
        if (r->member_fds[i] == fd) return true;
    return false;
}

void room_history_push(room_t *r, uint32_t uid, const char *nick,
                       const char *text, uint64_t ts_ms) {
    history_entry_t *e = &r->history[r->history_head];
    e->user_id      = uid;
    e->timestamp_ms = ts_ms;
    strncpy(e->nick, nick, sizeof(e->nick) - 1);
    strncpy(e->text, text, sizeof(e->text) - 1);

    r->history_head = (r->history_head + 1) % r->history_cap;
    if (r->history_len < r->history_cap) r->history_len++;
}
