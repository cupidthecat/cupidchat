#include "server/buddies.h"
#include "proto/tlv.h"
#include <stdlib.h>
#include <string.h>

static int buddy_error(server_state_t *s, conn_t *c, error_code_t code, const char *message) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u16(&b, TAG_ERROR_CODE, code);
    tlv_put_str(&b, TAG_ERROR_MSG, message);
    int rc = conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                             (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

static int put_buddy(tlv_builder_t *b, server_state_t *s, uint64_t account, const char *name,
                     bool offline) {
    conn_t *peer = offline ? NULL : server_find_by_account(s, account);
    if (tlv_put_str(b, TAG_BASE_NICK, name) < 0 ||
        tlv_put_str(b, TAG_NICK, peer ? peer->nick : "") < 0 ||
        tlv_put_u32(b, TAG_USER_ID, peer ? peer->user_id : 0) < 0 ||
        tlv_put_u8(b, TAG_PRESENCE,
                   !peer           ? 0
                   : peer->is_away ? 2
                                   : 1) < 0 ||
        tlv_put_str(b, TAG_AWAY_MSG, peer && peer->is_away ? peer->away_msg : "") < 0)
        return -1;
    return 0;
}

typedef struct {
    server_state_t *s;
    tlv_builder_t payload;
    uint64_t ids[MAX_BUDDIES];
    int count;
    bool failed;
} list_ctx_t;

static void list_row(void *ud, uint64_t id, const char *name) {
    list_ctx_t *ctx = ud;
    if (ctx->count >= MAX_BUDDIES) {
        ctx->failed = true;
        return;
    }
    ctx->ids[ctx->count++] = id;
    if (put_buddy(&ctx->payload, ctx->s, id, name, false) < 0)
        ctx->failed = true;
}

int server_send_buddies(server_state_t *s, conn_t *c) {
    if (!c->account_id)
        return buddy_error(s, c, ERR_NOT_LOGGED_IN, "Log in to use a persistent buddy list");
    list_ctx_t ctx = {.s = s};
    tlv_builder_init(&ctx.payload);
    int rc = db_list_buddies(s->db, c->account_id, list_row, &ctx);
    if (rc < 0 || ctx.failed) {
        tlv_builder_free(&ctx.payload);
        return buddy_error(s, c, ERR_UNKNOWN, "Could not load buddy list");
    }
    memcpy(c->buddy_ids, ctx.ids, (size_t)ctx.count * sizeof(ctx.ids[0]));
    c->buddy_count = ctx.count;
    rc = conn_send_frame(c, SMSG_BUDDY_LIST, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s),
                         ctx.payload.buf, (uint32_t)ctx.payload.len, s->cfg);
    tlv_builder_free(&ctx.payload);
    return rc;
}

void server_buddy_presence(server_state_t *s, uint64_t account, bool offline) {
    if (!account)
        return;
    char name[MAX_NICK_LEN + 1];
    if (db_account_name(s->db, account, name, sizeof(name)) < 0)
        return;
    tlv_builder_t b;
    tlv_builder_init(&b);
    if (put_buddy(&b, s, account, name, offline) < 0) {
        tlv_builder_free(&b);
        return;
    }
    for (conn_t *c = s->active_head; c; c = c->active_next) {
        if (!c || c->state != CONN_AUTHED || !c->account_id)
            continue;
        for (int i = 0; i < c->buddy_count; i++) {
            if (c->buddy_ids[i] == account) {
                conn_send_frame(c, SMSG_BUDDY_PRESENCE, FRAME_FLAG_PRIORITY_HIGH,
                                server_next_seq(s), b.buf, (uint32_t)b.len, s->cfg);
                break;
            }
        }
    }
    tlv_builder_free(&b);
}

int server_buddy_command(server_state_t *s, conn_t *c, const frame_t *f) {
    if (!c->account_id)
        return buddy_error(s, c, ERR_NOT_LOGGED_IN, "Log in to use a persistent buddy list");
    if (!rate_consume(&c->rate))
        return buddy_error(s, c, ERR_RATE_LIMITED, "slow down");
    if (f->hdr.type == CMSG_BUDDY_LIST)
        return server_send_buddies(s, c);
    char name[MAX_NICK_LEN + 1] = {0};
    uint16_t length = 0;
    const uint8_t *raw = tlv_find(f->payload, f->hdr.length, TAG_BASE_NICK, &length);
    if (!raw || !length || length > MAX_NICK_LEN || memchr(raw, 0, length))
        return buddy_error(s, c, ERR_PROTOCOL, "An account name is required");
    memcpy(name, raw, length);
    int rc = db_update_buddy(s->db, c->account_id, name, f->hdr.type == CMSG_BUDDY_REMOVE);
    if (rc < 0) {
        const char *message = rc == -2   ? "No such account"
                              : rc == -3 ? "Buddy list is full (64 accounts)"
                              : rc == -4 ? "You cannot add yourself"
                                         : "Could not update buddy list";
        return buddy_error(s, c, ERR_UNKNOWN, message);
    }
    return server_send_buddies(s, c);
}
