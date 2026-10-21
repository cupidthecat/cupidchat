#include "server/admin.h"
#include "proto/profile.h"
#include "server/log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int fail(server_state_t *s, conn_t *c, error_code_t code, const char *message) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u16(&b, TAG_ERROR_CODE, code);
    tlv_put_str(&b, TAG_ERROR_MSG, message);
    int rc = conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                             (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

static int notice(server_state_t *s, conn_t *c, const char *scope, const char *text,
                  const char *actor) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_str(&b, TAG_SCOPE, scope);
    tlv_put_str(&b, TAG_TEXT, text);
    tlv_put_str(&b, TAG_NICK, actor);
    int rc = conn_send_frame(c, SMSG_SERVER_NOTICE, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s),
                             b.buf, (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

int server_account_flags(server_state_t *s, conn_t *c) {
    account_controls_t flags;
    if (db_get_controls(s->db, c->account_id, &flags) < 0)
        return fail(s, c, ERR_UNKNOWN, "Could not load account permissions");
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u8(&b, TAG_IS_ADMIN, flags.admin);
    tlv_put_u8(&b, TAG_MUTED, flags.muted);
    int rc = conn_send_frame(c, SMSG_ACCOUNT_FLAGS, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s),
                             b.buf, (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

bool server_chat_allowed(server_state_t *s, conn_t *c) {
    account_controls_t flags;
    if (db_get_controls(s->db, c->account_id, &flags) < 0) {
        fail(s, c, ERR_UNKNOWN, "Could not check account permissions");
        return false;
    }
    if (flags.muted) {
        fail(s, c, ERR_PERMISSION_DENIED, "Your account is muted by a server admin");
        return false;
    }
    return true;
}

static bool read_text(const frame_t *f, tlv_tag_t tag, char *out, size_t capacity, bool required) {
    uint16_t length = 0;
    const uint8_t *raw = tlv_find(f->payload, f->hdr.length, tag, &length);
    out[0] = '\0';
    if (!raw)
        return !required;
    if (length >= capacity || !proto_text_valid(raw, length) || (required && !length))
        return false;
    memcpy(out, raw, length);
    out[length] = '\0';
    return true;
}

static int put_poll(tlv_builder_t *b, const poll_info_t *p, bool personal, bool created) {
    if (tlv_put_u64(b, TAG_POLL_ID, p->id) < 0 || tlv_put_str(b, TAG_TEXT, p->question) < 0 ||
        tlv_put_u8(b, TAG_CLOSED, p->closed) < 0 ||
        tlv_put_str(b, TAG_SCOPE, created ? "new" : "update") < 0)
        return -1;
    if (personal && tlv_put_u32(b, TAG_VOTE, p->my_vote) < 0)
        return -1;
    for (int i = 0; i < p->option_count; i++)
        if (tlv_put_str(b, TAG_OPTION, p->options[i]) < 0 ||
            tlv_put_u32(b, TAG_VOTES, p->votes[i]) < 0)
            return -1;
    return 0;
}

static int send_poll(server_state_t *s, conn_t *c, uint64_t id) {
    poll_info_t p;
    int found = db_get_poll(s->db, id, c->account_id, &p);
    if (found < 0)
        return fail(s, c, ERR_UNKNOWN, "Could not load poll");
    tlv_builder_t b;
    tlv_builder_init(&b);
    int rc = put_poll(&b, &p, true, false) < 0
                 ? -1
                 : conn_send_frame(c, SMSG_POLL, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s),
                                   b.buf, (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

static int broadcast_poll(server_state_t *s, bool created) {
    poll_info_t p;
    if (db_get_poll(s->db, 0, 0, &p) < 0)
        return -1;
    tlv_builder_t b;
    tlv_builder_init(&b);
    if (put_poll(&b, &p, false, created) < 0) {
        tlv_builder_free(&b);
        return -1;
    }
    for (int fd = 0; fd < s->conns_size; fd++) {
        conn_t *c = s->conns[fd];
        if (c && c->state == CONN_AUTHED)
            conn_send_frame(c, SMSG_POLL, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                            (uint32_t)b.len, s->cfg);
    }
    tlv_builder_free(&b);
    return 0;
}

int server_poll_command(server_state_t *s, conn_t *c, const frame_t *f) {
    if (!rate_consume(&c->rate))
        return fail(s, c, ERR_RATE_LIMITED, "Slow down");
    uint64_t id = 0;
    uint32_t choice = 0;
    tlv_read_u64(f->payload, f->hdr.length, TAG_POLL_ID, &id);
    if (f->hdr.type == CMSG_POLL_GET)
        return send_poll(s, c, id);
    if (!server_chat_allowed(s, c))
        return 0;
    if (!id || !tlv_read_u32(f->payload, f->hdr.length, TAG_VOTE, &choice) || choice < 1 ||
        choice > POLL_OPTIONS_MAX)
        return fail(s, c, ERR_PROTOCOL, "Choose an option in an open poll");
    if (db_vote_poll(s->db, id, c->account_id, choice) < 0)
        return fail(s, c, ERR_UNKNOWN,
                    "Poll is closed, the option is invalid, or the vote could not be saved");
    broadcast_poll(s, false);
    return send_poll(s, c, id);
}

static char *trim(char *text) {
    while (*text == ' ')
        text++;
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ')
        text[--n] = '\0';
    return text;
}

static int create_poll(server_state_t *s, conn_t *c, char *body) {
    poll_info_t p = {0};
    char *part = body, *next = strchr(part, '|');
    if (!next)
        return fail(s, c, ERR_PROTOCOL,
                    "Usage: /admin poll Question | Choice one | Choice two [| more]");
    *next++ = '\0';
    part = trim(part);
    if (!*part || strlen(part) >= sizeof(p.question))
        return fail(s, c, ERR_PROTOCOL, "Question must be 1-256 UTF-8 bytes");
    snprintf(p.question, sizeof(p.question), "%s", part);
    while (next) {
        part = next;
        next = strchr(part, '|');
        if (next)
            *next++ = '\0';
        part = trim(part);
        if (!*part || strlen(part) >= sizeof(p.options[0]) || p.option_count == POLL_OPTIONS_MAX)
            return fail(s, c, ERR_PROTOCOL,
                        "Use 2-4 nonempty choices, each at most 96 UTF-8 bytes");
        for (int i = 0; i < p.option_count; i++)
            if (!strcmp(part, p.options[i]))
                return fail(s, c, ERR_PROTOCOL, "Poll choices must be different");
        snprintf(p.options[p.option_count++], sizeof(p.options[0]), "%s", part);
    }
    if (p.option_count < 2)
        return fail(s, c, ERR_PROTOCOL, "A poll needs at least two choices");
    int rc = db_create_poll(s->db, c->account_id, &p);
    if (rc < 0)
        return fail(s, c, ERR_UNKNOWN,
                    rc == -2 ? "Close the current poll before creating another"
                             : "Could not create poll");
    return broadcast_poll(s, true);
}

int server_admin_command(server_state_t *s, conn_t *c, const frame_t *f) {
    if (!rate_consume(&c->rate))
        return fail(s, c, ERR_RATE_LIMITED, "Slow down");
    account_controls_t actor;
    if (db_get_controls(s->db, c->account_id, &actor) < 0)
        return fail(s, c, ERR_UNKNOWN, "Could not check admin permissions");
    if (!actor.admin)
        return fail(s, c, ERR_PERMISSION_DENIED, "This command requires a server admin");
    char action[33], account[33], body[1801];
    if (!read_text(f, TAG_ADMIN_ACTION, action, sizeof(action), true) ||
        !read_text(f, TAG_BASE_NICK, account, sizeof(account), false) ||
        !read_text(f, TAG_TEXT, body, sizeof(body), false))
        return fail(s, c, ERR_PROTOCOL, "Invalid or oversized admin command");
    if (!strcmp(action, "motd") || !strcmp(action, "banner")) {
        if (!strcmp(action, "banner") && strlen(body) > 512)
            return fail(s, c, ERR_PROTOCOL, "Banner limit is 512 UTF-8 bytes");
        if (!strcmp(body, "clear"))
            body[0] = '\0';
        if (!strcmp(action, "motd"))
            for (char *p = body; *p; p++)
                if (*p == '|')
                    *p = '\n';
        if (db_set_setting(s->db, action, body) < 0)
            return fail(s, c, ERR_UNKNOWN, "Could not save server message");
        for (int fd = 0; fd < s->conns_size; fd++) {
            conn_t *peer = s->conns[fd];
            if (peer && peer->state == CONN_AUTHED)
                notice(s, peer, action, body, c->base_nick);
        }
        LOG_INFO("admin=%s updated %s", c->base_nick, action);
        return 0;
    }
    if (!strcmp(action, "poll"))
        return create_poll(s, c, body);
    if (!strcmp(action, "pollclose")) {
        poll_info_t current;
        if (db_get_poll(s->db, 0, 0, &current) != 1 || current.closed)
            return fail(s, c, ERR_UNKNOWN, "There is no open poll to close");
        if (db_close_poll(s->db) < 0)
            return fail(s, c, ERR_UNKNOWN, "Could not close poll");
        return broadcast_poll(s, false);
    }
    const char *allowed[] = {"grant", "revoke", "kick", "ban", "unban", "mute", "unmute"};
    bool known = false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (!strcmp(action, allowed[i]))
            known = true;
    if (!known || !account[0])
        return fail(s, c, ERR_PROTOCOL, "Unknown admin action or missing account name");
    if (strlen(body) > 256)
        return fail(s, c, ERR_PROTOCOL, "Reason limit is 256 UTF-8 bytes");
    uint64_t target = db_find_account(s->db, account);
    if (!target || target == UINT64_MAX)
        return fail(s, c, ERR_UNKNOWN, "Could not find that account");
    account_controls_t flags;
    if (db_get_controls(s->db, target, &flags) < 0)
        return fail(s, c, ERR_UNKNOWN, "Could not load account controls");
    bool disconnect = !strcmp(action, "kick") || !strcmp(action, "ban");
    if (flags.admin && (disconnect || !strcmp(action, "mute")))
        return fail(s, c, ERR_PERMISSION_DENIED,
                    "Revoke admin access before moderating an admin account");
    if (flags.banned && !strcmp(action, "grant"))
        return fail(s, c, ERR_PERMISSION_DENIED, "Unban the account before granting admin access");
    conn_t *peer = server_find_by_account(s, target);
    if (!strcmp(action, "kick") && !peer)
        return fail(s, c, ERR_UNKNOWN, "That account is offline");
    int rc = db_control_account(s->db, c->account_id, target, action, body);
    if (rc < 0)
        return fail(s, c, ERR_UNKNOWN,
                    rc == -2 ? "Cannot remove the last server admin"
                             : "Could not save account controls");
    if (peer) {
        char explanation[384];
        snprintf(explanation, sizeof(explanation), "Admin action: %s%s%s", action,
                 body[0] ? " - " : "", body);
        if (disconnect) {
            fail(s, peer, ERR_PERMISSION_DENIED, explanation);
            peer->state = CONN_CLOSING;
            peer->close_deadline = time(NULL) + 2;
        } else {
            server_account_flags(s, peer);
            notice(s, peer, "admin", explanation, c->base_nick);
        }
    }
    char confirmation[128];
    snprintf(confirmation, sizeof(confirmation), "%s: %s", action, account);
    LOG_INFO("admin=%s action=%s account=%s", c->base_nick, action, account);
    return notice(s, c, "admin", confirmation, c->base_nick);
}
