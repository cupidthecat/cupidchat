#include "client/forms.h"
#include "client/text.h"
#include "client/ui.h"
#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { NEW = -1, REPLY = -2, AUTHOR = -3, CLOSE = -4, CANCEL = -5, REMOVE = -6 };

static void geometry(const ui_state_t *ui, int *h, int *w, int *y, int *x) {
    *h = ui->layout.rows - 2;
    if (*h > 28)
        *h = 28;
    *w = ui->layout.cols - 2;
    if (*w > 100)
        *w = 100;
    *y = (ui->layout.rows - *h) / 2;
    *x = (ui->layout.cols - *w) / 2;
}

static void clear_draft(board_state_t *b) {
    explicit_bzero(b->title, sizeof(b->title));
    explicit_bzero(b->body, sizeof(b->body));
    b->composing = b->discard = false;
    b->body_scroll = 0;
}

static int activate(ui_state_t *ui, const cli_model_t *m, int action) {
    board_state_t *b = &ui->board;
    if (action == NEW || action == REPLY) {
        if (m->is_muted || (action == REPLY && b->view.thread.locked)) {
            snprintf(b->message, sizeof(b->message), "%s",
                     m->is_muted ? "Your account is muted" : "This thread is locked");
            return UI_NONE;
        }
        clear_draft(b);
        b->composing = true;
        b->reply_thread = action == REPLY ? b->view.thread.id : 0;
        b->focus = b->reply_thread ? 1 : 0;
        b->cursors[0] = b->cursors[1] = 0;
        b->message[0] = '\0';
    } else if (action == AUTHOR) {
        const char *account =
            b->thread_mode && b->cursor < b->view.count ? b->view.posts[b->cursor].author
            : !b->thread_mode && b->cursor < b->count   ? b->threads[b->cursor].author
                                                        : NULL;
        if (account)
            return ui_profile_target(ui, 0, NULL, account);
    } else if (action == CLOSE)
        b->visible = false;
    else if (action == CANCEL) {
        if (b->title[0] || b->body[0])
            b->discard = true;
        else
            clear_draft(b);
    } else if (action == REMOVE) {
        if (b->cursor < b->view.count && !b->view.posts[b->cursor].deleted &&
            (b->view.posts[b->cursor].is_self || m->is_admin))
            b->confirm_delete = true;
        else
            snprintf(b->message, sizeof(b->message),
                     "Only the author or an admin can remove this post");
    } else
        return action;
    return UI_NONE;
}

