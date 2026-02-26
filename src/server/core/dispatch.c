/*
 * src/server/core/dispatch.c -  message dispatcher (protocol handler)
 *
 * Receives a fully-parsed frame from a connection and executes the
 * appropriate application logic.  Returns 0 on success, -1 if the
 * connection should be closed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server/dispatch.h"
#include "server/state.h"
#include "server/conn.h"
#include "server/room.h"
#include "server/log.h"
#include "server/rate_limit.h"
#include "server/db.h"
#include "proto/frame.h"
#include "proto/tlv.h"
#include "proto/types.h"

/* Helpers */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int send_error(conn_t *c, error_code_t code, const char *msg,
                      server_state_t *s) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u16(&b, TAG_ERROR_CODE, (uint16_t)code);
    tlv_put_str(&b, TAG_ERROR_MSG,  msg);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    int r = conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH,
                            server_next_seq(s), p, plen, s->cfg);
    free(p);
    return r;
}

static bool nick_valid(const char *nick) {
    if (!nick || !*nick) return false;
    size_t len = 0;
    for (const char *p = nick; *p; p++, len++) {
        char ch = *p;
        if (!(ch == '_' || ch == '-' ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9'))) return false;
    }
    return len >= 1 && len <= MAX_NICK_LEN;
}

static bool room_valid(const char *name) {
    if (!name || !*name) return false;
    size_t len = 0;
    for (const char *p = name; *p; p++, len++) {
        char ch = *p;
        if (!(ch == '_' || ch == '-' ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9'))) return false;
    }
    return len >= 1 && len <= MAX_ROOM_LEN;
}

/* Handler: CMSG_HELLO */

static int handle_hello(conn_t *c, const frame_t *f, server_state_t *s) {
    if (c->state != CONN_HANDSHAKE) {
        return send_error(c, ERR_PROTOCOL, "already authenticated", s);
    }

    uint16_t version = 0;
    char nick[MAX_NICK_LEN + 1] = {0};
    tlv_read_u16(f->payload, f->hdr.length, TAG_PROTO_VERSION, &version);
    tlv_read_str(f->payload, f->hdr.length, TAG_NICK, nick, sizeof(nick));

    if (version != CUPID_PROTO_VERSION) {
        send_error(c, ERR_PROTOCOL, "unsupported protocol version", s);
        return -1;
    }
    if (!nick_valid(nick)) {
        send_error(c, ERR_NICK_INVALID, "invalid nick (a-z A-Z 0-9 _ -)", s);
        return -1;
    }
    if (server_find_by_nick(s, nick)) {
        send_error(c, ERR_NICK_TAKEN, "nick already in use", s);
        return -1;
    }

    snprintf(c->nick,      sizeof(c->nick),      "%s", nick);
    snprintf(c->base_nick, sizeof(c->base_nick), "%s", nick);
    c->user_id = server_next_uid(s);
    c->state   = CONN_AUTHED;

    /* send WELCOME */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_u16(&b, TAG_PROTO_VERSION, CUPID_PROTO_VERSION);
    tlv_put_u32(&b, TAG_SESSION_ID,   c->user_id);
    tlv_put_str(&b, TAG_SERVER_NAME,  CUPID_SERVER_NAME);
    tlv_put_str(&b, TAG_MOTD,
        "  ____            _     _  ____  _           _\n"
        " / ___|   _ _ __ (_) __| |/ ___|| |__   __ _| |_\n"
        "| |  | | | | '_ \\| |/ _` | |    | '_ \\ / _` | __|\n"
        "| |__| |_| | |_) | | (_| | |___ | | | | (_| | |_\n"
        " \\____\\__,_| .__/|_|\\__,_|\\____||_| |_|\\__,_|\\__|\n"
        "            |_|        Welcome to CupidChat v1.0\n"
        "\n"
        "Type /help for a list of commands.");
    tlv_put_u32(&b, TAG_CAPS,         CAP_HISTORY);  /* TLS/files when ready */
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    conn_send_frame(c, SMSG_WELCOME, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), p, plen, s->cfg);
    free(p);

    LOG_INFO("fd=%d nick=%s uid=%u authenticated", c->fd, c->nick, c->user_id);
    return 0;
}

/* Handler: CMSG_JOIN */

static int handle_join(conn_t *c, const frame_t *f, server_state_t *s) {
    char room_name[MAX_ROOM_LEN + 1] = {0};
    if (!tlv_read_str(f->payload, f->hdr.length, TAG_ROOM,
                      room_name, sizeof(room_name))) {
        return send_error(c, ERR_PROTOCOL, "missing room tag", s);
    }
    if (!room_valid(room_name))
        return send_error(c, ERR_PROTOCOL, "invalid room name (a-z A-Z 0-9 _ -)", s);

    bool is_new = (server_find_room(s, room_name) < 0);
    int ri = server_get_or_create_room(s, room_name, c->user_id);
    if (ri < 0) return send_error(c, ERR_UNKNOWN, "too many rooms", s);

    /* if we just created the room, broadcast SMSG_ROOM_CREATED to all clients */
    if (is_new) {
        tlv_builder_t nc; tlv_builder_init(&nc);
        tlv_put_str(&nc, TAG_ROOM,      room_name);
        tlv_put_u32(&nc, TAG_OWNER_UID, c->user_id);
        uint32_t ncplen; uint8_t *ncp = tlv_builder_take(&nc, &ncplen);
        uint8_t *ncraw; ssize_t ncrlen =
            frame_encode(SMSG_ROOM_CREATED, FRAME_FLAG_PRIORITY_HIGH,
                         server_next_seq(s), ncp, ncplen, &ncraw);
        free(ncp);
        if (ncrlen > 0) {
            /* send to every authed connection so all clients see the new room */
            for (int i = 0; i < s->conns_size; i++) {
                conn_t *m = s->conns[i];
                if (!m || m->state != CONN_AUTHED) continue;
                uint8_t *copy = malloc((size_t)ncrlen);
                if (copy) { memcpy(copy, ncraw, (size_t)ncrlen);
                    conn_enqueue(m, copy, (size_t)ncrlen,
                                 FRAME_FLAG_PRIORITY_HIGH, s->cfg); }
            }
            free(ncraw);
        }
    }

    if (!conn_is_room_member(c, ri)) {
        conn_add_room(c, ri);
        room_add_member(s->rooms[ri], c->fd);

        /* broadcast USER_JOINED to existing members */
        tlv_builder_t b; tlv_builder_init(&b);
        tlv_put_str(&b, TAG_ROOM,    room_name);
        tlv_put_str(&b, TAG_NICK,    c->nick);
        tlv_put_u32(&b, TAG_USER_ID, c->user_id);
        if (strcmp(c->nick, c->base_nick) != 0)
            tlv_put_str(&b, TAG_BASE_NICK, c->base_nick);
        uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
        uint8_t *raw; ssize_t rlen =
            frame_encode(SMSG_USER_JOINED, FRAME_FLAG_PRIORITY_HIGH,
                         server_next_seq(s), p, plen, &raw);
        free(p);
        if (rlen > 0) {
            server_broadcast_room(s, ri, c->fd, raw, (size_t)rlen,
                                  FRAME_FLAG_PRIORITY_HIGH);
            free(raw);
        }

        /* send room history to the joiner */
        room_t *r = s->rooms[ri];
        int hist_start = (r->history_len == r->history_cap)
                       ? r->history_head : 0;
        for (int i = 0; i < r->history_len; i++) {
            int idx = (hist_start + i) % r->history_cap;
            history_entry_t *e = &r->history[idx];
            tlv_builder_t hb; tlv_builder_init(&hb);
            tlv_put_str(&hb, TAG_ROOM,      room_name);
            tlv_put_str(&hb, TAG_NICK,      e->nick);
            tlv_put_u32(&hb, TAG_USER_ID,   e->user_id);
            tlv_put_str(&hb, TAG_TEXT,      e->text);
            tlv_put_u64(&hb, TAG_TIMESTAMP, e->timestamp_ms);
            uint32_t hplen; uint8_t *hp = tlv_builder_take(&hb, &hplen);
            conn_send_frame(c, SMSG_ROOM_HISTORY, FRAME_FLAG_PRIORITY_HIGH,
                            server_next_seq(s), hp, hplen, s->cfg);
            free(hp);
        }
        LOG_INFO("fd=%d nick=%s joined #%s", c->fd, c->nick, room_name);
    }    /* always deliver the current topic to the joiner */
    if (s->rooms[ri]->topic[0]) {
        tlv_builder_t tb; tlv_builder_init(&tb);
        tlv_put_str(&tb, TAG_ROOM,  room_name);
        tlv_put_str(&tb, TAG_TOPIC, s->rooms[ri]->topic);
        tlv_put_str(&tb, TAG_NICK,  s->rooms[ri]->topic_set_by);
        uint32_t tplen; uint8_t *tp = tlv_builder_take(&tb, &tplen);
        conn_send_frame(c, SMSG_ROOM_TOPIC, FRAME_FLAG_PRIORITY_HIGH,
                        server_next_seq(s), tp, tplen, s->cfg);
        free(tp);
    }    return 0;
}

/* Handler: CMSG_LEAVE */

static int handle_leave(conn_t *c, const frame_t *f, server_state_t *s) {
    char room_name[MAX_ROOM_LEN + 1] = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_ROOM, room_name, sizeof(room_name));

    int ri = server_find_room(s, room_name);
    if (ri < 0 || !conn_is_room_member(c, ri))
        return send_error(c, ERR_NOT_IN_ROOM, "not in room", s);

    conn_remove_room(c, ri);
    room_remove_member(s->rooms[ri], c->fd);

    /* broadcast USER_LEFT */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM,    room_name);
    tlv_put_str(&b, TAG_NICK,    c->nick);
    tlv_put_u32(&b, TAG_USER_ID, c->user_id);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    uint8_t *raw; ssize_t rlen =
        frame_encode(SMSG_USER_LEFT, FRAME_FLAG_PRIORITY_HIGH,
                     server_next_seq(s), p, plen, &raw);
    free(p);
    if (rlen > 0) {
        server_broadcast_room(s, ri, -1, raw, (size_t)rlen, FRAME_FLAG_PRIORITY_HIGH);
        free(raw);
    }
    LOG_INFO("fd=%d nick=%s left #%s", c->fd, c->nick, room_name);
    return 0;
}

/* Handler: CMSG_ROOM_MSG */

static int handle_room_msg(conn_t *c, const frame_t *f, server_state_t *s) {
    if (!rate_consume(&c->rate)) {
        return send_error(c, ERR_RATE_LIMITED, "slow down", s);
    }

    char room_name[MAX_ROOM_LEN + 1] = {0};
    char text[MAX_MSG_LEN + 1]       = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_ROOM, room_name, sizeof(room_name));
    tlv_read_str(f->payload, f->hdr.length, TAG_TEXT, text,      sizeof(text));

    if (!*text) return 0;

    int ri = server_find_room(s, room_name);
    if (ri < 0 || !conn_is_room_member(c, ri))
        return send_error(c, ERR_NOT_IN_ROOM, "not in room", s);

    uint64_t ts = now_ms();
    room_history_push(s->rooms[ri], c->user_id, c->nick, text, ts);
    db_persist_message(s->db, room_name, c->user_id, c->nick, text, ts);

    /* build broadcast frame */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM,      room_name);
    tlv_put_str(&b, TAG_NICK,      c->nick);
    tlv_put_u32(&b, TAG_USER_ID,   c->user_id);
    tlv_put_str(&b, TAG_TEXT,      text);
    tlv_put_u64(&b, TAG_TIMESTAMP, ts);
    /* include base_nick when the user has renamed away from their account name */
    if (strcmp(c->nick, c->base_nick) != 0)
        tlv_put_str(&b, TAG_BASE_NICK, c->base_nick);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);

    uint8_t *raw; ssize_t rlen =
        frame_encode(SMSG_ROOM_MSG, FRAME_FLAG_PRIORITY_HIGH,
                     server_next_seq(s), p, plen, &raw);
    free(p);
    if (rlen > 0) {
        /* send to all room members including sender (echo) */
        server_broadcast_room(s, ri, -1, raw, (size_t)rlen, FRAME_FLAG_PRIORITY_HIGH);
        free(raw);
    }
    return 0;
}

