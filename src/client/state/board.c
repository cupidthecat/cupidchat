#include "client/board_flow.h"
#include "client/sound.h"
#include "client/ui.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

static void message(ui_state_t *ui, cli_model_t *m, const char *text) {
    snprintf(ui->board.message, sizeof(ui->board.message), "%.191s", text);
    model_set_status(m, text, 6);
    ui->dirty = true;
}

static void request(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, board_request_t r) {
    board_state_t *b = &ui->board;
    if (++b->request == 0)
        b->request++;
    r.request = b->request;
    b->pending = true;
    b->message[0] = '\0';
    if (client_send_board(cc, &r) < 0) {
        b->pending = b->saving = false;
        message(ui, m, "Could not send board request");
    }
    ui->dirty = true;
}

static void status(client_conn_t *cc) {
    client_send_board(cc, &(board_request_t){.type = CMSG_BBS_STATUS});
}

void board_action(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, int action) {
    board_state_t *b = &ui->board;
    if (!(m->server_caps & CAP_BBS)) {
        message(ui, m, "This server does not support the BBS board");
        return;
    }
    if (b->pending)
        return;
    if (action == UI_BOARD_OPEN) {
        bool reopening = !b->visible;
        b->visible = true;
        ui->show_poll = ui->show_buddies_modal = ui->show_emoji_modal = ui->show_help_modal = false;
        if (reopening && b->loaded) {
            ui->dirty = true;
            return;
        }
        request(ui, m, cc,
                (board_request_t){.type = b->thread_mode ? CMSG_BBS_GET : CMSG_BBS_LIST,
                                  .thread = b->view.thread.id,
                                  .offset = b->offset});
    } else if (action == UI_BOARD_LIST) {
        request(ui, m, cc, (board_request_t){.type = CMSG_BBS_LIST, .offset = b->offset});
    } else if (action == UI_BOARD_THREAD) {
        if (b->cursor >= 0 && b->cursor < b->count)
            request(ui, m, cc,
                    (board_request_t){.type = CMSG_BBS_GET, .thread = b->threads[b->cursor].id});
    } else if (action == UI_BOARD_PREV || action == UI_BOARD_NEXT) {
        bool previous = action == UI_BOARD_PREV;
        if (b->thread_mode) {
            if (!b->view.count || (previous ? !b->view.older : !b->view.newer)) {
                message(ui, m, previous ? "This is the oldest page" : "This is the newest page");
                return;
            }
            request(ui, m, cc,
                    (board_request_t){.type = CMSG_BBS_GET,
                                      .thread = b->view.thread.id,
                                      .before = previous ? b->view.posts[0].id : 0,
                                      .after = previous ? 0 : b->view.posts[b->view.count - 1].id});
        } else {
            if (previous ? !b->offset : (!b->more || b->offset > UINT32_MAX - BBS_PAGE_SIZE)) {
                message(ui, m, previous ? "This is the first page" : "This is the last page");
                return;
            }
            uint32_t offset = previous ? b->offset - BBS_PAGE_SIZE : b->offset + BBS_PAGE_SIZE;
            request(ui, m, cc, (board_request_t){.type = CMSG_BBS_LIST, .offset = offset});
        }
    } else if (action == UI_BOARD_POST) {
        if (!b->composing)
            return;
        if ((!b->reply_thread && (!b->title[0] || strspn(b->title, " ") == strlen(b->title))) ||
            !b->body[0] || strspn(b->body, " ") == strlen(b->body)) {
            message(ui, m, "Enter a subject and a message before posting");
            return;
        }
        b->saving = true;
        request(ui, m, cc,
                (board_request_t){.type = CMSG_BBS_POST,
                                  .thread = b->reply_thread,
                                  .title = b->title,
                                  .body = b->body});
    } else if (action == UI_BOARD_DELETE) {
        if (!b->thread_mode || b->cursor < 0 || b->cursor >= b->view.count)
            return;
        b->saving = true;
        request(ui, m, cc,
                (board_request_t){.type = CMSG_BBS_DELETE, .post = b->view.posts[b->cursor].id});
    } else if (action == UI_BOARD_PIN || action == UI_BOARD_LOCK) {
        if (!m->is_admin || !b->thread_mode)
            return;
        bool pin = action == UI_BOARD_PIN;
        b->saving = true;
        request(ui, m, cc,
                (board_request_t){.type = CMSG_BBS_FLAGS,
                                  .thread = b->view.thread.id,
                                  .setting = pin ? "pin" : "lock",
                                  .value = pin ? !b->view.thread.pinned : !b->view.thread.locked});
    }
}

