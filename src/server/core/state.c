/*
 * src/server/core/state.c -  global server state management
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

#include "server/state.h"
#include "server/db.h"
#include "server/log.h"
#include "proto/frame.h"
#include "proto/tlv.h"
#include "proto/types.h"

/* forward declaration -  defined later in this file */
int server_get_or_create_room(server_state_t *s, const char *name, uint32_t owner_uid);

/* DB startup hydration helpers */

/* context struct shared between the two callbacks below */
typedef struct { server_state_t *s; int ri; } hist_ctx_t;

/* fired once per message row; pushes into the in-memory ring buffer */
static void hydrate_msg_cb(void *ud, uint32_t uid,
                           const char *nick, const char *text, uint64_t ts) {
    hist_ctx_t *ctx = (hist_ctx_t *)ud;
    room_history_push(ctx->s->rooms[ctx->ri], uid, nick, text, ts);
}

/* fired once per room row; creates the room and loads its history */
static void hydrate_room_cb(void *ud, const char *name, const char *topic,
                            uint32_t owner_uid) {
    server_state_t *s = (server_state_t *)ud;

    int ri = server_get_or_create_room(s, name, owner_uid);
    if (ri < 0) return;

    /* restore the topic that was persisted in the database */
    if (topic && *topic)
        snprintf(s->rooms[ri]->topic, sizeof(s->rooms[ri]->topic), "%s", topic);

    hist_ctx_t ctx = { s, ri };
    db_load_room_history(s->db, name, s->cfg->history_size,
                         hydrate_msg_cb, &ctx);
}

/* Lifecycle */

int server_state_init(server_state_t *s, const server_config_t *cfg) {
    memset(s, 0, sizeof(*s));
    s->cfg            = cfg;
    s->next_uid       = 1;
    s->next_seq       = 1;
    s->listen_fd_tls  = -1;

    /* Size the fd-indexed conns[] array by the process's hard fd limit so
     * that fd values from the OS never exceed the array bounds.  We still
     * enforce cfg->max_clients as the maximum concurrent connection count. */
    struct rlimit rl = {0};
    getrlimit(RLIMIT_NOFILE, &rl);
    int conns_size = (rl.rlim_cur != RLIM_INFINITY)
                   ? (int)rl.rlim_cur : 65536;
    /* Ensure conns_size is at least as large as max_clients so the
     * configured limit is always reachable. */
    if (conns_size < cfg->max_clients) conns_size = cfg->max_clients;
    s->conns_size = conns_size;

    s->conns = calloc((size_t)conns_size, sizeof(conn_t *));
    if (!s->conns) return -1;

    s->rooms = calloc(MAX_ROOMS, sizeof(room_t *));
    if (!s->rooms) { free(s->conns); return -1; }

    /* Database */
    s->db = db_open(cfg->db_path);
    if (!s->db) {
        LOG_ERROR("%s", "failed to open database");
        free(s->rooms); free(s->conns);
        return -1;
    }
    db_init_schema(s->db);
    db_seed_defaults(s->db);

    /* hydrate in-memory rooms (and their history) from the last run */
    db_load_rooms(s->db, hydrate_room_cb, s);

    return 0;
}

void server_state_free(server_state_t *s) {
    for (int i = 0; i < s->conns_size; i++) {
        if (s->conns[i]) { conn_destroy(s->conns[i]); s->conns[i] = NULL; }
    }
    for (int i = 0; i < s->room_count; i++) {
        if (s->rooms[i]) { room_destroy(s->rooms[i]); s->rooms[i] = NULL; }
    }
    db_close(s->db);
    s->db = NULL;
    free(s->conns);
    free(s->rooms);
}

void server_register_conn(server_state_t *s, conn_t *c) {
    if (c->fd >= 0 && c->fd < s->conns_size) {
        s->conns[c->fd] = c;
        s->conn_count++;
    }
}

void server_unregister_conn(server_state_t *s, conn_t *c) {
    if (c->fd >= 0 && c->fd < s->conns_size &&
        s->conns[c->fd] == c) {
        s->conns[c->fd] = NULL;
        s->conn_count--;
    }
    /* remove from all rooms */
    for (int ri = 0; ri < s->room_count; ri++) {
        if (s->rooms[ri] && s->rooms[ri]->active)
            room_remove_member(s->rooms[ri], c->fd);
    }
}

conn_t *server_find_by_nick(server_state_t *s, const char *nick) {
    for (int i = 0; i < s->conns_size; i++) {
        conn_t *c = s->conns[i];
        if (c && c->state == CONN_AUTHED &&
            strcmp(c->nick, nick) == 0) return c;
    }
    return NULL;
}