/* Handler: CMSG_DM */

static int handle_dm(conn_t *c, const frame_t *f, server_state_t *s) {
    if (!rate_consume(&c->rate))
        return send_error(c, ERR_RATE_LIMITED, "slow down", s);

    char target_nick[MAX_NICK_LEN + 1] = {0};
    char text[MAX_MSG_LEN + 1]         = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_NICK, target_nick, sizeof(target_nick));
    tlv_read_str(f->payload, f->hdr.length, TAG_TEXT, text,        sizeof(text));

    if (!*text)
        return send_error(c, ERR_PROTOCOL, "empty message", s);
    if (strcmp(target_nick, c->nick) == 0)
        return send_error(c, ERR_PROTOCOL, "cannot DM yourself", s);

    conn_t *target = server_find_by_nick(s, target_nick);
    if (!target) return send_error(c, ERR_UNKNOWN, "no such user", s);

    uint64_t dm_ts = now_ms();

    /* Frame sent to the *target*: TAG_NICK=sender, TAG_PEER_NICK=target */
    tlv_builder_t bt; tlv_builder_init(&bt);
    tlv_put_str(&bt, TAG_NICK,      c->nick);
    tlv_put_u32(&bt, TAG_USER_ID,   c->user_id);
    tlv_put_str(&bt, TAG_PEER_NICK, target_nick);
    tlv_put_u32(&bt, TAG_PEER_UID,  target->user_id);
    tlv_put_str(&bt, TAG_TEXT,      text);
    tlv_put_u64(&bt, TAG_TIMESTAMP, dm_ts);
    uint32_t tplen; uint8_t *tp = tlv_builder_take(&bt, &tplen);
    conn_send_frame(target, SMSG_DM, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), tp, tplen, s->cfg);
    free(tp);

    /* Echo back to *sender*: same content -  client uses TAG_PEER_NICK to
     * identify the conversation peer (not the sender uid in TAG_USER_ID). */
    tlv_builder_t bs; tlv_builder_init(&bs);
    tlv_put_str(&bs, TAG_NICK,      c->nick);
    tlv_put_u32(&bs, TAG_USER_ID,   c->user_id);
    tlv_put_str(&bs, TAG_PEER_NICK, target_nick);
    tlv_put_u32(&bs, TAG_PEER_UID,  target->user_id);
    tlv_put_str(&bs, TAG_TEXT,      text);
    tlv_put_u64(&bs, TAG_TIMESTAMP, dm_ts);
    uint32_t splen; uint8_t *sp = tlv_builder_take(&bs, &splen);
    conn_send_frame(c, SMSG_DM, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), sp, splen, s->cfg);
    free(sp);
    db_persist_dm(s->db, c->user_id, c->nick,
                  target->user_id, target_nick, text, dm_ts);

    /* auto-reply if target is away */
    if (target->is_away && target->away_msg[0]) {
        char auto_text[300];
        snprintf(auto_text, sizeof(auto_text), "[Away] %s", target->away_msg);
        uint64_t ar_ts = now_ms();
        tlv_builder_t ar; tlv_builder_init(&ar);
        tlv_put_str(&ar, TAG_NICK,      target_nick);
        tlv_put_u32(&ar, TAG_USER_ID,   target->user_id);
        tlv_put_str(&ar, TAG_PEER_NICK, c->nick);
        tlv_put_u32(&ar, TAG_PEER_UID,  c->user_id);
        tlv_put_str(&ar, TAG_TEXT,      auto_text);
        tlv_put_u64(&ar, TAG_TIMESTAMP, ar_ts);
        uint32_t arplen; uint8_t *arp = tlv_builder_take(&ar, &arplen);
        conn_send_frame(c, SMSG_DM, FRAME_FLAG_PRIORITY_HIGH,
                        server_next_seq(s), arp, arplen, s->cfg);
        free(arp);
    }
    return 0;
}

