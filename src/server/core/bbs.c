#include "server/bbs.h"
#include "proto/bbs.h"
#include <string.h>

static int error(server_state_t *s, conn_t *c, uint32_t request, error_code_t code,
                 const char *text) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u32(&b, TAG_REQUEST_ID, request);
    tlv_put_str(&b, TAG_SCOPE, "bbs");
    tlv_put_u16(&b, TAG_ERROR_CODE, code);
    tlv_put_str(&b, TAG_ERROR_MSG, text);
    int rc = conn_send_frame(c, SMSG_ERROR, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b.buf,
                             (uint32_t)b.len, s->cfg);
    tlv_builder_free(&b);
    return rc;
}

static int send(server_state_t *s, conn_t *c, uint16_t type, tlv_builder_t *b) {
    int rc = conn_send_frame(c, type, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s), b->buf,
                             (uint32_t)b->len, s->cfg);
    tlv_builder_free(b);
    return rc;
}

static void changed(server_state_t *s, conn_t *actor, uint64_t thread, uint64_t post,
                    const char *action) {
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u64(&b, TAG_THREAD_ID, thread);
    tlv_put_u64(&b, TAG_POST_ID, post);
    tlv_put_str(&b, TAG_BASE_NICK, actor->base_nick);
    tlv_put_str(&b, TAG_SCOPE, action);
    for (int fd = 0; fd < s->conns_size; fd++) {
        conn_t *c = s->conns[fd];
        if (c && c->state == CONN_AUTHED)
            conn_send_frame(c, SMSG_BBS_CHANGED, FRAME_FLAG_PRIORITY_HIGH, server_next_seq(s),
                            b.buf, (uint32_t)b.len, s->cfg);
    }
    tlv_builder_free(&b);
}

