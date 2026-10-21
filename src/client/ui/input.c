/* Keyboard and mouse input. UI_* actions are handled by the main loop.
 * Editing a draft does not switch chats. Sidebar navigation selects a chat
 * only after Enter or a click; resize and modal keys keep the draft intact.
 */

#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

#include "client/history.h"
#include "client/text.h"
#include "client/ui.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>

void ui_clamp_navigation(ui_state_t *ui, const cli_model_t *m) {
    int total = m->room_count + m->dm_count;
    if (!total)
        ui->nav_cursor = -1;
    else {
        if (ui->nav_cursor < 0)
            ui->nav_cursor = 0;
        if (ui->nav_cursor >= total)
            ui->nav_cursor = total - 1;
    }
    int visible = ui->layout.chat_h - 1;
    if (visible < 1)
        visible = 1;
    if (ui->sidebar_offset > ui->nav_cursor)
        ui->sidebar_offset = ui->nav_cursor;
    if (ui->nav_cursor >= ui->sidebar_offset + visible)
        ui->sidebar_offset = ui->nav_cursor - visible + 1;
    if (ui->sidebar_offset < 0)
        ui->sidebar_offset = 0;
}

int ui_click(ui_state_t *ui, const cli_model_t *m, int x, int y) {
    if (x < 0 || y < 0 || x >= ui->layout.cols || y >= ui->layout.rows || ui->layout.too_small)
        return UI_NONE;
    for (int i = 0; i < ui->name_hit_count; i++)
        if (y == ui->name_hits[i].row && x >= ui->name_hits[i].left && x < ui->name_hits[i].right)
            return ui_profile_target(ui, ui->name_hits[i].uid, ui->name_hits[i].nick, NULL);
    if (y == 0)
        return ui_profile_target(ui, 0, NULL, m->base_nick);
    if (y == 1 && !m->banner[0] && m->active_dm >= 0 && m->active_dm < m->dm_count)
        return ui_profile_target(ui, m->dms[m->active_dm].peer_uid, m->dms[m->active_dm].peer_nick,
                                 NULL);
    if (ui->show_users && x >= ui->layout.cols - ui->layout.users_w && y >= 3 &&
        y < 2 + ui->layout.chat_h && m->active_room >= 0 &&
        !strcmp(m->users_room, m->rooms[m->active_room].name)) {
        int index = ui->users_offset + y - 3;
        if (index < m->user_count)
            return ui_profile_target(ui, m->users[index].user_id, m->users[index].nick, NULL);
    }
    if (y == ui->layout.rows - 2) {
        if (x >= 49 && x < 58 && ui->layout.cols >= 59)
            return ui_profile_target(ui, 0, NULL, m->base_nick);
        if (x < 7)
            return UI_CLOSE;
        if (x >= 8 && x < 14) {
            ui->show_help_modal = true;
            ui->help_offset = 0;
        }
        if (x >= 15 && x < 23)
            return UI_LATEST;
        if (x >= 24 && x < 30)
            ui->confirm_quit = true;
        if (x >= 31 && x < 38)
            ui->show_emoji_modal = true;
        if (x >= 39 && x < 48 && ui->layout.cols >= 49)
            return UI_BUDDY_OPEN;
    } else if (y >= 3 && y < 2 + ui->layout.chat_h && x < ui->layout.rooms_w - 1) {
        int index = ui->sidebar_offset + y - 3;
        if (index < m->room_count + m->dm_count) {
            ui->nav_cursor = index;
            return UI_SELECT;
        }
    } else if (y == ui->layout.rows - 1 && x >= 2) {
        ui->input_cursor = text_cell_offset(ui->input_buf, ui->input_offset, x - 2);
        if (ui->input_cursor > ui->input_len)
            ui->input_cursor = ui->input_len;
    }
    ui->dirty = true;
    return UI_NONE;
}

/* Command history */

static void hist_push(ui_state_t *ui, const char *line) {
    if (!line || !*line)
        return;
    if (!strncmp(line, "/login ", 7) || !strncmp(line, "/register ", 10)) {
        ui->hist_idx = -1;
        return;
    }
    if (ui->hist_count > 0 && strcmp(ui->hist[ui->hist_count - 1], line) == 0)
        return;

    if (ui->hist_count < 64) {
        ui->hist[ui->hist_count++] = strdup(line);
    } else {
        free(ui->hist[0]);
        memmove(ui->hist, ui->hist + 1, (size_t)(ui->hist_count - 1) * sizeof(ui->hist[0]));
        ui->hist[ui->hist_count - 1] = strdup(line);
    }
    ui->hist_idx = -1;
}