/* Handler: CMSG_LIST_ROOMS */

static int handle_list_rooms(conn_t *c, const frame_t *f, server_state_t *s) {
    (void)f;
    tlv_builder_t b; tlv_builder_init(&b);
    for (int i = 0; i < s->room_count; i++) {
        if (s->rooms[i] && s->rooms[i]->active) {
            tlv_put_str(&b, TAG_ROOM,      s->rooms[i]->name);
            tlv_put_u32(&b, TAG_OWNER_UID, s->rooms[i]->owner_uid);
        }
    }
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    conn_send_frame(c, SMSG_ROOM_LIST, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), p, plen, s->cfg);
    free(p);
    return 0;
}

/* Handler: CMSG_LIST_USERS */

static int handle_list_users(conn_t *c, const frame_t *f, server_state_t *s) {
    char room_name[MAX_ROOM_LEN + 1] = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_ROOM, room_name, sizeof(room_name));

    tlv_builder_t b; tlv_builder_init(&b);

    if (*room_name) {
        int ri = server_find_room(s, room_name);
        if (ri >= 0) {
            room_t *r = s->rooms[ri];
            for (int i = 0; i < r->member_count; i++) {
                int mfd = r->member_fds[i];
                conn_t *m = (mfd >= 0 && mfd < s->conns_size) ? s->conns[mfd] : NULL;
                if (!m) continue;
                tlv_put_str(&b, TAG_NICK,    m->nick);
                tlv_put_u32(&b, TAG_USER_ID, m->user_id);
            }
        }
    } else {
        /* all online users */
        for (int i = 0; i < s->conns_size; i++) {
            conn_t *m = s->conns[i];
            if (!m || m->state != CONN_AUTHED) continue;
            tlv_put_str(&b, TAG_NICK,    m->nick);
            tlv_put_u32(&b, TAG_USER_ID, m->user_id);
        }
    }

    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    conn_send_frame(c, SMSG_USER_LIST, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), p, plen, s->cfg);
    free(p);
    return 0;
}

