/*
 * src/client/ui/layout.c -  ncurses window creation and resize handling
 */

#include <locale.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "client/ui.h"

#define ROOMS_W_MIN 14
#define ROOMS_W_PCT 15 /* % of terminal width */
#define USERS_W_MIN 14
#define USERS_W_PCT 14

void ui_compute_layout(ui_layout_t *l, int rows, int cols, bool show_users) {
    memset(l, 0, sizeof(*l));
    l->rows = rows;
    l->cols = cols;
    l->too_small = rows < 8 || cols < 40;
    if (l->too_small)
        return;

    l->rooms_w = l->cols * ROOMS_W_PCT / 100;
    if (l->rooms_w < ROOMS_W_MIN)
        l->rooms_w = ROOMS_W_MIN;
    if (l->rooms_w > 24)
        l->rooms_w = 24;
    if (cols < 60)
        l->rooms_w = 0;

    l->users_w = show_users ? (l->cols * USERS_W_PCT / 100) : 0;
    if (show_users && l->users_w < USERS_W_MIN)
        l->users_w = USERS_W_MIN;
    if (cols < 100)
        l->users_w = 0;

    l->chat_w = l->cols - l->rooms_w - l->users_w;
    l->chat_h = rows - 4;
}

static void destroy_windows(ui_state_t *ui) {
    if (ui->win_topbar) {
        delwin((WINDOW *)ui->win_topbar);
        ui->win_topbar = NULL;
    }
    if (ui->win_topic) {
        delwin((WINDOW *)ui->win_topic);
        ui->win_topic = NULL;
    }
    if (ui->win_rooms) {
        delwin((WINDOW *)ui->win_rooms);
        ui->win_rooms = NULL;
    }
    if (ui->win_chat) {
        delwin((WINDOW *)ui->win_chat);
        ui->win_chat = NULL;
    }
    if (ui->win_users) {
        delwin((WINDOW *)ui->win_users);
        ui->win_users = NULL;
    }
    if (ui->win_input) {
        delwin((WINDOW *)ui->win_input);
        ui->win_input = NULL;
    }
}

static void create_windows(ui_state_t *ui) {
    const ui_layout_t *l = &ui->layout;
    if (l->too_small)
        return;

    /* topbar: row 0, full width */
    ui->win_topbar = newwin(1, l->cols, 0, 0);

    /* topic bar: row 1, full width */
    ui->win_topic = newwin(1, l->cols, 1, 0);

    /* rooms pane: rows 2..chat_h+1, col 0..rooms_w */
    if (l->rooms_w > 0)
        ui->win_rooms = newwin(l->chat_h, l->rooms_w, 2, 0);

    /* chat pane: rows 2..chat_h+1, col rooms_w..rooms_w+chat_w */
    ui->win_chat = newwin(l->chat_h, l->chat_w, 2, l->rooms_w);

    /* users pane (optional) */
    if (ui->show_users && l->users_w > 0)
        ui->win_users = newwin(l->chat_h, l->users_w, 2, l->rooms_w + l->chat_w);

    /* input line: last row */
    ui->win_input = newwin(1, l->cols, l->rows - 1, 0);

    if (!ui->win_topbar || !ui->win_topic || !ui->win_chat || !ui->win_input ||
        (l->rooms_w && !ui->win_rooms) || (l->users_w && !ui->win_users)) {
        destroy_windows(ui);
        ui->layout.too_small = true;
        return;
    }
    scrollok((WINDOW *)ui->win_chat, FALSE);
}

int ui_init(ui_state_t *ui) {
    memset(ui, 0, sizeof(*ui));
    ui->hist_idx = -1;
    ui->show_users = true;
    ui->nav_cursor = -1;

    setlocale(LC_ALL, "");
    initscr();
    raw(); /* Ctrl+Q must reach the client rather than terminal flow control. */
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);
    mousemask(BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON4_PRESSED
#ifdef BUTTON5_PRESSED
                  | BUTTON5_PRESSED
#endif
              ,
              NULL);
    mouseinterval(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);    /* topbar */
        init_pair(2, COLOR_YELLOW, -1);  /* room names */
        init_pair(3, COLOR_GREEN, -1);   /* own nick */
        init_pair(4, COLOR_MAGENTA, -1); /* peer nick */
        init_pair(5, COLOR_RED, -1);     /* error / warn */
        init_pair(6, COLOR_WHITE, -1);   /* normal text */
        init_pair(7, COLOR_BLUE, -1);    /* border */
        init_pair(8, COLOR_YELLOW, -1);  /* system events */
        /* nick palette -  pairs 9-15 (peer color cycling) */
        init_pair(9, COLOR_CYAN, -1);     /* nick palette 0 */
        init_pair(10, COLOR_YELLOW, -1);  /* nick palette 1 */
        init_pair(11, COLOR_MAGENTA, -1); /* nick palette 2 */
        init_pair(12, COLOR_RED, -1);     /* nick palette 3 */
        init_pair(13, COLOR_BLUE, -1);    /* nick palette 4 */
        init_pair(14, COLOR_WHITE, -1);   /* nick palette 5 */
        init_pair(15, COLOR_GREEN, -1);   /* nick palette 6 */
        init_pair(16, COLOR_YELLOW, -1);  /* away tag */
    }

    ui_compute_layout(&ui->layout, getmaxy(stdscr), getmaxx(stdscr), ui->show_users);
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
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col &&
        (size.ws_row != getmaxy(stdscr) || size.ws_col != getmaxx(stdscr)))
        resizeterm(size.ws_row, size.ws_col);
    destroy_windows(ui);
    erase();
    clearok(stdscr, TRUE);
    ui_compute_layout(&ui->layout, getmaxy(stdscr), getmaxx(stdscr), ui->show_users);
    create_windows(ui);
    ui->dirty = true;
}
