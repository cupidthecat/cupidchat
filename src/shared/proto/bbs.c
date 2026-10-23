#include "proto/bbs.h"
#include "proto/profile.h"
#include <string.h>

bool bbs_read_text(const uint8_t *p, uint32_t length, tlv_tag_t tag, char *out, size_t capacity) {
    uint16_t size = 0;
    const uint8_t *raw = tlv_find(p, length, tag, &size);
    if (!raw || size >= capacity || !proto_text_valid(raw, size))
        return false;
    memset(out, 0, capacity);
    memcpy(out, raw, size);
    return true;
}

int bbs_put_thread(tlv_builder_t *b, const bbs_thread_t *t) {
    return tlv_put_u64(b, TAG_THREAD_ID, t->id) < 0 || tlv_put_str(b, TAG_TITLE, t->title) < 0 ||
                   tlv_put_str(b, TAG_BASE_NICK, t->author) < 0 ||
                   tlv_put_u64(b, TAG_TIMESTAMP, t->created_at) < 0 ||
                   tlv_put_u64(b, TAG_POST_ID, t->last_post) < 0 ||
                   tlv_put_u32(b, TAG_COUNT, t->post_count) < 0 ||
                   tlv_put_u8(b, TAG_UNREAD, t->unread) < 0 ||
                   tlv_put_u8(b, TAG_PINNED, t->pinned) < 0 ||
                   tlv_put_u8(b, TAG_LOCKED, t->locked) < 0
               ? -1
               : 0;
}

bool bbs_read_thread(const uint8_t *p, uint32_t length, bbs_thread_t *t) {
    memset(t, 0, sizeof(*t));
    uint8_t unread = 0, pinned = 0, locked = 0;
    bool ok = tlv_read_u64(p, length, TAG_THREAD_ID, &t->id) && t->id &&
              bbs_read_text(p, length, TAG_TITLE, t->title, sizeof(t->title)) && t->title[0] &&
              bbs_read_text(p, length, TAG_BASE_NICK, t->author, sizeof(t->author)) &&
              t->author[0] && tlv_read_u64(p, length, TAG_TIMESTAMP, &t->created_at) &&
              tlv_read_u64(p, length, TAG_POST_ID, &t->last_post) &&
              tlv_read_u32(p, length, TAG_COUNT, &t->post_count);
    tlv_read_u8(p, length, TAG_UNREAD, &unread);
    tlv_read_u8(p, length, TAG_PINNED, &pinned);
    tlv_read_u8(p, length, TAG_LOCKED, &locked);
    t->unread = unread != 0;
    t->pinned = pinned != 0;
    t->locked = locked != 0;
    return ok;
}

int bbs_put_post(tlv_builder_t *b, const bbs_post_t *p) {
    return tlv_put_u64(b, TAG_POST_ID, p->id) < 0 || tlv_put_str(b, TAG_BASE_NICK, p->author) < 0 ||
                   tlv_put_str(b, TAG_TEXT, p->body) < 0 ||
                   tlv_put_u64(b, TAG_TIMESTAMP, p->created_at) < 0 ||
                   tlv_put_u8(b, TAG_DELETED, p->deleted) < 0 ||
                   tlv_put_u8(b, TAG_IS_SELF, p->is_self) < 0
               ? -1
               : 0;
}

bool bbs_read_post(const uint8_t *p, uint32_t length, bbs_post_t *post) {
    memset(post, 0, sizeof(*post));
    uint8_t deleted = 0, self = 0;
    bool ok = tlv_read_u64(p, length, TAG_POST_ID, &post->id) && post->id &&
              bbs_read_text(p, length, TAG_BASE_NICK, post->author, sizeof(post->author)) &&
              post->author[0] &&
              bbs_read_text(p, length, TAG_TEXT, post->body, sizeof(post->body)) &&
              tlv_read_u64(p, length, TAG_TIMESTAMP, &post->created_at);
    tlv_read_u8(p, length, TAG_DELETED, &deleted);
    tlv_read_u8(p, length, TAG_IS_SELF, &self);
    post->deleted = deleted != 0;
    post->is_self = self != 0;
    return ok;
}