/* Handler: CMSG_DELETE_ROOM */

static int handle_delete_room(conn_t *c, const frame_t *f, server_state_t *s) {
    char room_name[MAX_ROOM_LEN + 1] = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_ROOM, room_name, sizeof(room_name));

    int ri = server_find_room(s, room_name);
    if (ri < 0)
        return send_error(c, ERR_NO_SUCH_ROOM, "no such room", s);

    room_t *r = s->rooms[ri];

    /* system rooms (owner_uid == 0) can never be deleted */
    if (r->owner_uid == 0)
        return send_error(c, ERR_PERMISSION_DENIED, "cannot delete a system room", s);

    if (r->owner_uid != c->user_id)
        return send_error(c, ERR_PERMISSION_DENIED, "you do not own this room", s);

    LOG_INFO("fd=%d nick=%s deleting room #%s", c->fd, c->nick, room_name);

    /* server_delete_room broadcasts SMSG_ROOM_DELETED to ALL authed clients,
     * strips membership, persists the deletion, and frees the room. */
    server_delete_room(s, ri);
    return 0;
}

/* Handler: CMSG_PONG */

static int handle_pong(conn_t *c, const frame_t *f, server_state_t *s) {
    (void)f; (void)s;
    c->ping_pending = false;
    c->last_active  = time(NULL);
    LOG_DEBUG("fd=%d PONG received", c->fd);
    return 0;
}

