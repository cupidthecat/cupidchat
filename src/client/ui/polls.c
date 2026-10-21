#include "client/text.h"
#include "client/ui.h"
#include <ncurses.h>
#include <stdio.h>

static void geometry(const ui_state_t *ui, int *h, int *w, int *y, int *x) {
    *h = ui->layout.rows - 2;
    if (*h > 14)
        *h = 14;
    *w = ui->layout.cols - 2;
    if (*w > 76)
        *w = 76;
    *y = (ui->layout.rows - *h) / 2;
    *x = (ui->layout.cols - *w) / 2;
}

int ui_poll_input(ui_state_t *ui, const cli_model_t *m, int key) {
    ui->dirty = true;
    if (key == 27) {
        ui->show_poll = false;
        return UI_NONE;
    }
    if (ui->layout.too_small)
        return key == 0x11 ? UI_QUIT : UI_NONE;
    const poll_info_t *p = &m->poll;
    if (key == 'r' || key == 'R')
        return UI_POLL_OPEN;
    if (key == KEY_PPAGE || key == KEY_NPAGE) {
        ui->poll_question_row += key == KEY_PPAGE ? -1 : 1;
        return UI_NONE;
    }
    if ((!p->id || !p->option_count) && key != KEY_MOUSE)
        return UI_NONE;
    if (ui->poll_cursor >= p->option_count)
        ui->poll_cursor = 0;
    if (key == KEY_DOWN || key == '\t')
        ui->poll_cursor = (ui->poll_cursor + 1) % p->option_count;
    else if (key == KEY_UP || key == KEY_BTAB)
        ui->poll_cursor = (ui->poll_cursor + p->option_count - 1) % p->option_count;
    else if (key >= '1' && key <= '0' + p->option_count)
        ui->poll_cursor = key - '1';
    else if (key == '\n' || key == KEY_ENTER)
        return UI_POLL_VOTE;
    else if (key == KEY_MOUSE) {
        MEVENT mouse;
        if (getmouse(&mouse) != OK || !(mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
            return UI_NONE;
        int h, w, y, x;
        geometry(ui, &h, &w, &y, &x);
        int row = mouse.y - y, col = mouse.x - x;
        if (col < 2 || col >= w - 2)
            return UI_NONE;
        int visible = h - 5, first = ui->poll_cursor / visible * visible;
        if (p->id && row >= 2 && row < h - 3 && first + row - 2 < p->option_count)
            ui->poll_cursor = first + row - 2;
        if (row == h - 2) {
            if (col < 8 && p->id)
                return UI_POLL_VOTE;
            if (col >= 9 && col < 18)
                return UI_POLL_OPEN;
            if (col >= 19 && col < 26)
                ui->show_poll = false;
        }
    }
    return UI_NONE;
}

void ui_redraw_poll(ui_state_t *ui, const cli_model_t *m) {
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    WINDOW *win = newwin(h, w, y, x);
    if (!win)
        return;
    box(win, 0, 0);
    mvwaddstr(win, 0, 2, " Server poll ");
    const poll_info_t *p = &m->poll;
    if (!p->id)
        mvwaddnstr(win, 1, 2, "No poll yet. Admins can create one with /admin poll", w - 4);
    else {
        char line[512];
        int question_rows = text_wrap_rows(p->question, w - 4);
        if (ui->poll_question_row >= question_rows)
            ui->poll_question_row = question_rows - 1;
        if (ui->poll_question_row < 0)
            ui->poll_question_row = 0;
        text_wrap_line(p->question, w - 4, ui->poll_question_row, line, sizeof(line));
        mvwaddstr(win, 1, 2, line);
        int visible = h - 5;
        if (ui->poll_cursor >= p->option_count)
            ui->poll_cursor = 0;
        int first = ui->poll_cursor / visible * visible;
        for (int i = first, row = 2; i < p->option_count && row < h - 3; i++, row++) {
            snprintf(line, sizeof(line), "%d. [%u] %s%s", i + 1, p->votes[i], p->options[i],
                     p->my_vote == (uint32_t)i + 1 ? " <- your vote" : "");
            char clipped[512];
            text_wrap_line(line, w - 4, 0, clipped, sizeof(clipped));
            if (i == ui->poll_cursor)
                wattron(win, A_REVERSE);
            mvwaddstr(win, row, 2, clipped);
            wattroff(win, A_REVERSE);
        }
        const char *state = p->closed          ? "Closed | final results"
                            : m->is_muted      ? "Muted accounts cannot vote"
                            : ui->poll_pending ? "Saving vote..."
                            : p->my_vote       ? "Vote saved; choose another option to change it"
                                               : "One vote per account; Enter submits";
        mvwaddnstr(win, h - 3, 2, state, w - 4);
    }
    mvwaddnstr(win, h - 2, 2, "[Vote] [Refresh] [Close]", w - 4);
    mvwaddnstr(win, h - 1, 1, "Arrows: choose | PgUp/Dn: question | Esc", w - 2);
    wnoutrefresh(win);
    delwin(win);
}
