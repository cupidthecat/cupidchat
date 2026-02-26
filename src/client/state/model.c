/*
 * src/client/state/model.c -  client application state updates
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasestr */
#include <time.h>

#include "client/model.h"

/* Lifecycle */

void model_init(cli_model_t *m) {
    memset(m, 0, sizeof(*m));
    m->active_room = -1;
    m->active_dm   = -1;
    m->status = CLI_DISCONNECTED;
}

void model_free(cli_model_t *m) {
    for (int i = 0; i < m->room_count; i++) {
        free(m->rooms[i].msgs);
        m->rooms[i].msgs = NULL;
    }
    for (int i = 0; i < m->dm_count; i++) {
        free(m->dms[i].msgs);
        m->dms[i].msgs = NULL;
    }
}

/* Room helpers */

cli_room_t *model_find_room(cli_model_t *m, const char *name) {
    for (int i = 0; i < m->room_count; i++)
        if (m->rooms[i].active && strcmp(m->rooms[i].name, name) == 0)
            return &m->rooms[i];
    return NULL;
}

cli_room_t *model_get_or_create_room(cli_model_t *m, const char *name) {
    cli_room_t *r = model_find_room(m, name);
    if (r) return r;
    if (m->room_count >= CLI_MAX_ROOMS) return NULL;

    r = &m->rooms[m->room_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, CLI_ROOM_MAX - 1);
    r->active    = true;
    r->msgs_cap  = CLI_HISTORY_CAP;
    r->msgs      = calloc((size_t)r->msgs_cap, sizeof(cli_msg_t));
    if (!r->msgs) {
        /* OOM -  roll back the slot so callers get NULL rather than a
         * room that silently drops every message pushed into it. */
        memset(r, 0, sizeof(*r));
        m->room_count--;
        return NULL;
    }
    return r;
}

static void room_push_msg(cli_room_t *r, uint32_t uid, const char *nick,
                          const char *base_nick,
                          const char *text, uint64_t ts_ms, bool self,
                          cli_msg_type_t type, const char *my_nick) {
    if (!r->msgs) return;
    cli_msg_t *m = &r->msgs[r->msgs_head];
    memset(m, 0, sizeof(*m));
    m->user_id      = uid;
    m->timestamp_ms = ts_ms;
    m->is_self      = self;
    m->type         = type;
    strncpy(m->nick, nick, CLI_NICK_MAX - 1);
    if (base_nick && base_nick[0] && strcmp(base_nick, nick) != 0)
        strncpy(m->base_nick, base_nick, CLI_NICK_MAX - 1);
    strncpy(m->text, text, CLI_MSG_MAX - 1);
    /* set highlight if someone mentioned our nick (never self-highlight) */
    m->is_highlight = !self && my_nick && my_nick[0]
                      && strcasestr(text, my_nick) != NULL;

    r->msgs_head = (r->msgs_head + 1) % r->msgs_cap;
    if (r->msgs_len < r->msgs_cap) r->msgs_len++;
    /* system events don't bump unread -  only real chat does */
    if (!self && type == MSG_CHAT) r->unread++;
}

void model_push_room_msg(cli_model_t *m, const char *room,
                         uint32_t uid, const char *nick,
                         const char *base_nick,
                         const char *text, uint64_t ts_ms, bool self,
                         const char *my_nick) {
    cli_room_t *r = model_get_or_create_room(m, room);
    if (!r) return;
    room_push_msg(r, uid, nick, base_nick, text, ts_ms, self, MSG_CHAT, my_nick);
}

void model_push_room_msg_typed(cli_model_t *m, const char *room,
                                uint32_t uid, const char *nick,
                                const char *base_nick,
                                const char *text, uint64_t ts_ms,
                                bool self, cli_msg_type_t type,
                                const char *my_nick) {
    cli_room_t *r = model_get_or_create_room(m, room);
    if (!r) return;
    room_push_msg(r, uid, nick, base_nick, text, ts_ms, self, type, my_nick);
}

void model_push_system_msg(cli_model_t *m, const char *room,
                           const char *text, uint64_t ts_ms,
                           cli_msg_type_t type) {
    cli_room_t *r = model_get_or_create_room(m, room);
    if (!r) return;
    room_push_msg(r, 0, "", "", text, ts_ms, false, type, NULL);
}

void model_clear_room_msgs(cli_model_t *m, const char *room) {
    cli_room_t *r = model_find_room(m, room);
    if (!r) return;
    r->msgs_len  = 0;
    r->msgs_head = 0;
    r->unread    = 0;
    r->scroll_offset = 0;
}

bool model_is_ignored(const cli_model_t *m, const char *nick) {
    for (int i = 0; i < m->ignore_count; i++)
        if (strcasecmp(m->ignore_list[i], nick) == 0) return true;
    return false;
}

/* DM helpers */

cli_dm_t *model_get_or_create_dm(cli_model_t *m, uint32_t uid,
                                  const char *nick) {
    for (int i = 0; i < m->dm_count; i++)
        if (m->dms[i].peer_uid == uid) return &m->dms[i];

    if (m->dm_count >= 64) return NULL;
    cli_dm_t *d = &m->dms[m->dm_count++];
    memset(d, 0, sizeof(*d));
    d->peer_uid  = uid;
    strncpy(d->peer_nick, nick, CLI_NICK_MAX - 1);
    d->msgs_cap  = CLI_HISTORY_CAP;
    d->msgs      = calloc((size_t)d->msgs_cap, sizeof(cli_msg_t));
    if (!d->msgs) {
        /* OOM -  roll back so the caller gets NULL */
        memset(d, 0, sizeof(*d));
        m->dm_count--;
        return NULL;
    }
    return d;
}

void model_push_dm_msg(cli_model_t *m, uint32_t uid, const char *nick,
                       const char *text, uint64_t ts_ms, bool self) {
    cli_dm_t *d = model_get_or_create_dm(m, uid, nick);
    if (!d || !d->msgs) return;

    cli_msg_t *msg = &d->msgs[d->msgs_head];
    memset(msg, 0, sizeof(*msg));
    msg->user_id      = uid;
    msg->timestamp_ms = ts_ms;
    msg->is_self      = self;
    msg->is_dm        = true;
    msg->type         = MSG_CHAT;
    msg->is_highlight = !self;   /* all inbound DMs are highlights */
    /* For outgoing messages store our own nick; for incoming store sender's. */
    const char *src_nick = self ? m->nick : nick;
    snprintf(msg->nick, CLI_NICK_MAX, "%s", src_nick);
    strncpy(msg->text, text, CLI_MSG_MAX - 1);
    d->msgs_head = (d->msgs_head + 1) % d->msgs_cap;
    if (d->msgs_len < d->msgs_cap) d->msgs_len++;
    if (!self) d->unread++;
}

/* Status message */

void model_set_status(cli_model_t *m, const char *msg, int secs) {
    strncpy(m->status_msg, msg, sizeof(m->status_msg) - 1);
    m->status_msg_until = time(NULL) + secs;
}

/* User list */

void model_update_users(cli_model_t *m, const char *room,
                        cli_user_t *users, int count) {
    (void)room;  /* for now update the flat global user list */
    int n = count < CLI_MAX_USERS ? count : CLI_MAX_USERS;
    memcpy(m->users, users, (size_t)n * sizeof(cli_user_t));
    m->user_count = n;
}
