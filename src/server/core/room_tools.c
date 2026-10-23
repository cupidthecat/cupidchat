#include "server/room_tools.h"
#include "server/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int error(server_state_t *s, conn_t *c, uint32_t request, error_code_t code,
                 const char *text) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u32(&b, TAG_REQUEST_ID, request);
    tlv_put_str(&b, TAG_SCOPE, "room");
    tlv_put_u16(&b, TAG_ERROR_CODE, code);
    tlv_put_str(&b, TAG_ERROR_MSG, text);
    int rc = conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                             (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

int server_room_role(server_state_t *s, const room_t *r, uint64_t account, uint8_t *role) {
    account_controls_t controls;
    room_access_t access;
    *role = 0;
    if (!account || db_get_controls(s->db, account, &controls) < 0 ||
        db_room_access(s->db, r->name, account, &access) < 0)
        return -1;
    *role = access.flags;
    if (account == r->owner_account_id)
        *role |= ROOM_OWNER;
    if (controls.admin)
        *role |= ROOM_ADMIN;
    return 0;
}

int server_room_info(server_state_t *s, conn_t *c, int ri, uint32_t request, const char *message,
                     bool details) {
    room_t *r = s->rooms[ri];
    room_info_t info = {0};
    if (server_room_role(s, r, c->account_id, &info.role) < 0)
        return error(s, c, request, ERR_UNKNOWN, "Could not load room permissions");
    if (r->owner_account_id &&
        db_account_name(s->db, r->owner_account_id, info.owner, sizeof(info.owner)) < 0)
        return error(s, c, request, ERR_UNKNOWN, "Could not load room owner");
    if (details && db_room_access_list(s->db, r->name, (info.role & ROOM_MANAGERS) != 0, &info) < 0)
        return error(s, c, request, ERR_UNKNOWN, "Could not load room access list");
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u32(&b, TAG_REQUEST_ID, request);
    tlv_put_str(&b, TAG_ROOM, r->name);
    tlv_put_str(&b, TAG_ROOM_OWNER, info.owner);
    tlv_put_u32(&b, TAG_OWNER_UID, server_room_owner_uid(s, r));
    tlv_put_str(&b, TAG_TOPIC, r->topic);
    tlv_put_u8(&b, TAG_TOPIC_LOCK, r->topic_locked);
    tlv_put_u8(&b, TAG_ROOM_ROLE, info.role);
    tlv_put_u8(&b, TAG_MORE, info.more);
    tlv_put_str(&b, TAG_TEXT, message ? message : "");
    bool failed = false;
    for (int i = 0; i < info.count; i++) {
        room_access_t *a = &info.entries[i];
        tlv_builder_t e;
        tlv_builder_init(&e);
        if (tlv_put_str(&e, TAG_BASE_NICK, a->account) < 0 ||
            tlv_put_u8(&e, TAG_ROOM_ROLE, a->flags) < 0 ||
            tlv_put_str(&e, TAG_NICK, a->inviter) < 0 || tlv_put_str(&e, TAG_TEXT, a->reason) < 0 ||
            tlv_put_u64(&e, TAG_TIMESTAMP, a->invited_at) < 0 ||
            tlv_put_bytes(&b, TAG_ENTRY, e.buf, (uint16_t)e.len) < 0)
            failed = true;
        tlv_builder_free(&e);
    }
    int rc = failed ? -1
                    : conn_send_frame(c, SMSG_ROOM_INFO, FRAME_FLAG_PRIORITY_HIGH,
                                      server_next_seq(s), b.buf, (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

static void update_members(server_state_t *s, int ri, conn_t *actor) {
    room_t *r = s->rooms[ri];
    for (int i = 0; i < r->member_count; i++) {
        conn_t *c = s->conns[r->member_fds[i]];
        if (c && c != actor && c->state == CONN_AUTHED)
            server_room_info(s, c, ri, 0, "", false);
    }
}

static void invitation(server_state_t *s, conn_t *c, const char *room, const char *inviter,
                       bool fresh) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, room);
    tlv_put_str(&b, TAG_BASE_NICK, inviter);
    tlv_put_str(&b, TAG_SCOPE, fresh ? "new" : "saved");
    conn_send_frame(c, SMSG_ROOM_INVITE, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                    (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
}

typedef struct {
    server_state_t *s;
    conn_t *c;
} invite_ctx_t;

static void pending_invite(void *ctx, const char *room, const char *inviter) {
    invite_ctx_t *i = ctx;
    invitation(i->s, i->c, room, inviter, false);
}

void server_room_invites(server_state_t *s, conn_t *c) {
    invite_ctx_t ctx = {s, c};
    db_room_invites(s->db, c->account_id, pending_invite, &ctx);
}

static void kick(server_state_t *s, conn_t *actor, conn_t *target, int ri, const char *reason,
                 bool banned) {
    room_t *r = s->rooms[ri];
    conn_remove_room(target, ri);
    room_remove_member(r, target->fd);
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, r->name);
    tlv_put_str(&b, TAG_NICK, actor->base_nick);
    tlv_put_str(&b, TAG_TEXT, reason);
    tlv_put_u8(&b, TAG_BANNED, banned);
    conn_send_frame(target, SMSG_ROOM_KICKED, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                    (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_ROOM, r->name);
    tlv_put_str(&b, TAG_NICK, target->nick);
    tlv_put_u32(&b, TAG_USER_ID, target->user_id);
    uint8_t *raw = NULL;
    ssize_t length = frame_encode(SMSG_USER_LEFT, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s),
                                  b.buf, (uint32_t)b.len, &raw);
    if (length > 0) {
        server_broadcast_room(s, ri, -1, raw, (size_t)length, FRAME_FLAG_PRIORITY_HIGH);
        free(raw);
    }
    tlv_builder_free(&b);
}

int server_room_command(server_state_t *s, conn_t *c, const frame_t *f) {
    uint32_t request = 0;
    tlv_read_u32(f->payload, f->hdr.length, TAG_REQUEST_ID, &request);
    if (!rate_consume(&c->rate))
        return error(s, c, request, ERR_RATE_LIMITED, "Slow down and try again");
    char room[65], action[16], account[33] = {0}, reason[257] = {0};
    if (!request || !room_read_name(f->payload, f->hdr.length, room) ||
        !room_read_text(f->payload, f->hdr.length, TAG_ADMIN_ACTION, action, sizeof(action)))
        return error(s, c, request, ERR_PROTOCOL, "Invalid room command");
    int ri = server_find_room(s, room);
    if (ri < 0)
        return error(s, c, request, ERR_NO_SUCH_ROOM, "No such room");
    uint8_t role = 0;
    room_t *r = s->rooms[ri];
    if (server_room_role(s, r, c->account_id, &role) < 0)
        return error(s, c, request, ERR_UNKNOWN, "Could not check room permissions");
    if (!strcmp(action, "info"))
        return server_room_info(s, c, ri, request, "", true);
    if (!strcmp(action, "decline")) {
        if (db_room_clear_invite(s->db, room, c->account_id) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not decline invitation");
        return server_room_info(s, c, ri, request, "Invitation declined", true);
    }
    if (!(role & ROOM_MANAGERS))
        return error(s, c, request, ERR_PERMISSION_DENIED,
                     "A room operator is required for this action");
    if (!strcmp(action, "topiclock")) {
        if (!(role & (ROOM_OWNER | ROOM_ADMIN)))
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "Only the owner or a server admin can change topic permissions");
        uint8_t enabled = 0;
        if (!tlv_read_u8(f->payload, f->hdr.length, TAG_ENABLED, &enabled) || enabled > 1)
            return error(s, c, request, ERR_PROTOCOL, "Use topic lock on or off");
        if (db_room_topic_lock(s->db, room, enabled != 0) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not save topic permissions");
        r->topic_locked = enabled != 0;
        LOG_INFO("room=%s actor=%s topic_lock=%u", room, c->base_nick, enabled);
        update_members(s, ri, c);
        return server_room_info(s, c, ri, request,
                                enabled ? "Only room operators can change the topic"
                                        : "All room members can change the topic",
                                true);
    }
    if (!room_read_text(f->payload, f->hdr.length, TAG_BASE_NICK, account, sizeof(account)) ||
        !account[0] || !room_read_text(f->payload, f->hdr.length, TAG_TEXT, reason, sizeof(reason)))
        return error(s, c, request, ERR_PROTOCOL,
                     "Provide an account name and a reason of at most 256 UTF-8 bytes");
    uint64_t id = db_find_account(s->db, account);
    if (!id || id == UINT64_MAX || db_account_name(s->db, id, account, sizeof(account)) < 0)
        return error(s, c, request, ERR_UNKNOWN, "No such account");
    uint8_t target_role = 0;
    if (server_room_role(s, r, id, &target_role) < 0)
        return error(s, c, request, ERR_UNKNOWN, "Could not check target permissions");
    conn_t *peer = server_find_by_account(s, id);
    const char *result = "Room access updated";
    if (!strcmp(action, "invite")) {
        account_controls_t controls;
        if (db_get_controls(s->db, c->account_id, &controls) < 0 || controls.muted)
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "Muted accounts cannot send invitations");
        if (id == c->account_id)
            return error(s, c, request, ERR_PROTOCOL, "Choose another account to invite");
        if (peer && conn_is_room_member(peer, ri))
            return server_room_info(s, c, ri, request, "That account is already in this room",
                                    true);
        int rc = db_room_invite(s->db, room, id, c->account_id);
        if (rc < 0)
            return error(s, c, request, ERR_UNKNOWN,
                         rc == -2   ? "Unban the account before inviting it"
                         : rc == -3 ? "Wait 30 seconds before repeating an invitation"
                         : rc == -4 ? "That account already has 64 pending invitations"
                                    : "Could not save invitation");
        if (peer)
            invitation(s, peer, room, c->base_nick, true);
        result = peer ? "Invitation sent" : "Invitation saved for the account's next login";
    } else if (!strcmp(action, "op") || !strcmp(action, "deop")) {
        if (!(role & (ROOM_OWNER | ROOM_ADMIN)))
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "Only the owner or a server admin can grant or remove operators");
        if (target_role & (ROOM_OWNER | ROOM_ADMIN))
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "Room owners and server admins always have operator rights");
        int rc = db_room_access_update(s->db, room, id, action, reason);
        if (rc < 0)
            return error(s, c, request, ERR_UNKNOWN,
                         rc == -2 ? "Unban the account before granting operator rights"
                                  : "Could not save operator rights");
        result = !strcmp(action, "op") ? "Room operator granted" : "Room operator removed";
    } else if (!strcmp(action, "kick") || !strcmp(action, "ban")) {
        if (id == c->account_id || (target_role & (ROOM_OWNER | ROOM_ADMIN)) ||
            ((target_role & ROOM_OPERATOR) && !(role & (ROOM_OWNER | ROOM_ADMIN))))
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "You cannot remove this account from the room");
        bool banned = !strcmp(action, "ban");
        if (!banned && (!peer || !conn_is_room_member(peer, ri)))
            return error(s, c, request, ERR_NOT_IN_ROOM, "That account is not in this room");
        if (banned && db_room_access_update(s->db, room, id, "ban", reason) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not save room ban");
        if (peer && conn_is_room_member(peer, ri))
            kick(s, c, peer, ri, reason, banned);
        result = banned ? "Account banned from this room" : "Account removed from this room";
    } else if (!strcmp(action, "unban")) {
        if (db_room_access_update(s->db, room, id, "unban", "") < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not remove room ban");
        result = "Room ban removed";
    } else
        return error(s, c, request, ERR_PROTOCOL, "Unknown room action");
    LOG_INFO("room=%s actor=%s action=%s account=%s", room, c->base_nick, action, account);
    if (peer && peer != c && peer->state == CONN_AUTHED)
        server_room_info(s, peer, ri, 0, "", false);
    update_members(s, ri, c);
    return server_room_info(s, c, ri, request, result, true);
}