/* Shared rename helper */

/*
 * Rename c to new_nick.  Broadcasts SMSG_NICK_CHANGED to every shared room
 * (excluding the renaming user) with TAG_ROOM + TAG_OLD_NICK + TAG_NICK.
 * Then sends a private confirmation to c with confirm_type + TAG_NICK + TAG_USER_ID.
 */
static int server_do_rename(conn_t *c, const char *new_nick,
                             server_msg_t confirm_type, server_state_t *s) {
    char old_nick[MAX_NICK_LEN + 1];
    snprintf(old_nick, sizeof(old_nick), "%s", c->nick);
    snprintf(c->nick,  sizeof(c->nick),  "%s", new_nick);
    /* Update base_nick on account transitions (register / login / logout) */
    if (confirm_type == SMSG_REGISTER_OK ||
        confirm_type == SMSG_LOGIN_OK    ||
        confirm_type == SMSG_LOGOUT_OK) {
        snprintf(c->base_nick, sizeof(c->base_nick), "%s", new_nick);
    }

    /* broadcast SMSG_NICK_CHANGED to each shared room (excl. sender) */
    for (int i = 0; i < c->room_count; i++) {
        int ri = c->rooms[i];
        if (ri < 0 || ri >= s->room_count || !s->rooms[ri]) continue;
        tlv_builder_t rb; tlv_builder_init(&rb);
        tlv_put_str(&rb, TAG_ROOM,     s->rooms[ri]->name);
        tlv_put_str(&rb, TAG_OLD_NICK, old_nick);
        tlv_put_str(&rb, TAG_NICK,     new_nick);
        tlv_put_u32(&rb, TAG_USER_ID,  c->user_id);
        /* include base_nick so peers can display "newNick (base)" */
        if (strcmp(new_nick, c->base_nick) != 0)
            tlv_put_str(&rb, TAG_BASE_NICK, c->base_nick);
        uint32_t rplen; uint8_t *rp = tlv_builder_take(&rb, &rplen);
        uint8_t *rraw; ssize_t rrlen =
            frame_encode(SMSG_NICK_CHANGED, FRAME_FLAG_PRIORITY_HIGH,
                         server_next_seq(s), rp, rplen, &rraw);
        free(rp);
        if (rrlen > 0) {
            server_broadcast_room(s, ri, c->fd, rraw, (size_t)rrlen,
                                  FRAME_FLAG_PRIORITY_HIGH);
            free(rraw);
        }
    }

    /* private confirmation to the requester */
    tlv_builder_t bc; tlv_builder_init(&bc);
    tlv_put_str(&bc, TAG_NICK,    new_nick);
    tlv_put_u32(&bc, TAG_USER_ID, c->user_id);
    uint32_t cplen; uint8_t *cp = tlv_builder_take(&bc, &cplen);
    conn_send_frame(c, confirm_type, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), cp, cplen, s->cfg);
    free(cp);

    LOG_INFO("fd=%d uid=%u rename: %s -> %s", c->fd, c->user_id, old_nick, new_nick);
    return 0;
}

