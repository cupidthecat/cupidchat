#include "client/forms.h"
#include "client/text.h"
#include "client/ui.h"
#include <ncurses.h>
#include <stdio.h>
#include <string.h>

static const char *ACTIONS[] = {"Invite",
                                "Kick",
                                "Ban",
                                "Unban",
                                "Grant operator",
                                "Remove operator",
                                "Toggle topic lock",
                                "Decline invitation",
                                "Refresh info",
                                "Join room"};

static bool allowed(const room_control_state_t *r, const cli_model_t *m, int action) {
    uint8_t role = r->info.role;
    if (action == 0)
        return (role & ROOM_MANAGERS) && !m->is_muted;
    if (action >= 1 && action <= 3)
        return (role & ROOM_MANAGERS) != 0;
    if (action >= 4 && action <= 6)
        return (role & (ROOM_OWNER | ROOM_ADMIN)) != 0;
    if (action == 7)
        return (role & ROOM_INVITED) != 0;
    if (action == 9) {
        if ((role & ROOM_BANNED) && !(role & (ROOM_OWNER | ROOM_ADMIN)))
            return false;
        for (int i = 0; i < m->room_count; i++)
            if (!strcmp(m->rooms[i].name, r->info.room))
                return !m->rooms[i].joined;
        return true;
    }
    return action == 8;
}

static void choose(room_control_state_t *r, const cli_model_t *m, int delta) {
    do {
        r->action = (r->action + delta + 10) % 10;
    } while (!allowed(r, m, r->action));
}

static void geometry(const ui_state_t *ui, int *h, int *w, int *y, int *x) {
    *h = ui->layout.rows - 2;
    if (*h > 22)
        *h = 22;
    *w = ui->layout.cols - 2;
    if (*w > 86)
        *w = 86;
    *y = (ui->layout.rows - *h) / 2;
    *x = (ui->layout.cols - *w) / 2;
}

static int profile(ui_state_t *ui) {
    room_control_state_t *r = &ui->room_control;
    const char *account = r->target[0] ? r->target : r->info.owner;
    if (!*account) {
        snprintf(r->message, sizeof(r->message), "Choose an account first");
        return UI_NONE;
    }
    return ui_profile_target(ui, 0, NULL, account);
}

