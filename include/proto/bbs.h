#pragma once
#include "proto/tlv.h"
#include <stdbool.h>
#include <stdint.h>

#define BBS_PAGE_SIZE 20
#define BBS_TITLE_SIZE 129
#define BBS_BODY_SIZE 2049

typedef struct {
    uint64_t id, created_at, last_post;
    char title[BBS_TITLE_SIZE], author[33];
    uint32_t post_count;
    bool unread, pinned, locked;
} bbs_thread_t;

typedef struct {
    uint64_t id, created_at;
    char author[33], body[BBS_BODY_SIZE];
    bool deleted, is_self;
} bbs_post_t;

typedef struct {
    bbs_thread_t thread;
    bbs_post_t posts[BBS_PAGE_SIZE];
    int count;
    bool older, newer;
} bbs_view_t;

bool bbs_read_text(const uint8_t *payload, uint32_t length, tlv_tag_t tag, char *out,
                   size_t capacity);
int bbs_put_thread(tlv_builder_t *b, const bbs_thread_t *t);
bool bbs_read_thread(const uint8_t *p, uint32_t length, bbs_thread_t *t);
int bbs_put_post(tlv_builder_t *b, const bbs_post_t *post);
bool bbs_read_post(const uint8_t *p, uint32_t length, bbs_post_t *post);