int server_bbs_command(server_state_t *s, conn_t *c, const frame_t *f) {
    uint32_t request = 0;
    tlv_read_u32(f->payload, f->hdr.length, TAG_REQUEST_ID, &request);
    if (!c->account_id)
        return error(s, c, request, ERR_AUTH_REQUIRED, "Log in to use the BBS board");
    if (!rate_consume(&c->rate))
        return error(s, c, request, ERR_RATE_LIMITED, "Slow down and try again");
    if (f->hdr.type == CMSG_BBS_STATUS) {
        uint32_t unread = 0;
        if (db_bbs_status(s->db, c->account_id, &unread) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not load board status");
        tlv_builder_t b;
        tlv_builder_init(&b);
        tlv_put_u32(&b, TAG_UNREAD, unread);
        return send(s, c, SMSG_BBS_STATUS, &b);
    }
    if (!request)
        return error(s, c, request, ERR_PROTOCOL, "A board request ID is required");
    if (f->hdr.type == CMSG_BBS_LIST) {
        uint32_t offset = 0;
        tlv_read_u32(f->payload, f->hdr.length, TAG_OFFSET, &offset);
        bbs_thread_t rows[BBS_PAGE_SIZE];
        int count;
        bool more;
        if (db_bbs_list(s->db, c->account_id, offset, rows, &count, &more) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not load the board");
        tlv_builder_t b;
        tlv_builder_init(&b);
        tlv_put_u32(&b, TAG_REQUEST_ID, request);
        tlv_put_u32(&b, TAG_OFFSET, offset);
        tlv_put_u8(&b, TAG_MORE, more);
        bool failed = false;
        for (int i = 0; i < count; i++) {
            tlv_builder_t entry;
            tlv_builder_init(&entry);
            if (bbs_put_thread(&entry, &rows[i]) < 0 ||
                tlv_put_bytes(&b, TAG_ENTRY, entry.buf, (uint16_t)entry.len) < 0)
                failed = true;
            tlv_builder_free(&entry);
        }
        if (failed) {
            tlv_builder_free(&b);
            return -1;
        }
        return send(s, c, SMSG_BBS_LIST, &b);
    }
    uint64_t thread = 0, post = 0;
    tlv_read_u64(f->payload, f->hdr.length, TAG_THREAD_ID, &thread);
    tlv_read_u64(f->payload, f->hdr.length, TAG_POST_ID, &post);
    if (f->hdr.type == CMSG_BBS_GET) {
        uint64_t before = 0, after = 0;
        tlv_read_u64(f->payload, f->hdr.length, TAG_BEFORE_ID, &before);
        tlv_read_u64(f->payload, f->hdr.length, TAG_AFTER_ID, &after);
        if (!thread || (before && after))
            return error(s, c, request, ERR_PROTOCOL, "Invalid thread page");
        bbs_view_t view;
        if (db_bbs_view(s->db, c->account_id, thread, before, after, &view) != 1)
            return error(s, c, request, ERR_UNKNOWN, "Thread or page not found");
        uint64_t last = view.posts[view.count - 1].id;
        if (db_bbs_read(s->db, c->account_id, thread, last) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not save the read marker");
        if (last == view.thread.last_post)
            view.thread.unread = false;
        tlv_builder_t b;
        tlv_builder_init(&b);
        bool failed = bbs_put_thread(&b, &view.thread) < 0;
        tlv_put_u32(&b, TAG_REQUEST_ID, request);
        tlv_put_u8(&b, TAG_OLDER, view.older);
        tlv_put_u8(&b, TAG_NEWER, view.newer);
        for (int i = 0; i < view.count; i++) {
            tlv_builder_t entry;
            tlv_builder_init(&entry);
            if (bbs_put_post(&entry, &view.posts[i]) < 0 ||
                tlv_put_bytes(&b, TAG_ENTRY, entry.buf, (uint16_t)entry.len) < 0)
                failed = true;
            tlv_builder_free(&entry);
        }
        if (failed) {
            tlv_builder_free(&b);
            return -1;
        }
        return send(s, c, SMSG_BBS_THREAD, &b);
    }
    account_controls_t controls;
    if (db_get_controls(s->db, c->account_id, &controls) < 0)
        return error(s, c, request, ERR_UNKNOWN, "Could not check account permissions");
    const char *action;
    if (f->hdr.type == CMSG_BBS_POST) {
        if (controls.muted)
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "Muted accounts cannot post to the board");
        char title[BBS_TITLE_SIZE] = {0}, body[BBS_BODY_SIZE] = {0};
        if ((!thread &&
             (!bbs_read_text(f->payload, f->hdr.length, TAG_TITLE, title, sizeof(title)) ||
              !title[0])) ||
            !bbs_read_text(f->payload, f->hdr.length, TAG_TEXT, body, sizeof(body)) || !body[0])
            return error(
                s, c, request, ERR_PROTOCOL,
                "Use a subject up to 128 bytes and a body up to 2048 bytes of plain UTF-8 text");
        if ((!thread && strspn(title, " ") == strlen(title)) || strspn(body, " ") == strlen(body))
            return error(s, c, request, ERR_PROTOCOL, "The subject and message cannot be blank");
        int rc = db_bbs_post(s->db, c->account_id, &thread, title, body, &post);
        if (rc < 0)
            return error(s, c, request, ERR_UNKNOWN,
                         rc == -2   ? "That thread is locked or no longer available"
                         : rc == -4 ? "Wait five seconds between board posts"
                                    : "Could not save the post");
        action = "post";
    } else if (f->hdr.type == CMSG_BBS_DELETE) {
        if (!post)
            return error(s, c, request, ERR_PROTOCOL, "Select a post to remove");
        int rc = db_bbs_delete(s->db, c->account_id, controls.admin, post, &thread);
        if (rc < 0)
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         rc == -2 ? "You can only remove your own posts"
                                  : "Could not remove the post");
        action = "delete";
    } else {
        if (!controls.admin)
            return error(s, c, request, ERR_PERMISSION_DENIED,
                         "Only admins can pin or lock threads");
        uint8_t value = 0;
        char setting[8];
        if (!thread ||
            !bbs_read_text(f->payload, f->hdr.length, TAG_SCOPE, setting, sizeof(setting)) ||
            (strcmp(setting, "pin") && strcmp(setting, "lock")) ||
            !tlv_read_u8(f->payload, f->hdr.length,
                         !strcmp(setting, "pin") ? TAG_PINNED : TAG_LOCKED, &value) ||
            value > 1)
            return error(s, c, request, ERR_PROTOCOL, "Invalid thread setting");
        if (db_bbs_flags(s->db, thread, setting, value != 0) < 0)
            return error(s, c, request, ERR_UNKNOWN, "Could not update the thread");
        action = "flags";
    }
    tlv_builder_t b;
    tlv_builder_init(&b);
    tlv_put_u32(&b, TAG_REQUEST_ID, request);
    tlv_put_u64(&b, TAG_THREAD_ID, thread);
    tlv_put_u64(&b, TAG_POST_ID, post);
    tlv_put_str(&b, TAG_SCOPE, action);
    int rc = send(s, c, SMSG_BBS_SAVED, &b);
    changed(s, c, thread, post, action);
    return rc;
}
