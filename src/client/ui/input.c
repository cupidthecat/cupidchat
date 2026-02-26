/*
 * src/client/ui/input.c -  keyboard input, command history, sidebar nav
 *
 * Return values from ui_input_tick():
 *   ERR  -  no key available (non-blocking)
 *   0    -  key consumed, model unchanged
 *   1    -  Enter pressed with text: *out_line filled, ready to send/dispatch
 *   2    -  Page-Up pressed: caller should scroll chat up
 *   3    -  Page-Down pressed: caller should scroll chat down
 *   4    -  Nav-Enter: confirm nav_cursor selection (caller joins/switches)
 *
 * Sidebar navigation (rooms / DM list):
 *   When the input buffer is EMPTY:
 *     UP / DOWN      -  move nav_cursor through rooms then DMs
 *     Enter          -  confirm selection (return 4)
 *   Tab / Shift-Tab  -  always move nav_cursor (regardless of input content)
 *   Any printable    -  starts/continues typing; nav_cursor is unaffected
 */

#include <stdlib.h>
#include <string.h>
#include <ncurses.h>

#include "client/ui.h"

/* Command history */

static void hist_push(ui_state_t *ui, const char *line) {
    if (!line || !*line) return;
    if (ui->hist_count > 0 &&
        strcmp(ui->hist[ui->hist_count - 1], line) == 0) return;

    if (ui->hist_count < 64) {
        ui->hist[ui->hist_count++] = strdup(line);
    } else {
        free(ui->hist[0]);
        memmove(ui->hist, ui->hist + 1,
                (size_t)(ui->hist_count - 1) * sizeof(ui->hist[0]));
        ui->hist[ui->hist_count - 1] = strdup(line);
    }
    ui->hist_idx = -1;
}

static void input_clear(ui_state_t *ui) {
    memset(ui->input_buf, 0, sizeof(ui->input_buf));
    ui->input_len    = 0;
    ui->input_cursor = 0;
}

/* Main tick */

int ui_input_tick(ui_state_t *ui, char *out_line, int out_max) {
    int ch = getch();
    if (ch == ERR) return ERR;

    /* Any key dismisses the help modal */
    if (ui->show_help_modal) {
        ui->show_help_modal = false;
        ui->dirty = true;
        return 0;
    }

    switch (ch) {

        /* Enter */
        case '\n':
        case KEY_ENTER:
            if (ui->input_len == 0) {
                /* empty input: confirm sidebar nav selection */
                if (ui->nav_cursor >= 0) return 4;
                return 0;
            }
            {
                int copy = ui->input_len < out_max - 1
                         ? ui->input_len : out_max - 1;
                memcpy(out_line, ui->input_buf, (size_t)copy);
                out_line[copy] = '\0';
                hist_push(ui, out_line);
                input_clear(ui);
                ui->dirty = true;
                return 1;
            }

        /* Backspace / Delete */
        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (ui->input_cursor > 0) {
                int pos = ui->input_cursor - 1;
                memmove(ui->input_buf + pos,
                        ui->input_buf + pos + 1,
                        (size_t)(ui->input_len - pos));
                ui->input_len--;
                ui->input_cursor--;
                ui->dirty = true;
            }
            break;

        case KEY_DC:
            if (ui->input_cursor < ui->input_len) {
                memmove(ui->input_buf + ui->input_cursor,
                        ui->input_buf + ui->input_cursor + 1,
                        (size_t)(ui->input_len - ui->input_cursor));
                ui->input_len--;
                ui->dirty = true;
            }
            break;

        /* In-line cursor motion */
        case KEY_LEFT:
            if (ui->input_cursor > 0) { ui->input_cursor--; ui->dirty = true; }
            break;

        case KEY_RIGHT:
            if (ui->input_cursor < ui->input_len) { ui->input_cursor++; ui->dirty = true; }
            break;

        case KEY_HOME:
        case 0x01: /* Ctrl-A */
            ui->input_cursor = 0;
            ui->dirty = true;
            break;

        case KEY_END:
        case 0x05: /* Ctrl-E */
            ui->input_cursor = ui->input_len;
            ui->dirty = true;
            break;

        case 0x0B: /* Ctrl-K: kill to end of line */
            ui->input_buf[ui->input_cursor] = '\0';
            ui->input_len = ui->input_cursor;
            ui->dirty = true;
            break;

        /* Up/Down: history when typing, sidebar nav when empty */
        case KEY_UP:
            if (ui->input_len == 0) {
                if (ui->nav_cursor > 0) ui->nav_cursor--;
                ui->dirty = true;
            } else if (ui->hist_count > 0) {
                if (ui->hist_idx < 0) ui->hist_idx = ui->hist_count - 1;
                else if (ui->hist_idx > 0) ui->hist_idx--;
                const char *h = ui->hist[ui->hist_idx];
                int l = (int)strlen(h);
                memcpy(ui->input_buf, h, (size_t)l);
                ui->input_buf[l] = '\0';
                ui->input_len = ui->input_cursor = l;
                ui->dirty = true;
            }
            break;

        case KEY_DOWN:
            if (ui->input_len == 0) {
                /* upper bound is clamped in main loop after the call */
                ui->nav_cursor++;
                ui->dirty = true;
            } else if (ui->hist_idx >= 0) {
                ui->hist_idx++;
                if (ui->hist_idx >= ui->hist_count) {
                    ui->hist_idx = -1;
                    input_clear(ui);
                } else {
                    const char *h = ui->hist[ui->hist_idx];
                    int l = (int)strlen(h);
                    memcpy(ui->input_buf, h, (size_t)l);
                    ui->input_buf[l] = '\0';
                    ui->input_len = ui->input_cursor = l;
                }
                ui->dirty = true;
            }
            break;

        /* Tab / Shift-Tab: sidebar nav always */
        case '\t':
            ui->nav_cursor++;
            ui->dirty = true;
            break;

        case KEY_BTAB:  /* Shift-Tab */
            if (ui->nav_cursor > 0) ui->nav_cursor--;
            ui->dirty = true;
            break;

        /* Page Up/Down: scroll chat pane */
        case KEY_PPAGE:
            ui->dirty = true;
            return 2;

        case KEY_NPAGE:
            ui->dirty = true;
            return 3;

        /* Printable characters */
        default:
            if (ch >= 32 && ch < 256 &&
                ui->input_len < (int)sizeof(ui->input_buf) - 2) {
                memmove(ui->input_buf + ui->input_cursor + 1,
                        ui->input_buf + ui->input_cursor,
                        (size_t)(ui->input_len - ui->input_cursor + 1));
                ui->input_buf[ui->input_cursor] = (char)ch;
                ui->input_len++;
                ui->input_cursor++;
                ui->dirty = true;
            }
            break;
    }
    return 0;
}

/* Scroll */

void ui_scroll(ui_state_t *ui, cli_model_t *m, int delta) {
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        cli_room_t *r = &m->rooms[m->active_room];
        r->scroll_offset += delta;
        if (r->scroll_offset < 0) r->scroll_offset = 0;
        int max_scroll = r->msgs_len - 1;
        if (max_scroll < 0) max_scroll = 0;
        if (r->scroll_offset > max_scroll) r->scroll_offset = max_scroll;
    } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
        cli_dm_t *d = &m->dms[m->active_dm];
        d->scroll_offset += delta;
        if (d->scroll_offset < 0) d->scroll_offset = 0;
    }
    ui->dirty = true;
}

void ui_set_status(ui_state_t *ui, const char *msg) {
    (void)ui; (void)msg;
}