conn_t *server_find_by_uid(server_state_t *s, uint32_t uid) {
    for (int i = 0; i < s->conns_size; i++) {
        conn_t *c = s->conns[i];
        if (c && c->user_id == uid) return c;
    }
    return NULL;
}

int server_find_room(server_state_t *s, const char *name) {
    for (int i = 0; i < s->room_count; i++) {
        if (s->rooms[i] && s->rooms[i]->active &&
            strcmp(s->rooms[i]->name, name) == 0) return i;
    }
    return -1;
}

int server_get_or_create_room(server_state_t *s, const char *name, uint32_t owner_uid) {
    int idx = server_find_room(s, name);
    if (idx >= 0) return idx;

    /* Reuse the first NULL slot left by a previously-deleted room so we
     * don't exhaust MAX_ROOMS through create/delete cycles. */
    int free_slot = -1;
    for (int i = 0; i < s->room_count; i++) {
        if (!s->rooms[i]) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        if (s->room_count >= MAX_ROOMS) return -1;
        free_slot = s->room_count++;
    }

    room_t *r = room_create(name, s->cfg->history_size);
    if (!r) return -1;
    r->owner_uid      = owner_uid;
    s->rooms[free_slot] = r;
    LOG_INFO("created room #%s idx=%d owner=%u", name, free_slot, owner_uid);
    db_persist_room(s->db, name, "", owner_uid);
    return free_slot;
}

void server_broadcast_room(server_state_t *s, int room_idx,
                           int skip_fd,
                           uint8_t *buf, size_t len, uint16_t flags) {
    room_t *r = s->rooms[room_idx];
    if (!r) return;

    for (int i = 0; i < r->member_count; i++) {
        int mfd = r->member_fds[i];
        if (mfd == skip_fd) continue;
        conn_t *c = (mfd >= 0 && mfd < s->conns_size) ? s->conns[mfd] : NULL;
        if (!c || c->state != CONN_AUTHED) continue;

        /* duplicate the buffer for each recipient */
        uint8_t *copy = malloc(len);
        if (!copy) {
            LOG_WARN("server_broadcast_room: malloc failed for fd=%d", mfd);
            continue;
        }
        memcpy(copy, buf, len);
        conn_enqueue(c, copy, len, flags, s->cfg);
    }
}

uint32_t server_next_uid(server_state_t *s) { return s->next_uid++; }
uint32_t server_next_seq(server_state_t *s) { return s->next_seq++; }

/* Room deletion */

void server_delete_room(server_state_t *s, int ri) {
    room_t *r = (ri >= 0 && ri < s->room_count) ? s->rooms[ri] : NULL;
    if (!r || !r->active) return;

    char name[65];
    snprintf(name, sizeof(name), "%s", r->name);
    LOG_INFO("deleting room #%s idx=%d", name, ri);

    /* build SMSG_ROOM_DELETED frame once */
    tlv_builder_t b; tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, name);
    uint32_t plen; uint8_t *p = tlv_builder_take(&b, &plen);
    uint8_t *raw = NULL; ssize_t rlen = -1;
    rlen = frame_encode(SMSG_ROOM_DELETED, FRAME_FLAG_PRIORITY_HIGH,
                        server_next_seq(s), p, plen, &raw);
    free(p);

    /* Strip membership from every client and broadcast SMSG_ROOM_DELETED
     * to all authenticated connections in one pass. */
    if (rlen > 0) {
        for (int i = 0; i < s->conns_size; i++) {
            conn_t *c = s->conns[i];
            if (!c || c->state != CONN_AUTHED) continue;
            conn_remove_room(c, ri);  /* no-op if not a member */
            uint8_t *copy = malloc((size_t)rlen);
            if (copy) {
                memcpy(copy, raw, (size_t)rlen);
                conn_enqueue(c, copy, (size_t)rlen,
                             FRAME_FLAG_PRIORITY_HIGH, s->cfg);
            } else {
                LOG_WARN("server_delete_room: malloc failed for fd=%d", c->fd);
            }
        }
    } else {
        /* frame_encode failed; still strip membership */
        for (int i = 0; i < s->conns_size; i++) {
            conn_t *c = s->conns[i];
            if (c && c->state == CONN_AUTHED) conn_remove_room(c, ri);
        }
    }
    if (rlen > 0 && raw) free(raw);

    db_delete_room(s->db, name);
    room_destroy(r);
    s->rooms[ri] = NULL;
    /* slot stays NULL; room_count is not decremented so existing indices
     * held by live connections remain valid */
}
