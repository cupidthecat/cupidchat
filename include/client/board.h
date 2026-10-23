#pragma once
#include "proto/bbs.h"

typedef struct {
    bool visible, pending, saving, composing, discard, confirm_delete, stale, more, follow;
    uint32_t request, offset;
    uint64_t reply_thread;
    bbs_thread_t threads[BBS_PAGE_SIZE];
    int count, cursor, scroll, focus, cursors[2], body_scroll;
    bbs_view_t view;
    char title[BBS_TITLE_SIZE], body[BBS_BODY_SIZE], message[192];
    int buttons[8][4], button_count, post_rows[BBS_PAGE_SIZE][2];
    bool thread_mode, loaded;
} board_state_t;

typedef struct {
    uint16_t type;
    uint32_t request, offset;
    uint64_t thread, post, before, after;
    const char *title, *body, *setting;
    bool value;
} board_request_t;