static void input_clear(ui_state_t *ui) {
    memset(ui->input_buf, 0, sizeof(ui->input_buf));
    ui->input_len = 0;
    ui->input_cursor = 0;
    ui->input_offset = 0;
}

static void insert_text(ui_state_t *ui, const char *text) {
    int length = (int)strlen(text);
    if (ui->input_len + length >= (int)sizeof(ui->input_buf))
        return;
    memmove(ui->input_buf + ui->input_cursor + length, ui->input_buf + ui->input_cursor,
            (size_t)(ui->input_len - ui->input_cursor + 1));
    memcpy(ui->input_buf + ui->input_cursor, text, (size_t)length);
    ui->input_len += length;
    ui->input_cursor += length;
    ui->dirty = true;
}

/* Complete only an unambiguous prefix; an ambiguous match extends to the
 * common prefix. Empty-input Tab keeps navigating the chat sidebar. */
static void complete_nick(ui_state_t *ui, const cli_model_t *m) {
    if (!strncmp(ui->input_buf, "/login ", 7) || !strncmp(ui->input_buf, "/register ", 10) ||
        (ui->input_buf[ui->input_cursor] && ui->input_buf[ui->input_cursor] != ' '))
        return;
    int start = ui->input_cursor;
    while (start > 0 && ui->input_buf[start - 1] != ' ')
        start--;
    int length = ui->input_cursor - start;
    if (!length)
        return;
    char match[CLI_NICK_MAX] = {0};
    int matches = 0, common = 0;
    bool room_matches =
        m->active_room >= 0 && !strcmp(m->rooms[m->active_room].name, m->users_room);
    int count = room_matches ? m->user_count : m->active_dm >= 0 ? 1 : 0;
    for (int i = 0; i < count; i++) {
        const char *nick = room_matches ? m->users[i].nick : m->dms[m->active_dm].peer_nick;
        if (strncasecmp(nick, ui->input_buf + start, (size_t)length))
            continue;
        if (!matches) {
            snprintf(match, sizeof(match), "%s", nick);
            common = (int)strlen(match);
        } else {
            int j = 0;
            while (j < common && nick[j] &&
                   tolower((unsigned char)match[j]) == tolower((unsigned char)nick[j]))
                j++;
            common = j;
        }
        matches++;
    }
    if (!matches || common < length)
        return;
    char suffix[CLI_NICK_MAX + 3];
    snprintf(suffix, sizeof(suffix), "%.*s%s", common, match,
             matches == 1 && (ui->input_buf[ui->input_cursor] == '\0') ? (start == 0 ? ": " : " ")
                                                                       : "");
    if (ui->input_len - length + (int)strlen(suffix) >= (int)sizeof(ui->input_buf))
        return;
    memmove(ui->input_buf + start, ui->input_buf + ui->input_cursor,
            (size_t)(ui->input_len - ui->input_cursor + 1));
    ui->input_len -= length;
    ui->input_cursor = start;
    insert_text(ui, suffix);
}

/* Main tick */

