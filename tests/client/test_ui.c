/* Client regressions run on an ncurses screen backed by temporary files. */
#include <assert.h>
#include <limits.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client/ui.h"
#include "client/history.h"

static void test_layout(void) {
    for (int rows = 1; rows <= 80; rows++) {
        for (int cols = 1; cols <= 200; cols++) {
            ui_layout_t l;
            ui_compute_layout(&l, rows, cols, true);
            assert(l.too_small == (rows < 8 || cols < 40));
            if (l.too_small) continue;
            assert(l.chat_h == rows - 4 && l.chat_h > 0);
            assert(l.chat_w >= 20);
            assert(l.rooms_w + l.chat_w + l.users_w == cols);
            assert(!l.rooms_w || l.rooms_w >= 14);
            assert(!l.users_w || l.users_w >= 14);
        }
    }
    puts("  PASS  panes fit narrow, short, and wide terminals");
}

static void test_iterator(void) {
    cli_msg_t messages[4] = {0};
    history_iter_t it;
    history_iter_init(&it, messages, 4, 3, 3, 1, 5);
    assert(history_iter_next(&it) == &messages[0]);
    assert(history_iter_next(&it) == &messages[1]);
    assert(!history_iter_next(&it));
    history_iter_init(&it, messages, 4, 3, 3, 999, 5);
    assert(!history_iter_next(&it));
    history_iter_init(&it, NULL, 0, 0, 0, 0, 0);
    assert(!history_iter_next(&it));
    puts("  PASS  history excludes skipped messages and handles empty buffers");
}

static cli_room_t *room(cli_model_t *m, const char *name) {
    cli_room_t *r = model_get_or_create_room(m, name);
    assert(r);
    r->joined = true;
    return r;
}

static void test_conversations(void) {
    cli_model_t m;
    model_init(&m);
    room(&m, "first"); room(&m, "second"); room(&m, "third");
    m.active_room = 0;
    m.rooms[1].joined = false;
    assert(model_remove_room(&m, "first"));
    assert(m.active_room == 1 && !strcmp(m.rooms[m.active_room].name, "third"));
    assert(!model_remove_room(&m, "missing"));
    model_get_or_create_dm(&m, 10, "alice");
    model_get_or_create_dm(&m, 11, "bob");
    m.active_room = -1; m.active_dm = 1;
    assert(!model_close_dm(&m, -1));
    assert(model_close_dm(&m, m.active_dm));
    assert(m.dm_count == 1 && !strcmp(m.dms[0].peer_nick, "alice"));
    assert(m.active_room == 1 && m.active_dm == -1);
    model_free(&m);
    puts("  PASS  closing a DM and deleting a room keep valid selections");
}

static void test_scrollback(void) {
    cli_model_t m;
    model_init(&m);
    cli_room_t *r = room(&m, "general");
    r->view_width = 40;
    m.active_room = 0;
    for (int i = 0; i < 30; i++)
        model_push_room_msg(&m, "general", 1, "alice", "", "hello", 1000, false, "me");
    r->scroll_offset = 8;
    model_push_room_msg(&m, "general", 1, "alice", "", "another", 1000, false, "me");
    assert(r->scroll_offset == 9);
    cli_dm_t *d = model_get_or_create_dm(&m, 1, "alice");
    assert(d); d->view_width = 40;
    model_push_dm_msg(&m, 1, "alice", "first", 1000, false);
    d->scroll_offset = 1;
    model_push_dm_msg(&m, 1, "alice", "next", 1000, false);
    assert(d->scroll_offset == 2);
    ui_state_t ui = {0};
    ui_compute_layout(&ui.layout, 12, 40, true);
    m.active_room = -1; m.active_dm = 0;
    ui_scroll(&ui, &m, INT_MAX);
    assert(d->scroll_offset == 0); /* the whole short conversation fits */
    m.active_room = 0; m.active_dm = -1;
    ui_scroll(&ui, &m, INT_MAX);
    int total = history_total_rows(r->msgs, r->msgs_cap, r->msgs_head, r->msgs_len, 40, &m);
    assert(r->scroll_offset == total - (ui.layout.chat_h - 1));
    ui_scroll(&ui, &m, INT_MIN);
    assert(!r->scroll_offset);
    for (int i = 0; i < CLI_HISTORY_CAP + 3; i++)
        model_push_room_msg(&m, "general", 1, "alice", "", "wrap", 1000, false, "me");
    assert(r->msgs_len == CLI_HISTORY_CAP);
    model_free(&m);
    puts("  PASS  room and DM scrollback stays anchored and bounded");
}