/* Handler: CMSG_NICK */

static int handle_nick(conn_t *c, const frame_t *f, server_state_t *s) {
    char new_nick[MAX_NICK_LEN + 1] = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_NICK, new_nick, sizeof(new_nick));

    if (!nick_valid(new_nick))
        return send_error(c, ERR_NICK_INVALID, "invalid nick (a-z A-Z 0-9 _ -)", s);

    /* no-op if the requested nick is identical to the current one */
    if (strcmp(new_nick, c->nick) == 0) return 0;

    conn_t *existing = server_find_by_nick(s, new_nick);
    if (existing && existing != c)
        return send_error(c, ERR_NICK_TAKEN, "nick already in use", s);

    return server_do_rename(c, new_nick, SMSG_NICK_CHANGED, s);
}

/* Handler: CMSG_REGISTER */

static int handle_register(conn_t *c, const frame_t *f, server_state_t *s) {
    char nick[MAX_NICK_LEN + 1] = {0};
    char pass[256]               = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_NICK,     nick, sizeof(nick));
    tlv_read_str(f->payload, f->hdr.length, TAG_PASSWORD, pass, sizeof(pass));

    if (c->registered)
        return send_error(c, ERR_PROTOCOL, "already logged in -  /logout first", s);
    if (!nick_valid(nick))
        return send_error(c, ERR_NICK_INVALID, "invalid nick (a-z A-Z 0-9 _ -)", s);
    if (!pass[0])
        return send_error(c, ERR_PROTOCOL, "password required", s);

    /* check nick not taken by another live connection */
    conn_t *existing = server_find_by_nick(s, nick);
    if (existing && existing != c)
        return send_error(c, ERR_NICK_TAKEN, "nick already in use by a connected user", s);

    if (!db_register_user(s->db, nick, pass))
        return send_error(c, ERR_USER_EXISTS, "account already exists", s);

    c->registered = true;
    return server_do_rename(c, nick, SMSG_REGISTER_OK, s);
}

/* Handler: CMSG_LOGIN */