int ui_input_tick(ui_state_t *ui, const cli_model_t *m, char *out_line, int out_max) {
    wint_t key;
    int kind = get_wch(&key);
    if (kind == ERR)
        return ERR;
    /* Unicode values can equal ncurses key codes. Keep the two namespaces apart. */
    int ch = kind == KEY_CODE_YES ? (int)key : key < 128 ? (int)key : 0;
    if (ch == 0x03)
        return UI_QUIT;

    if (ch == KEY_RESIZE || ch == 0x0c)
        return UI_RESIZE;
    if (ui->show_auth) {
        if (ui->layout.too_small)
            return ch == 0x11 ? UI_QUIT : UI_NONE;
        return ui_auth_input(ui, ch, key, kind == OK);
    }
    if (ui->show_poll)
        return ui_poll_input(ui, m, ch);
    if (ui->show_profile)
        return ui_profile_input(ui, m, ch, key, kind == OK);
    if (ui->confirm_quit) {
        if (ch == '\n' || ch == KEY_ENTER)
            return UI_QUIT;
        if (ch == 27 || ch == 'n' || ch == 'N')
            ui->confirm_quit = false;
        ui->dirty = true;
        return UI_NONE;
    }

    if (ui->show_emoji_modal) {
        if (ch == 27)
            ui->show_emoji_modal = false;
        else if (ch == KEY_DOWN || ch == KEY_RIGHT || ch == '\t')
            ui->emoji_cursor = (ui->emoji_cursor + 1) % EMOJI_COUNT;
        else if (ch == KEY_UP || ch == KEY_LEFT)
            ui->emoji_cursor = (ui->emoji_cursor + EMOJI_COUNT - 1) % EMOJI_COUNT;
        else if (ch == '\n' || ch == KEY_ENTER) {
            insert_text(ui, EMOJIS[ui->emoji_cursor].glyph);
            ui->show_emoji_modal = false;
        }
        ui->dirty = true;
        return UI_NONE;
    }

    if (ui->show_buddies_modal) {
        if (m->buddy_count == 0)
            ui->buddy_cursor = 0;
        else if (ui->buddy_cursor >= m->buddy_count)
            ui->buddy_cursor = m->buddy_count - 1;
        if (ch == 'p' || ch == 'P') {
            if (m->buddy_count)
                return ui_profile_target(ui, 0, NULL, m->buddies[ui->buddy_cursor].account);
        } else if (ch == KEY_MOUSE) {
            MEVENT mouse;
            if (getmouse(&mouse) == OK && (mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED))) {
                int width = ui->layout.cols - 2;
                if (width > 64)
                    width = 64;
                int height = ui->layout.rows - 2;
                if (height > m->buddy_count + 5)
                    height = m->buddy_count + 5;
                if (height < 6)
                    height = 6;
                int visible = height - 4;
                int first = ui->buddy_cursor / visible * visible;
                int row = mouse.y - (ui->layout.rows - height) / 2 - 1;
                if (mouse.x >= (ui->layout.cols - width) / 2 + 1 &&
                    mouse.x < (ui->layout.cols + width) / 2 - 1 && row >= 0 && row < visible &&
                    first + row < m->buddy_count) {
                    ui->buddy_cursor = first + row;
                    return ui_profile_target(ui, 0, NULL, m->buddies[ui->buddy_cursor].account);
                }
            }
        } else if (ch == 27)
            ui->show_buddies_modal = false;
        else if ((ch == KEY_DOWN || ch == '\t') && m->buddy_count)
            ui->buddy_cursor = (ui->buddy_cursor + 1) % m->buddy_count;
        else if (ch == KEY_UP && m->buddy_count)
            ui->buddy_cursor = (ui->buddy_cursor + m->buddy_count - 1) % m->buddy_count;
        else if (ch == 'r' || ch == 'R')
            return UI_BUDDY_OPEN;
        else if ((ch == '\n' || ch == KEY_ENTER) && m->buddy_count)
            return UI_BUDDY_SELECT;
        ui->dirty = true;
        return UI_NONE;
    }

    /* Any key dismisses the help modal */
    if (ui->show_help_modal) {
        if (ch == KEY_DOWN || ch == KEY_NPAGE)
            ui->help_offset++;
        else if (ch == KEY_UP || ch == KEY_PPAGE) {
            if (ui->help_offset > 0)
                ui->help_offset--;
        } else
            ui->show_help_modal = false;
        ui->dirty = true;
        return 0;
    }

    if (ch == KEY_F(10) || ch == 0x11) {
        ui->confirm_quit = ui->dirty = true;
        return UI_NONE;
    }
    if (ui->layout.too_small)
        return UI_NONE;
    if (ch == KEY_F(1)) {
        ui->show_help_modal = ui->dirty = true;
        ui->help_offset = 0;
        return UI_NONE;
    }
    if (ch == KEY_F(2)) {
        ui->show_emoji_modal = ui->dirty = true;
        return UI_NONE;
    }
    if (ch == KEY_F(3) || ch == 0x17)
        return UI_CLOSE;
    if (ch == KEY_F(4))
        return UI_LATEST;
    if (ch == KEY_F(5))
        return UI_NEXT_UNREAD;
    if (ch == KEY_F(8))
        return UI_POLL_OPEN;
    if (ch == KEY_F(7))
        return ui_profile_target(ui, 0, NULL, m->base_nick);
    if (ch == KEY_F(6))
        return UI_BUDDY_OPEN;
    if (ch == KEY_MOUSE) {
        MEVENT event;
        if (getmouse(&event) != OK)
            return UI_NONE;
        if (ui->show_users && event.x >= ui->layout.cols - ui->layout.users_w && event.y >= 3 &&
            event.y < ui->layout.rows - 2) {
            if (event.bstate & BUTTON4_PRESSED)
                ui->users_offset -= 3;
#ifdef BUTTON5_PRESSED
            if (event.bstate & BUTTON5_PRESSED)
                ui->users_offset += 3;
#endif
            ui->dirty = true;
        }
        if (event.x >= 0 && event.x < ui->layout.cols && event.y >= 2 &&
            event.y < 2 + ui->layout.chat_h && event.x >= ui->layout.rooms_w &&
            event.x < ui->layout.rooms_w + ui->layout.chat_w) {
            ui->scroll_delta = 0;
            if (event.bstate & BUTTON4_PRESSED)
                ui->scroll_delta = 3;
#ifdef BUTTON5_PRESSED
            if (event.bstate & BUTTON5_PRESSED)
                ui->scroll_delta = -3;
#endif
            if (ui->scroll_delta)
                return UI_SCROLL;
        }
        if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED))
            return ui_click(ui, m, event.x, event.y);
        return UI_NONE;
    }

    switch (ch) {

        /* Enter */
        case '\n':
        case KEY_ENTER:
            if (ui->input_len == 0) {
                /* empty input: confirm sidebar nav selection */
                if (ui->nav_cursor >= 0)
                    return 4;
                return 0;
            }
            {
                if (out_max <= ui->input_len)
                    return UI_NONE;
                int copy = ui->input_len;
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
                int pos = text_prev(ui->input_buf, ui->input_cursor);
                memmove(ui->input_buf + pos, ui->input_buf + ui->input_cursor,
                        (size_t)(ui->input_len - ui->input_cursor + 1));
                ui->input_len -= ui->input_cursor - pos;
                ui->input_cursor = pos;
                ui->dirty = true;
            }
            break;

        case KEY_DC:
            if (ui->input_cursor < ui->input_len) {
                int end = text_next(ui->input_buf, ui->input_cursor);
                memmove(ui->input_buf + ui->input_cursor, ui->input_buf + end,
                        (size_t)(ui->input_len - end + 1));
                ui->input_len -= end - ui->input_cursor;
                ui->dirty = true;
            }
            break;

        /* In-line cursor motion */
        case KEY_LEFT:
            if (ui->input_cursor > 0) {
                ui->input_cursor = text_prev(ui->input_buf, ui->input_cursor);
                ui->dirty = true;
            }
            break;

        case KEY_RIGHT:
            if (ui->input_cursor < ui->input_len) {
                ui->input_cursor = text_next(ui->input_buf, ui->input_cursor);
                ui->dirty = true;
            }
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
                if (ui->nav_cursor > 0)
                    ui->nav_cursor--;
                ui->dirty = true;
            } else if (ui->hist_count > 0) {
                if (ui->hist_idx < 0)
                    ui->hist_idx = ui->hist_count - 1;
                else if (ui->hist_idx > 0)
                    ui->hist_idx--;
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
            if (ui->input_len)
                complete_nick(ui, m);
            else
                ui->nav_cursor++;
            ui->dirty = true;
            break;

        case KEY_BTAB: /* Shift-Tab */
            if (ui->nav_cursor > 0)
                ui->nav_cursor--;
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
            if (kind == OK && key >= 32 && iswprint(key)) {
                char bytes[MB_LEN_MAX + 1];
                mbstate_t state = {0};
                size_t length = wcrtomb(bytes, (wchar_t)key, &state);
                if (length != (size_t)-1) {
                    bytes[length] = '\0';
                    insert_text(ui, bytes);
                }
            }
            break;
    }
    return 0;
}

/* Scroll */

void ui_scroll(ui_state_t *ui, cli_model_t *m, int delta) {
    const cli_msg_t *buf = NULL;
    int cap = 0, head = 0, len = 0, *offset = NULL;
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        cli_room_t *r = &m->rooms[m->active_room];
        buf = r->msgs;
        cap = r->msgs_cap;
        head = r->msgs_head;
        len = r->msgs_len;
        offset = &r->scroll_offset;
    } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
        cli_dm_t *d = &m->dms[m->active_dm];
        buf = d->msgs;
        cap = d->msgs_cap;
        head = d->msgs_head;
        len = d->msgs_len;
        offset = &d->scroll_offset;
    }
    if (offset) {
        int maximum =
            history_total_rows(buf, cap, head, len, ui->layout.chat_w, m) - (ui->layout.chat_h - 1);
        if (maximum < 0)
            maximum = 0;
        long next = (long)*offset + delta;
        *offset = next < 0 ? 0 : next > maximum ? maximum : (int)next;
    }
    ui->dirty = true;
}

void ui_set_status(ui_state_t *ui, const char *msg) {
    (void)ui;
    (void)msg;
}