static void test_screen(void) {
    FILE *output = tmpfile(), *input = tmpfile();
    assert(output && input);
    SCREEN *screen = newterm("xterm", output, input);
    assert(screen);
    ui_state_t ui = {0};
    cli_model_t m;
    model_init(&m);
    ui.show_users = true;
    assert(resizeterm(24, 100) == OK);
    ui_resize(&ui);
    cli_room_t *r = room(&m, "general");
    m.active_room = 0;
    char text[CLI_MSG_MAX];
    memset(text, 'x', sizeof(text));
    memcpy(text + sizeof(text) - 5, "TAIL", 5);
    model_push_room_msg(&m, "general", 1, "alice", "", text, 1000, false, "me");
    ui_redraw(&ui, &m);
    char row[256];
    mvwinnstr((WINDOW *)ui.win_chat, ui.layout.chat_h - 2, 0, row, ui.layout.chat_w);
    assert(strstr(row, "TAIL"));
    ui_scroll(&ui, &m, 3);
    assert(r->scroll_offset == 3);
    model_push_room_msg(&m, "general", 1, "alice", "", "incoming", 1000, false, "me");
    assert(r->scroll_offset == 4);
    int old_total = history_total_rows(r->msgs, r->msgs_cap, r->msgs_head, r->msgs_len, r->view_width, &m);
    assert(old_total > r->scroll_offset);
    strcpy(ui.input_buf, "unfinished message");
    ui.input_len = ui.input_cursor = (int)strlen(ui.input_buf);
    assert(resizeterm(4, 12) == OK); ui_resize(&ui); ui_redraw(&ui, &m);
    assert(ui.layout.too_small && !ui.win_chat);
    assert(!strcmp(ui.input_buf, "unfinished message"));
    assert(resizeterm(16, 60) == OK); ui_resize(&ui); ui_redraw(&ui, &m);
    assert(!ui.layout.too_small && ui.win_chat);
    assert(r->scroll_offset > 0);
    assert(ui_click(&ui, &m, 2, 3) == UI_SELECT);
    assert(ui.nav_cursor == 0);
    assert(ui_click(&ui, &m, 1, ui.layout.rows - 2) == UI_CLOSE);
    assert(ui_click(&ui, &m, 17, ui.layout.rows - 2) == UI_LATEST);
    assert(ui_click(&ui, &m, 26, ui.layout.rows - 2) == UI_NONE && ui.confirm_quit);
    flushinp(); ungetch(27);
    char line[4096];
    assert(ui_input_tick(&ui, &m, line, sizeof(line)) == UI_NONE && !ui.confirm_quit);
    ungetch(KEY_F(10));
    assert(ui_input_tick(&ui, &m, line, sizeof(line)) == UI_NONE && ui.confirm_quit);
    ungetch('\n');
    assert(ui_input_tick(&ui, &m, line, sizeof(line)) == UI_QUIT);
    ui.confirm_quit = false;
    ui.show_help_modal = true; ui.help_offset = 999;
    ui_redraw(&ui, &m);
    assert(ui.help_offset < 999);
    ui.show_help_modal = false;
    memset(ui.input_buf, 'x', 300); ui.input_buf[300] = '\0';
    ui.input_len = ui.input_cursor = 300;
    ui_redraw(&ui, &m);
    assert(getcurx((WINDOW *)ui.win_input) < ui.layout.cols);
    ui_free(&ui); delscreen(screen);
    fclose(output); fclose(input);
    model_free(&m);
    puts("  PASS  wrapped messages, resize recovery, mouse controls, and quit dialog");
}

int main(void) {
    test_layout(); test_iterator(); test_conversations(); test_scrollback(); test_screen();
    puts("Client UI regressions passed");
    return 0;
}