static int handle_login(conn_t *c, const frame_t *f, server_state_t *s) {
    char nick[MAX_NICK_LEN + 1] = {0};
    char pass[256]               = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_NICK,     nick, sizeof(nick));
    tlv_read_str(f->payload, f->hdr.length, TAG_PASSWORD, pass, sizeof(pass));

    if (c->registered)
        return send_error(c, ERR_PROTOCOL, "already logged in -  /logout first", s);
    if (!nick_valid(nick))
        return send_error(c, ERR_NICK_INVALID, "invalid nick", s);
    if (!pass[0])
        return send_error(c, ERR_PROTOCOL, "password required", s);

    if (!db_authenticate_user(s->db, nick, pass))
        return send_error(c, ERR_AUTH_FAILED, "incorrect username or password", s);

    /* check nick not taken by another live connection */
    conn_t *existing = server_find_by_nick(s, nick);
    if (existing && existing != c)
        return send_error(c, ERR_NICK_TAKEN, "account already logged in elsewhere", s);

    c->registered = true;
    return server_do_rename(c, nick, SMSG_LOGIN_OK, s);
}

/* Handler: CMSG_LOGOUT */

static int handle_logout(conn_t *c, const frame_t *f, server_state_t *s) {
    (void)f;
    if (!c->registered)
        return send_error(c, ERR_NOT_LOGGED_IN, "not logged in", s);

    char guest_nick[MAX_NICK_LEN + 1];
    snprintf(guest_nick, sizeof(guest_nick), "guest_%u", c->user_id);

    c->registered = false;
    return server_do_rename(c, guest_nick, SMSG_LOGOUT_OK, s);
}

/* Handler: CMSG_SET_TOPIC */

static int handle_set_topic(conn_t *c, const frame_t *f, server_state_t *s) {
    if (!rate_consume(&c->rate))
        return send_error(c, ERR_RATE_LIMITED, "slow down", s);

    char room_name[MAX_ROOM_LEN + 1] = {0};
    char topic[256]                  = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_ROOM,  room_name, sizeof(room_name));
    tlv_read_str(f->payload, f->hdr.length, TAG_TOPIC, topic,     sizeof(topic));

    int ri = server_find_room(s, room_name);
    if (ri < 0 || !conn_is_room_member(c, ri))
        return send_error(c, ERR_NOT_IN_ROOM, "not in room", s);

    snprintf(s->rooms[ri]->topic,        sizeof(s->rooms[ri]->topic),        "%s", topic);
    snprintf(s->rooms[ri]->topic_set_by, sizeof(s->rooms[ri]->topic_set_by), "%s", c->nick);

    db_persist_room(s->db, room_name, topic, s->rooms[ri]->owner_uid);

    /* broadcast the new topic to every room member */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM,  room_name);
    tlv_put_str(&b, TAG_TOPIC, topic);
    tlv_put_str(&b, TAG_NICK,  c->nick);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    uint8_t *raw; ssize_t rlen =
        frame_encode(SMSG_ROOM_TOPIC, FRAME_FLAG_PRIORITY_HIGH,
                     server_next_seq(s), p, plen, &raw);
    free(p);
    if (rlen > 0) {
        server_broadcast_room(s, ri, -1, raw, (size_t)rlen, FRAME_FLAG_PRIORITY_HIGH);
        free(raw);
    }
    LOG_INFO("fd=%d nick=%s set topic in #%s: %.40s", c->fd, c->nick, room_name, topic);
    return 0;
}
/* Handler: CMSG_TYPING */

static int handle_typing(conn_t *c, const frame_t *f, server_state_t *s) {
    char room_name[MAX_ROOM_LEN + 1] = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_ROOM, room_name, sizeof(room_name));
    int ri = server_find_room(s, room_name);
    if (ri < 0) return 0;  /* room not found -  silently ignore */
    /* sender must be a member of the room */
    if (!conn_is_room_member(c, ri)) return 0;

    /* Build SMSG_TYPING_NOTIFY: room + nick of the typer.
     * Use low priority so it is dropped under backpressure rather than
     * displacing actual chat messages. */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room_name);
    tlv_put_str(&b, TAG_NICK, c->nick);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    uint8_t *raw; ssize_t rlen =
        frame_encode(SMSG_TYPING_NOTIFY, FRAME_FLAG_PRIORITY_LOW,
                     server_next_seq(s), p, plen, &raw);
    free(p);
    if (rlen > 0) {
        /* broadcast to all room members except the sender */
        server_broadcast_room(s, ri, c->fd, raw, (size_t)rlen,
                              FRAME_FLAG_PRIORITY_LOW);
        free(raw);
    }
    return 0;
}
/* Handler: CMSG_AWAY */