int ui_board_input(ui_state_t *ui, const cli_model_t *m, int key, uint32_t character,
                   bool printable) {
    board_state_t *b = &ui->board;
    ui->modal_dirty = true;
    if (ui->layout.too_small)
        return UI_NONE;
    if (b->discard || b->confirm_delete) {
        if (key == 'y' || key == 'Y' || key == '\n' || key == KEY_ENTER) {
            if (b->confirm_delete) {
                b->confirm_delete = false;
                return UI_BOARD_DELETE;
            }
            clear_draft(b);
        } else if (key == 27 || key == 'n' || key == 'N')
            b->discard = b->confirm_delete = false;
        return UI_NONE;
    }
    if (b->pending) {
        if (key == 27 && !b->saving)
            b->visible = false;
        return UI_NONE;
    }
    if (key == 27)
        return activate(ui, m, b->composing ? CANCEL : b->thread_mode ? UI_BOARD_LIST : CLOSE);
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    if (key == KEY_MOUSE) {
        MEVENT mouse;
        if (getmouse(&mouse) != OK)
            return UI_NONE;
        int row = mouse.y - y, col = mouse.x - x;
        if (row < 1 || row >= h - 1 || col < 1 || col >= w - 1)
            return UI_NONE;
        if (!b->composing) {
            int delta = 0;
            if (mouse.bstate & BUTTON4_PRESSED)
                delta = -3;
#ifdef BUTTON5_PRESSED
            if (mouse.bstate & BUTTON5_PRESSED)
                delta = 3;
#endif
            if (b->thread_mode)
                b->scroll += delta;
            else {
                b->cursor += delta;
                if (b->cursor < 0)
                    b->cursor = 0;
                if (b->cursor >= b->count)
                    b->cursor = b->count ? b->count - 1 : 0;
            }
        }
        if (!(mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
            return UI_NONE;
        for (int i = 0; i < b->button_count; i++) {
            int *hit = b->buttons[i];
            if (row == hit[0] && col >= hit[1] && col < hit[2])
                return activate(ui, m, hit[3]);
        }
        if (b->composing) {
            if (row == 1 && !b->reply_thread)
                b->focus = 0;
            else if (row >= 2 && row < h - 3)
                b->focus = 1;
        } else if (!b->thread_mode && row >= 2 && row < h - 3) {
            int visible = h - 5, index = b->cursor / visible * visible + row - 2;
            if (index < b->count) {
                b->cursor = index;
                return UI_BOARD_THREAD;
            }
        } else if (b->thread_mode) {
            for (int i = 0; i < b->view.count; i++)
                if (row == b->post_rows[i][0]) {
                    b->cursor = i;
                    if (col < 3 + (int)strlen(b->view.posts[i].author))
                        return activate(ui, m, AUTHOR);
                }
        }
        return UI_NONE;
    }
    if (b->composing) {
        if (key == KEY_F(7))
            return ui_profile_target(ui, 0, NULL, m->base_nick);
        if (key == 19)
            return UI_BOARD_POST;
        int first = b->reply_thread ? 1 : 0, total = 4 - first;
        if (key == '\t' || key == KEY_DOWN)
            b->focus = first + (b->focus - first + 1) % total;
        else if (key == KEY_BTAB || key == KEY_UP)
            b->focus = first + (b->focus - first + total - 1) % total;
        else if (key == '\n' || key == KEY_ENTER) {
            if (b->focus < 2)
                b->focus++;
            else
                return activate(ui, m, b->focus == 2 ? UI_BOARD_POST : CANCEL);
        } else if (b->focus < 2) {
            int i = b->focus;
            if (!form_edit(i ? b->body : b->title, i ? sizeof(b->body) : sizeof(b->title),
                           &b->cursors[i], key, character, printable) &&
                printable && character >= 32)
                snprintf(b->message, sizeof(b->message), "Field limit: %d UTF-8 bytes",
                         i ? BBS_BODY_SIZE - 1 : BBS_TITLE_SIZE - 1);
            else
                b->message[0] = '\0';
        }
        return UI_NONE;
    }
    if (key == 'n' || key == 'N')
        return activate(ui, m, NEW);
    if (key == 'a' || key == 'A')
        return activate(ui, m, b->thread_mode ? REPLY : AUTHOR);
    if (key == 'u' || key == 'U')
        return activate(ui, m, AUTHOR);
    if (key == 'r' || key == 'R')
        return UI_BOARD_OPEN;
    if (key == KEY_LEFT)
        return UI_BOARD_PREV;
    if (key == KEY_RIGHT)
        return UI_BOARD_NEXT;
    if ((key == 'd' || key == 'D') && b->thread_mode)
        return activate(ui, m, REMOVE);
    if ((key == 'p' || key == 'P') && b->thread_mode && m->is_admin)
        return UI_BOARD_PIN;
    if ((key == 'l' || key == 'L') && b->thread_mode && m->is_admin)
        return UI_BOARD_LOCK;
    int count = b->thread_mode ? b->view.count : b->count;
    if (key == KEY_DOWN && count) {
        b->cursor = (b->cursor + 1) % count;
        b->follow = true;
    } else if (key == KEY_UP && count) {
        b->cursor = (b->cursor + count - 1) % count;
        b->follow = true;
    } else if (key == KEY_PPAGE || key == KEY_NPAGE) {
        if (b->thread_mode)
            b->scroll += key == KEY_PPAGE ? -(h - 5) : h - 5;
        else {
            b->cursor += key == KEY_PPAGE ? -(h - 5) : h - 5;
            if (b->cursor < 0)
                b->cursor = 0;
            if (b->cursor >= count)
                b->cursor = count ? count - 1 : 0;
        }
    } else if (key == '\n' || key == KEY_ENTER)
        return b->thread_mode ? activate(ui, m, REPLY) : count ? UI_BOARD_THREAD : UI_NONE;
    return UI_NONE;
}

static void line(WINDOW *win, const char *text, int width, int offset, int visible, int *logical,
                 bool selected) {
    int rows = text_wrap_rows(text, width);
    if (selected)
        wattron(win, A_REVERSE);
    int first = offset > *logical ? offset - *logical : 0;
    int last = offset + visible - *logical;
    if (last > rows)
        last = rows;
    int byte = first < last ? text_row_offset(text, width, first) : 0;
    for (int i = first; i < last; i++) {
        char part[BBS_BODY_SIZE + 256];
        byte = text_wrap_next_line(text, width, byte, part, sizeof(part));
        mvwaddstr(win, 2 + *logical + i - offset, 2, part);
    }
    *logical += rows;
    wattroff(win, A_REVERSE);
}

static void button(board_state_t *b, WINDOW *win, int row, int *col, const char *label, int action,
                   bool selected) {
    if (selected)
        wattron(win, A_REVERSE);
    mvwaddstr(win, row, *col, label);
    wattroff(win, A_REVERSE);
    int *hit = b->buttons[b->button_count++];
    hit[0] = row;
    hit[1] = *col;
    hit[2] = *col + (int)strlen(label);
    hit[3] = action;
    *col = hit[2] + 1;
}

static void post_header(const bbs_post_t *p, char header[128]) {
    time_t stamp = (time_t)(p->created_at / 1000);
    struct tm date;
    char when[32] = "";
    if (localtime_r(&stamp, &date))
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &date);
    snprintf(header, 128, "@%s | %s | #%llu", p->author, when, (unsigned long long)p->id);
}

void ui_redraw_board(ui_state_t *ui, const cli_model_t *m) {
    board_state_t *b = &ui->board;
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    WINDOW *win = ui_modal_window(ui, UI_MODAL_BOARD, h, w, y, x);
    if (!win)
        return;
    box(win, 0, 0);
    mvwaddstr(win, 0, 2,
              b->composing ? (b->reply_thread ? " Reply to thread " : " New BBS thread ")
                           : " CupidChat BBS ");
    b->button_count = 0;
    int visible = h - 5, cursor_y = -1, cursor_x = 0;
    if (b->composing) {
        const char *subject = b->reply_thread ? b->view.thread.title : b->title;
        char label[160];
        snprintf(label, sizeof(label), "Subject: %s", subject);
        char part[BBS_BODY_SIZE + 2];
        text_wrap_line(label, w - 4, 0, part, sizeof(part));
        mvwaddstr(win, 1, 2, part);
        if (b->focus == 0) {
            int width = w - 13;
            int offset = text_view_offset(b->title, 0, b->cursors[0], width);
            int end = text_cell_offset(b->title, offset, width);
            mvwhline(win, 1, 11, ' ', width);
            mvwaddnstr(win, 1, 11, b->title + offset, end - offset);
            cursor_y = 1;
            cursor_x = 11 + text_width(b->title, offset, b->cursors[0]);
        }
        char display[BBS_BODY_SIZE + 1];
        snprintf(display, sizeof(display), "%s ", b->body);
        int row = text_offset_row(display, w - 4, b->cursors[1]);
        if (b->focus == 1) {
            if (row < b->body_scroll)
                b->body_scroll = row;
            if (row >= b->body_scroll + visible)
                b->body_scroll = row - visible + 1;
            cursor_y = 2 + row - b->body_scroll;
            cursor_x = 2 + text_width(display, text_row_offset(display, w - 4, row), b->cursors[1]);
        }
        int body_offset = text_row_offset(display, w - 4, b->body_scroll);
        for (int i = 0; i < visible; i++) {
            body_offset = text_wrap_next_line(display, w - 4, body_offset, part, sizeof(part));
            mvwaddstr(win, 2 + i, 2, part);
        }
        int col = 2;
        button(b, win, h - 2, &col, "[Post]", UI_BOARD_POST, b->focus == 2);
        button(b, win, h - 2, &col, "[Cancel]", CANCEL, b->focus == 3);
    } else if (!b->thread_mode) {
        char label[128];
        snprintf(label, sizeof(label), "%u unread threads | page %u", m->board_unread,
                 b->offset / BBS_PAGE_SIZE + 1);
        mvwaddnstr(win, 1, 2, label, w - 4);
        if (b->cursor >= b->count)
            b->cursor = b->count ? b->count - 1 : 0;
        int first = b->cursor / visible * visible;
        for (int i = first, row = 2; i < b->count && row < h - 3; i++, row++) {
            bbs_thread_t *t = &b->threads[i];
            char text[256], part[256];
            snprintf(text, sizeof(text), "%c%c%c %u %s | %s", t->unread ? '*' : ' ',
                     t->pinned ? 'P' : ' ', t->locked ? 'L' : ' ', t->post_count, t->title,
                     t->author);
            text_wrap_line(text, w - 4, 0, part, sizeof(part));
            if (i == b->cursor)
                wattron(win, A_REVERSE);
            mvwaddstr(win, row, 2, part);
            wattroff(win, A_REVERSE);
        }
        if (!b->count)
            mvwaddnstr(win, 2, 2, "No threads here. N creates one.", w - 4);
        int col = 2;
        button(b, win, h - 3, &col, "[New]", NEW, false);
        button(b, win, h - 3, &col, "[Open]", UI_BOARD_THREAD, false);
        button(b, win, h - 3, &col, "[Author]", AUTHOR, false);
        button(b, win, h - 3, &col, "[Close]", CLOSE, false);
        col = 2;
        button(b, win, h - 2, &col, "[Prev]", UI_BOARD_PREV, false);
        button(b, win, h - 2, &col, "[Next]", UI_BOARD_NEXT, false);
        button(b, win, h - 2, &col, "[Refresh]", UI_BOARD_OPEN, false);
    } else {
        bbs_view_t *v = &b->view;
        char label[160];
        snprintf(label, sizeof(label), "Thread #%llu | %u posts%s%s",
                 (unsigned long long)v->thread.id, v->thread.post_count,
                 v->thread.pinned ? " | pinned" : "", v->thread.locked ? " | locked" : "");
        mvwaddnstr(win, 1, 2, label, w - 4);
        int total = text_wrap_rows(v->thread.title, w - 4), selected_start = total;
        for (int i = 0; i < v->count; i++) {
            if (i == b->cursor)
                selected_start = total;
            char header[128];
            post_header(&v->posts[i], header);
            total +=
                text_wrap_rows(header, w - 4) +
                text_wrap_rows(v->posts[i].deleted ? "[post removed]" : v->posts[i].body, w - 4) +
                1;
        }
        if (b->follow) {
            if (selected_start < b->scroll || selected_start >= b->scroll + visible)
                b->scroll = selected_start;
            b->follow = false;
        }
        int maximum = total > visible ? total - visible : 0;
        if (b->scroll > maximum)
            b->scroll = maximum;
        if (b->scroll < 0)
            b->scroll = 0;
        int logical = 0;
        line(win, v->thread.title, w - 4, b->scroll, visible, &logical, false);
        for (int i = 0; i < v->count; i++) {
            bbs_post_t *p = &v->posts[i];
            char header[128];
            post_header(p, header);
            b->post_rows[i][0] = 2 + logical - b->scroll;
            line(win, header, w - 4, b->scroll, visible, &logical, i == b->cursor);
            b->post_rows[i][1] = logical;
            line(win, p->deleted ? "[post removed]" : p->body, w - 4, b->scroll, visible, &logical,
                 false);
            logical++;
        }
        int col = 2;
        button(b, win, h - 3, &col, "[Reply]", REPLY, false);
        button(b, win, h - 3, &col, "[Author]", AUTHOR, false);
        button(b, win, h - 3, &col, "[Remove]", REMOVE, false);
        col = 2;
        button(b, win, h - 2, &col, "[Back]", UI_BOARD_LIST, false);
        button(b, win, h - 2, &col, "[Older]", UI_BOARD_PREV, false);
        button(b, win, h - 2, &col, "[Newer]", UI_BOARD_NEXT, false);
        button(b, win, h - 2, &col, "[Close]", CLOSE, false);
    }
    const char *message = b->discard          ? "Discard draft? Enter/Y yes; Esc/N keep"
                          : b->confirm_delete ? "Remove this post? Enter/Y yes; Esc/N cancel"
                          : b->pending        ? (b->saving ? "Saving..." : "Loading...")
                          : b->message[0]     ? b->message
                          : b->stale          ? "Board changed. R refreshes this view."
                          : b->composing      ? "Tab fields | Ctrl+S posts | Esc cancels"
                          : b->thread_mode    ? "A reply | U author | R refresh | P/L admin"
                                              : "Enter opens | N new | U author | R refresh";
    mvwaddnstr(win, h - 1, 1, message, w - 2);
    if (cursor_y >= 0)
        wmove(win, cursor_y, cursor_x);
    wnoutrefresh(win);
}