bool board_frame(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const frame_t *f) {
    const uint8_t *p = f->payload;
    uint32_t length = f->hdr.length;
    board_state_t *b = &ui->board;
    if (f->hdr.type == SMSG_ERROR) {
        char scope[16] = {0};
        tlv_read_str(p, length, TAG_SCOPE, scope, sizeof(scope));
        if (strcmp(scope, "bbs"))
            return false;
        uint32_t id = 0;
        tlv_read_u32(p, length, TAG_REQUEST_ID, &id);
        if (id == b->request) {
            char text[192] = {0};
            tlv_read_str(p, length, TAG_ERROR_MSG, text, sizeof(text));
            b->pending = b->saving = false;
            message(ui, m, text);
            sound_play(SND_ERROR);
        }
        return true;
    }
    if (f->hdr.type < SMSG_BBS_STATUS || f->hdr.type > SMSG_BBS_CHANGED)
        return false;
    if (!m->logged_in)
        return true;
    ui->dirty = true;
    if (f->hdr.type == SMSG_BBS_STATUS) {
        tlv_read_u32(p, length, TAG_UNREAD, &m->board_unread);
        return true;
    }
    if (f->hdr.type == SMSG_BBS_CHANGED) {
        char author[33] = {0}, scope[16] = {0};
        tlv_read_str(p, length, TAG_BASE_NICK, author, sizeof(author));
        tlv_read_str(p, length, TAG_SCOPE, scope, sizeof(scope));
        b->stale = true;
        if (!b->composing && !b->pending)
            b->message[0] = '\0';
        status(cc);
        if (!strcmp(scope, "post") && strcasecmp(author, m->base_nick) &&
            !model_is_ignored(m, author)) {
            sound_play(SND_BOARD);
            model_set_status(m, "New BBS post: F9 opens the board", 8);
        }
        return true;
    }
    uint32_t id = 0;
    tlv_read_u32(p, length, TAG_REQUEST_ID, &id);
    if (id != b->request || !b->pending)
        return true;
    if (f->hdr.type == SMSG_BBS_SAVED) {
        uint64_t thread = 0;
        tlv_read_u64(p, length, TAG_THREAD_ID, &thread);
        if (!thread || !b->saving)
            return true;
        bool posted = b->composing;
        b->pending = b->saving = b->composing = false;
        explicit_bzero(b->title, sizeof(b->title));
        explicit_bzero(b->body, sizeof(b->body));
        request(ui, m, cc, (board_request_t){.type = CMSG_BBS_GET, .thread = thread});
        message(ui, m, posted ? "Post saved" : "Board updated");
        if (posted)
            sound_play(SND_PROFILE_SAVED);
        return true;
    }
    bool valid = true;
    tlv_reader_t reader;
    tlv_field_t field;
    tlv_reader_init(&reader, p, length);
    if (f->hdr.type == SMSG_BBS_LIST) {
        bbs_thread_t entries[BBS_PAGE_SIZE];
        int count = 0;
        uint32_t offset = 0;
        uint8_t more = 0;
        while (tlv_reader_next(&reader, &field))
            if (field.tag == TAG_ENTRY) {
                if (count == BBS_PAGE_SIZE ||
                    !bbs_read_thread(field.val, field.len, &entries[count])) {
                    valid = false;
                    break;
                }
                count++;
            }
        if (valid) {
            tlv_read_u32(p, length, TAG_OFFSET, &offset);
            tlv_read_u8(p, length, TAG_MORE, &more);
            memcpy(b->threads, entries, (size_t)count * sizeof(entries[0]));
            b->count = count;
            b->offset = offset;
            b->more = more != 0;
            b->thread_mode = false;
            b->cursor = 0;
        }
    } else {
        bbs_view_t view = {0};
        valid = bbs_read_thread(p, length, &view.thread);
        while (valid && tlv_reader_next(&reader, &field))
            if (field.tag == TAG_ENTRY) {
                if (view.count == BBS_PAGE_SIZE ||
                    !bbs_read_post(field.val, field.len, &view.posts[view.count])) {
                    valid = false;
                    break;
                }
                view.count++;
            }
        if (!view.count)
            valid = false;
        if (valid) {
            uint8_t older = 0, newer = 0;
            tlv_read_u8(p, length, TAG_OLDER, &older);
            tlv_read_u8(p, length, TAG_NEWER, &newer);
            view.older = older != 0;
            view.newer = newer != 0;
            b->view = view;
            b->thread_mode = true;
            b->cursor = b->scroll = 0;
            status(cc);
        }
    }
    b->pending = false;
    if (valid) {
        b->stale = false;
        b->loaded = true;
    } else
        message(ui, m, "Invalid board response; refresh to retry");
    return true;
}