static int handle_away(conn_t *c, const frame_t *f, server_state_t *s) {
    if (!rate_consume(&c->rate))
        return send_error(c, ERR_RATE_LIMITED, "slow down", s);

    char away_msg[256] = {0};
    tlv_read_str(f->payload, f->hdr.length, TAG_AWAY_MSG, away_msg, sizeof(away_msg));

    c->is_away = (away_msg[0] != '\0');
    snprintf(c->away_msg, sizeof(c->away_msg), "%s", away_msg);

    /* broadcast SMSG_USER_AWAY to all rooms the user is in */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_NICK,     c->nick);
    tlv_put_u32(&b, TAG_USER_ID,  c->user_id);
    if (c->is_away)
        tlv_put_str(&b, TAG_AWAY_MSG, c->away_msg);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    uint8_t *raw; ssize_t rlen =
        frame_encode(SMSG_USER_AWAY, FRAME_FLAG_PRIORITY_HIGH,
                     server_next_seq(s), p, plen, &raw);
    free(p);
    if (rlen > 0) {
        /* send to every room the user is in, including to themselves */
        for (int i = 0; i < c->room_count; i++)
            server_broadcast_room(s, c->rooms[i], -1, raw, (size_t)rlen,
                                  FRAME_FLAG_PRIORITY_HIGH);
        free(raw);
    }
    LOG_INFO("fd=%d nick=%s %s: %.80s", c->fd, c->nick,
             c->is_away ? "now away" : "back", c->away_msg);
    return 0;
}

static int handle_ping(conn_t *c, const frame_t *f, server_state_t *s) {
    (void)f;
    /* echo back a PONG */
    conn_send_frame(c, SMSG_PONG, FRAME_FLAG_PRIORITY_HIGH,
                    server_next_seq(s), NULL, 0, s->cfg);
    return 0;
}

/* Main dispatcher */

int dispatch_frame(conn_t *c, const frame_t *f, server_state_t *s) {
    /* update liveness regardless of message type */
    c->last_active = time(NULL);

    /* guard: unauthenticated clients may only send HELLO; CMSG_NICK requires auth */
    if (c->state == CONN_HANDSHAKE &&
        f->hdr.type != CMSG_HELLO) {
        send_error(c, ERR_AUTH_REQUIRED, "must HELLO first", s);
        return -1;
    }

    switch ((client_msg_t)f->hdr.type) {
        case CMSG_HELLO:       return handle_hello(c, f, s);
        case CMSG_NICK:        return handle_nick(c, f, s);
        case CMSG_REGISTER:    return handle_register(c, f, s);
        case CMSG_LOGIN:       return handle_login(c, f, s);
        case CMSG_LOGOUT:      return handle_logout(c, f, s);
        case CMSG_AWAY:        return handle_away(c, f, s);
        case CMSG_TYPING:      return handle_typing(c, f, s);
        case CMSG_JOIN:        return handle_join(c, f, s);
        case CMSG_LEAVE:       return handle_leave(c, f, s);
        case CMSG_ROOM_MSG:    return handle_room_msg(c, f, s);
        case CMSG_DM:          return handle_dm(c, f, s);
        case CMSG_LIST_ROOMS:  return handle_list_rooms(c, f, s);
        case CMSG_LIST_USERS:  return handle_list_users(c, f, s);
        case CMSG_DELETE_ROOM: return handle_delete_room(c, f, s);
        case CMSG_SET_TOPIC:   return handle_set_topic(c, f, s);
        case CMSG_PING:        return handle_ping(c, f, s);
        case CMSG_PONG:        return handle_pong(c, f, s);
        /* file transfer: not yet implemented */
        case CMSG_FILE_OFFER:
        case CMSG_FILE_ACCEPT:
        case CMSG_FILE_DECLINE:
        case CMSG_FILE_CHUNK:
        case CMSG_FILE_END:
        case CMSG_FILE_ACK:
            return send_error(c, ERR_UNKNOWN, "file transfer not yet supported", s);
        default:
            LOG_WARN("fd=%d unknown msg type 0x%04x", c->fd, f->hdr.type);
            return send_error(c, ERR_BAD_FRAME, "unknown message type", s);
    }
}