int ui_room_control_input(ui_state_t *ui, const cli_model_t *m, int key, uint32_t character,
                          bool printable) {
    room_control_state_t *r = &ui->room_control;
    ui->modal_dirty = true;
    if (!allowed(r, m, r->action))
        r->action = 8;
    if (ui->layout.too_small)
        return UI_NONE;
    if (key == 27) {
        r->visible = false;
        return UI_NONE;
    }
    if (r->pending)
        return UI_NONE;
    if (key == KEY_F(7))
        return profile(ui);
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    int field_row = h >= 10 ? 3 : 2;
    if (key == KEY_MOUSE) {
        MEVENT mouse;
        if (getmouse(&mouse) != OK)
            return UI_NONE;
        int row = mouse.y - y, col = mouse.x - x;
        if (row < 1 || row >= h - 1 || col < 1 || col >= w - 1)
            return UI_NONE;
        if (mouse.bstate & BUTTON4_PRESSED)
            r->scroll -= 3;
#ifdef BUTTON5_PRESSED
        if (mouse.bstate & BUTTON5_PRESSED)
            r->scroll += 3;
#endif
        if (!(mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
            return UI_NONE;
        if (row == h - 2) {
            if (col >= 2 && col < 7)
                return UI_ROOM_RUN;
            if (col >= 8 && col < 17)
                return UI_ROOM_REFRESH;
            if (col >= 18 && col < 27)
                return profile(ui);
            if (col >= 28 && col < 35)
                r->visible = false;
        } else if (row == h - 3) {
            r->focus = 2;
            choose(r, m, col < w / 2 ? -1 : 1);
        } else if (row == field_row)
            r->focus = h >= 10 ? 0 : r->focus == 1 ? 1 : 0;
        else if (h >= 10 && row == 4)
            r->focus = 1;
        else if (h >= 12 && row >= 6 && row < h - 3) {
            int i = r->scroll + row - 6;
            if (i >= 0 && i < r->info.count) {
                snprintf(r->target, sizeof(r->target), "%s", r->info.entries[i].account);
                r->cursors[0] = (int)strlen(r->target);
                snprintf(r->reason, sizeof(r->reason), "%s", r->info.entries[i].reason);
                r->cursors[1] = (int)strlen(r->reason);
                return profile(ui);
            }
        }
        return UI_NONE;
    }
    if (key == '\t' || key == KEY_DOWN)
        r->focus = (r->focus + 1) % 7;
    else if (key == KEY_BTAB || key == KEY_UP)
        r->focus = (r->focus + 6) % 7;
    else if (key == KEY_PPAGE || key == KEY_NPAGE)
        r->scroll += key == KEY_PPAGE ? -3 : 3;
    else if (key == '\n' || key == KEY_ENTER) {
        if (r->focus < 2)
            r->focus++;
        else if (r->focus <= 3)
            return UI_ROOM_RUN;
        else if (r->focus == 4)
            return UI_ROOM_REFRESH;
        else if (r->focus == 5)
            return profile(ui);
        else
            r->visible = false;
    } else if (r->focus == 2 && (key == KEY_LEFT || key == KEY_RIGHT))
        choose(r, m, key == KEY_LEFT ? -1 : 1);
    else if (r->focus < 2) {
        int i = r->focus;
        if (!i && printable && character >= 32 &&
            !(character == '_' || character == '-' || (character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')))
            return UI_NONE;
        if (!form_edit(i ? r->reason : r->target, i ? sizeof(r->reason) : sizeof(r->target),
                       &r->cursors[i], key, character, printable) &&
            printable && character >= 32)
            snprintf(r->message, sizeof(r->message), "Field limit: %d bytes", i ? 256 : 32);
        else
            r->message[0] = '\0';
    }
    return UI_NONE;
}

void ui_redraw_room_control(ui_state_t *ui, const cli_model_t *m) {
    room_control_state_t *r = &ui->room_control;
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    WINDOW *win = ui_modal_window(ui, UI_MODAL_ROOM, h, w, y, x);
    if (!win)
        return;
    if (!allowed(r, m, r->action))
        r->action = 8;
    box(win, 0, 0);
    mvwaddstr(win, 0, 2, " Room controls ");
    char text[256], part[256];
    snprintf(text, sizeof(text), "#%s | owner: %s", r->info.room,
             r->info.owner[0] ? r->info.owner : "unassigned");
    text_wrap_line(text, w - 4, 0, part, sizeof(part));
    mvwaddstr(win, 1, 2, part);
    const char *role = r->info.role & ROOM_OWNER      ? "owner"
                       : r->info.role & ROOM_ADMIN    ? "admin"
                       : r->info.role & ROOM_OPERATOR ? "operator"
                       : r->info.role & ROOM_BANNED   ? "banned"
                       : r->info.role & ROOM_INVITED  ? "invited"
                                                      : "member";
    if (h >= 10) {
        snprintf(text, sizeof(text), "Your role: %s | topics: %s", role,
                 r->info.topic_locked ? "operators only" : "all members");
        mvwaddnstr(win, 2, 2, text, w - 4);
    }
    int cursor_y = -1, cursor_x = 0, first = h >= 10 ? 0 : r->focus == 1 ? 1 : 0;
    for (int i = first, row = h >= 10 ? 3 : 2; i < 2 && row < h - 3; i++, row++) {
        if (h < 10 && i != first)
            break;
        const char *value = i ? r->reason : r->target;
        int cursor = r->cursors[i], visible = w - 14;
        int offset = text_view_offset(value, 0, cursor, visible);
        int end = text_cell_offset(value, offset, visible);
        mvwaddstr(win, row, 2, i ? "Reason" : "Account");
        if (r->focus == i)
            wattron(win, A_REVERSE);
        mvwhline(win, row, 12, ' ', visible);
        mvwaddnstr(win, row, 12, value + offset, end - offset);
        wattroff(win, A_REVERSE);
        if (r->focus == i) {
            cursor_y = row;
            cursor_x = 12 + text_width(value, offset, cursor);
        }
    }
    if (h >= 12) {
        int visible = h - 9, maximum = r->info.count > visible ? r->info.count - visible : 0;
        if (r->scroll > maximum)
            r->scroll = maximum;
        if (r->scroll < 0)
            r->scroll = 0;
        mvwaddnstr(win, 5, 2,
                   r->info.more ? "Access list (first 64; click a name)"
                                : "Access list (click a name for profile)",
                   w - 4);
        for (int i = r->scroll, row = 6; i < r->info.count && row < h - 3; i++, row++) {
            room_access_t *a = &r->info.entries[i];
            snprintf(text, sizeof(text), "%s%s%s%s", a->account,
                     a->flags & ROOM_OPERATOR ? " [op]" : "",
                     a->flags & ROOM_BANNED ? " [banned]" : "",
                     a->flags & ROOM_INVITED ? " [invited]" : "");
            mvwaddnstr(win, row, 2, text, w - 4);
        }
    }
    snprintf(text, sizeof(text), "< %s >", ACTIONS[r->action]);
    if (r->focus == 2)
        wattron(win, A_REVERSE);
    mvwaddnstr(win, h - 3, 2, text, w - 4);
    wattroff(win, A_REVERSE);
    const char *buttons[] = {"[Run]", "[Refresh]", "[Profile]", "[Close]"};
    int col = 2;
    for (int i = 0; i < 4; i++) {
        if (r->focus == 3 + i)
            wattron(win, A_REVERSE);
        mvwaddstr(win, h - 2, col, buttons[i]);
        wattroff(win, A_REVERSE);
        col += (int)strlen(buttons[i]) + 1;
    }
    const char *help = r->pending      ? "Waiting for server..."
                       : r->message[0] ? r->message
                                       : "Tab fields | Arrows choose action | Enter";
    mvwaddnstr(win, h - 1, 1, help, w - 2);
    if (cursor_y >= 0)
        wmove(win, cursor_y, cursor_x);
    wnoutrefresh(win);
}
