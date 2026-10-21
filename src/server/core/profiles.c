#include "server/profiles.h"
#include "proto/profile.h"
#include <string.h>

static int profile_error(server_state_t *s, conn_t *c, uint32_t request, error_code_t code,
                         const char *message) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u32(&b, TAG_REQUEST_ID, request);
    tlv_put_u16(&b, TAG_ERROR_CODE, code);
    tlv_put_str(&b, TAG_ERROR_MSG, message);
    int rc = conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                             (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

int server_profile_command(server_state_t *s, conn_t *c, const frame_t *f) {
    uint32_t request = 0, uid = 0;
    tlv_read_u32(f->payload, f->hdr.length, TAG_REQUEST_ID, &request);
    if (!rate_consume(&c->rate))
        return profile_error(s, c, request, ERR_RATE_LIMITED, "Slow down and try again");
    uint64_t account = c->account_id;
    char name[MAX_NICK_LEN + 1] = {0};
    bool save = f->hdr.type == CMSG_PROFILE_SET;
    if (save) {
        profile_fields_t fields;
        if (profile_fields_read(f->payload, f->hdr.length, &fields) < 0)
            return profile_error(
                s, c, request, ERR_PROTOCOL,
                "Profile fields must fit their limits and contain plain UTF-8 text");
        if (db_set_profile(s->db, account, &fields) < 0)
            return profile_error(s, c, request, ERR_UNKNOWN, "Could not save your profile");
    } else {
        uint16_t length = 0;
        const uint8_t *raw = tlv_find(f->payload, f->hdr.length, TAG_BASE_NICK, &length);
        bool by_account = raw != NULL;
        if (!raw)
            raw = tlv_find(f->payload, f->hdr.length, TAG_NICK, &length);
        if (raw) {
            if (!length || length > MAX_NICK_LEN || memchr(raw, 0, length))
                return profile_error(s, c, request, ERR_PROTOCOL, "Invalid profile name");
            memcpy(name, raw, length);
        }
        tlv_read_u32(f->payload, f->hdr.length, TAG_USER_ID, &uid);
        if (uid) {
            conn_t *peer = server_find_by_uid(s, uid);
            if (!peer || peer->state != CONN_AUTHED || (name[0] && strcmp(name, peer->nick)))
                return profile_error(
                    s, c, request, ERR_UNKNOWN,
                    "That user has left or changed their nickname; refresh the list");
            account = peer->account_id;
        } else if (name[0]) {
            conn_t *peer = by_account ? NULL : server_find_by_nick(s, name);
            account = peer ? peer->account_id : 0;
        }
    }
    profile_info_t info = {0};
    uint64_t found = 0;
    int rc = db_get_profile(s->db, account, name, &info, &found);
    if (rc != 1)
        return profile_error(s, c, request, ERR_UNKNOWN,
                             rc == 0 ? "No such account" : "Could not load profile");
    conn_t *peer = server_find_by_account(s, found);
    info.is_self = found == c->account_id;
    tlv_builder_t b;
    tlv_builder_init(&b);
    bool failed = profile_fields_write(&b, &info.fields) < 0 ||
                  tlv_put_u32(&b, TAG_REQUEST_ID, request) < 0 ||
                  tlv_put_str(&b, TAG_BASE_NICK, info.account) < 0 ||
                  tlv_put_str(&b, TAG_NICK, peer ? peer->nick : "") < 0 ||
                  tlv_put_u32(&b, TAG_USER_ID, peer ? peer->user_id : 0) < 0 ||
                  tlv_put_u8(&b, TAG_PRESENCE, peer ? (peer->is_away ? 2 : 1) : 0) < 0 ||
                  tlv_put_str(&b, TAG_AWAY_MSG, peer && peer->is_away ? peer->away_msg : "") < 0 ||
                  tlv_put_u64(&b, TAG_MEMBER_SINCE, info.member_since) < 0 ||
                  tlv_put_u8(&b, TAG_IS_ACCOUNT, 1) < 0 ||
                  tlv_put_u8(&b, TAG_IS_SELF, info.is_self) < 0;
    rc = failed ? -1
                : conn_send_frame(c, save ? SMSG_PROFILE_SAVED : SMSG_PROFILE,
                                  FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                                  (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}
