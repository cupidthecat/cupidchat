/*
 * src/client/ui/layout.c -  ncurses window creation and resize handling
 */

#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ncurses.h>

#include "client/ui.h"

#define ROOMS_W_MIN  14
#define ROOMS_W_PCT  15    /* % of terminal width */
#define USERS_W_MIN  14
#define USERS_W_PCT  14

static void compute_layout(ui_layout_t *l, bool show_users) {
    getmaxyx(stdscr, l->rows, l->cols);

    l->rooms_w = l->cols * ROOMS_W_PCT / 100;
    if (l->rooms_w < ROOMS_W_MIN) l->rooms_w = ROOMS_W_MIN;
    if (l->rooms_w > 24) l->rooms_w = 24;

    l->users_w = show_users ? (l->cols * USERS_W_PCT / 100) : 0;
    if (show_users && l->users_w < USERS_W_MIN) l->users_w = USERS_W_MIN;

    l->chat_w  = l->cols - l->rooms_w - l->users_w;
    if (l->chat_w < 20) l->chat_w = 20;
    l->chat_h  = l->rows - 3;   /* minus topbar, topic bar, and input */
}

static void destroy_windows(ui_state_t *ui) {
    if (ui->win_topbar) { delwin((WINDOW *)ui->win_topbar); ui->win_topbar = NULL; }
    if (ui->win_topic)  { delwin((WINDOW *)ui->win_topic);  ui->win_topic  = NULL; }
    if (ui->win_rooms)  { delwin((WINDOW *)ui->win_rooms);  ui->win_rooms  = NULL; }
    if (ui->win_chat)   { delwin((WINDOW *)ui->win_chat);   ui->win_chat   = NULL; }
    if (ui->win_users)  { delwin((WINDOW *)ui->win_users);  ui->win_users  = NULL; }
    if (ui->win_input)  { delwin((WINDOW *)ui->win_input);  ui->win_input  = NULL; }
}

static void create_windows(ui_state_t *ui) {
    const ui_layout_t *l = &ui->layout;

    /* topbar: row 0, full width */
    ui->win_topbar = newwin(1, l->cols, 0, 0);

    /* topic bar: row 1, full width */
    ui->win_topic  = newwin(1, l->cols, 1, 0);

    /* rooms pane: rows 2..chat_h+1, col 0..rooms_w */
    ui->win_rooms  = newwin(l->chat_h, l->rooms_w, 2, 0);

    /* chat pane: rows 2..chat_h+1, col rooms_w..rooms_w+chat_w */
    ui->win_chat   = newwin(l->chat_h, l->chat_w, 2, l->rooms_w);

    /* users pane (optional) */
    if (ui->show_users && l->users_w > 0)
        ui->win_users = newwin(l->chat_h, l->users_w,
                               2, l->rooms_w + l->chat_w);

    /* input line: last row */
    ui->win_input  = newwin(1, l->cols, l->rows - 1, 0);

    scrollok((WINDOW *)ui->win_chat, FALSE);  /* we handle scrolling manually */
}

int ui_init(ui_state_t *ui) {
    memset(ui, 0, sizeof(*ui));
    ui->hist_idx   = -1;
    ui->show_users = true;

    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN,    -1);   /* topbar */
        init_pair(2, COLOR_YELLOW,  -1);   /* room names */
        init_pair(3, COLOR_GREEN,   -1);   /* own nick */
        init_pair(4, COLOR_MAGENTA, -1);   /* peer nick */
        init_pair(5, COLOR_RED,     -1);   /* error / warn */
        init_pair(6, COLOR_WHITE,   -1);   /* normal text */
        init_pair(7, COLOR_BLUE,    -1);   /* border */
        init_pair(8, COLOR_YELLOW,  -1);   /* system events */
        /* nick palette -  pairs 9-15 (peer color cycling) */
        init_pair( 9, COLOR_CYAN,    -1);  /* nick palette 0 */
        init_pair(10, COLOR_YELLOW,  -1);  /* nick palette 1 */
        init_pair(11, COLOR_MAGENTA, -1);  /* nick palette 2 */
        init_pair(12, COLOR_RED,     -1);  /* nick palette 3 */
        init_pair(13, COLOR_BLUE,    -1);  /* nick palette 4 */
        init_pair(14, COLOR_WHITE,   -1);  /* nick palette 5 */
        init_pair(15, COLOR_GREEN,   -1);  /* nick palette 6 */
        init_pair(16, COLOR_YELLOW,  -1);  /* away tag */
    }

    compute_layout(&ui->layout, ui->show_users);
    create_windows(ui);
    ui->dirty = true;
    return 0;
}

void ui_free(ui_state_t *ui) {
    destroy_windows(ui);
    endwin();

    for (int i = 0; i < ui->hist_count; i++) {
        free(ui->hist[i]);
        ui->hist[i] = NULL;
    }
}

void ui_resize(ui_state_t *ui) {
    endwin();
    refresh();
    clear();
    destroy_windows(ui);
    compute_layout(&ui->layout, ui->show_users);
    create_windows(ui);
    ui->dirty = true;
}
