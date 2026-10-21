#include "client/forms.h"
#include "client/text.h"
#include "client/ui.h"
#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

int ui_profile_target(ui_state_t *ui, uint32_t uid, const char *nick, const char *account) {
    ui->profile_target_uid = uid;
    snprintf(ui->profile_target_nick, sizeof(ui->profile_target_nick), "%s", nick ? nick : "");
    snprintf(ui->profile_target_account, sizeof(ui->profile_target_account), "%s",
             account ? account : "");
    return UI_PROFILE_OPEN;
}

static void geometry(const ui_state_t *ui, int *h, int *w, int *y, int *x) {
    *h = ui->layout.rows - 2;
    if (*h > 23)
        *h = 23;
    *w = ui->layout.cols - 2;
    if (*w > 80)
        *w = 80;
    *y = (ui->layout.rows - *h) / 2;
    *x = (ui->layout.cols - *w) / 2;
}

static void finish_edit(ui_state_t *ui) {
    ui->profile_editing = ui->profile_discard = false;
    explicit_bzero(&ui->profile_draft, sizeof(ui->profile_draft));
    ui->profile_error[0] = '\0';
    ui->profile_focus = ui->profile_offset = 0;
}

static int activate(ui_state_t *ui, int action) {
    if (action == 0) {
        ui->profile_editing = true;
        ui->profile_draft = ui->profile.fields;
        ui->profile_focus = ui->profile_offset = 0;
        for (int i = 0; i < PROFILE_FIELD_COUNT; i++)
            ui->profile_cursors[i] = (int)strlen(profile_field(&ui->profile_draft, i));
    } else if (action == 1)
        return UI_PROFILE_MESSAGE;
    else if (action == 2)
        return UI_PROFILE_BUDDY;
    else if (action == 3)
        return UI_PROFILE_REFRESH;
    else if (action == 4)
        ui->show_profile = false;
    else if (action == 5)
        return UI_PROFILE_SAVE;
    else if (action == 6) {
        if (memcmp(&ui->profile_draft, &ui->profile.fields, sizeof(ui->profile_draft)))
            ui->profile_discard = true;
        else
            finish_edit(ui);
    }
    return UI_NONE;
}

int ui_profile_input(ui_state_t *ui, const cli_model_t *m, int key, uint32_t character,
                     bool printable) {
    (void)m;
    ui->dirty = true;
    if (ui->layout.too_small)
        return key == 0x11 ? UI_QUIT : UI_NONE;
    if (ui->profile_discard) {
        if (key == 'y' || key == 'Y' || key == '\n' || key == KEY_ENTER)
            finish_edit(ui);
        else if (key == 27 || key == 'n' || key == 'N')
            ui->profile_discard = false;
        return UI_NONE;
    }
    if (ui->profile_saving)
        return UI_NONE;
    if (key == 27)
        return activate(ui, ui->profile_editing ? 6 : 4);
    if (ui->profile_loading)
        return UI_NONE;
    if (!ui->profile.account[0])
        return key == 'r' || key == 'R' ? UI_PROFILE_OPEN : UI_NONE;
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    if (key == KEY_MOUSE) {
        MEVENT mouse;
        if (getmouse(&mouse) != OK)
            return UI_NONE;
        if (!ui->profile_editing) {
            if (mouse.bstate & BUTTON4_PRESSED)
                ui->profile_offset -= 3;
#ifdef BUTTON5_PRESSED
            if (mouse.bstate & BUTTON5_PRESSED)
                ui->profile_offset += 3;
#endif
        }
        if (!(mouse.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)))
            return UI_NONE;
        int row = mouse.y - y, col = mouse.x - x;
        for (int i = 0; i < ui->profile_button_count; i++) {
            int *b = ui->profile_buttons[i];
            if (row == b[0] && col >= b[1] && col < b[2])
                return activate(ui, b[3]);
        }
        int first =
            ui->profile_focus < PROFILE_FIELD_COUNT ? ui->profile_focus : PROFILE_FIELD_COUNT - 1;
        if (h - 5 >= PROFILE_FIELD_COUNT)
            first = 0;
        if (ui->profile_editing && row >= 2 && row < h - 3 && col >= 2 && col < w - 2 &&
            first + row - 2 < PROFILE_FIELD_COUNT) {
            ui->profile_focus = first + row - 2;
        }
        return UI_NONE;
    }
    if (ui->profile_editing) {
        int total = PROFILE_FIELD_COUNT + 2;
        if (key == '\t' || key == KEY_DOWN)
            ui->profile_focus = (ui->profile_focus + 1) % total;
        else if (key == KEY_BTAB || key == KEY_UP)
            ui->profile_focus = (ui->profile_focus + total - 1) % total;
        else if (key == '\n' || key == KEY_ENTER) {
            if (ui->profile_focus < PROFILE_FIELD_COUNT)
                ui->profile_focus = (ui->profile_focus + 1) % total;
            else
                return activate(ui, ui->profile_focus == PROFILE_FIELD_COUNT ? 5 : 6);
        } else if (ui->profile_focus < PROFILE_FIELD_COUNT) {
            int i = ui->profile_focus;
            if (!form_edit(profile_field(&ui->profile_draft, i), PROFILE_FIELDS[i].capacity,
                           &ui->profile_cursors[i], key, character, printable) &&
                printable && character >= 32)
                snprintf(ui->profile_error, sizeof(ui->profile_error),
                         "Field limit: %zu UTF-8 bytes", PROFILE_FIELDS[i].capacity - 1);
            else
                ui->profile_error[0] = '\0';
        }
    } else {
        int total = ui->profile.is_self ? 3 : 4;
        if (key == '\t' || key == KEY_RIGHT)
            ui->profile_focus = (ui->profile_focus + 1) % total;
        else if (key == KEY_BTAB || key == KEY_LEFT)
            ui->profile_focus = (ui->profile_focus + total - 1) % total;
        else if (key == KEY_DOWN || key == KEY_NPAGE)
            ui->profile_offset += key == KEY_NPAGE ? h - 5 : 1;
        else if (key == KEY_UP || key == KEY_PPAGE)
            ui->profile_offset -= key == KEY_PPAGE ? h - 5 : 1;
        else if (key == '\n' || key == KEY_ENTER) {
            int actions_self[] = {0, 3, 4}, actions_other[] = {1, 2, 3, 4};
            return activate(ui, ui->profile.is_self ? actions_self[ui->profile_focus]
                                                    : actions_other[ui->profile_focus]);
        } else if ((key == 'e' || key == 'E') && ui->profile.is_self)
            return activate(ui, 0);
        else if (key == 'r' || key == 'R')
            return UI_PROFILE_REFRESH;
    }
    return UI_NONE;
}

static void put_line(WINDOW *window, int *logical, int offset, int visible, int width,
                     const char *line) {
    int rows = text_wrap_rows(line, width);
    for (int i = 0; i < rows; i++, (*logical)++) {
        if (*logical < offset || *logical >= offset + visible)
            continue;
        char part[1024];
        text_wrap_line(line, width, i, part, sizeof(part));
        mvwaddstr(window, 1 + *logical - offset, 2, part);
    }
}

static void button(ui_state_t *ui, WINDOW *win, int *row, int *col, int width, const char *text,
                   int action, bool selected) {
    if (*col + (int)strlen(text) >= width - 1) {
        (*row)++;
        *col = 2;
    }
    if (selected)
        wattron(win, A_REVERSE);
    mvwaddstr(win, *row, *col, text);
    wattroff(win, A_REVERSE);
    int *hit = ui->profile_buttons[ui->profile_button_count++];
    hit[0] = *row;
    hit[1] = *col;
    hit[2] = *col + (int)strlen(text);
    hit[3] = action;
    *col = hit[2] + 1;
}

void ui_redraw_profile(ui_state_t *ui, const cli_model_t *m) {
    int h, w, y, x;
    geometry(ui, &h, &w, &y, &x);
    WINDOW *win = newwin(h, w, y, x);
    if (!win)
        return;
    box(win, 0, 0);
    wattron(win, A_BOLD | COLOR_PAIR(1));
    mvwaddstr(win, 0, 2, ui->profile_editing ? " Edit your public profile " : " User profile ");
    wattroff(win, A_BOLD | COLOR_PAIR(1));
    ui->profile_button_count = 0;
    int cursor_y = -1, cursor_x = 0;
    if (ui->profile_loading)
        mvwaddstr(win, 1, 2, "Loading profile... Esc closes");
    else if (!ui->profile.account[0])
        mvwaddnstr(win, 1, 2, "Profile unavailable. R retries; Esc closes.", w - 4);
    else if (ui->profile_editing) {
        mvwaddnstr(win, 1, 2, "Optional fields; visible to other accounts", w - 4);
        int first = h - 5 >= PROFILE_FIELD_COUNT              ? 0
                    : ui->profile_focus < PROFILE_FIELD_COUNT ? ui->profile_focus
                                                              : PROFILE_FIELD_COUNT - 1;
        for (int i = first, row = 2; i < PROFILE_FIELD_COUNT && row < h - 3; i++, row++) {
            mvwaddnstr(win, row, 2, PROFILE_FIELDS[i].label, 12);
            const char *value = profile_field_const(&ui->profile_draft, i);
            int cursor = ui->profile_cursors[i], offset = 0, visible = w - 18;
            while (text_width(value, offset, cursor) >= visible)
                offset = text_next(value, offset);
            int end = text_cell_offset(value, offset, visible);
            if (ui->profile_focus == i)
                wattron(win, A_REVERSE);
            mvwhline(win, row, 16, ' ', visible);
            mvwaddnstr(win, row, 16, value + offset, end - offset);
            wattroff(win, A_REVERSE);
            if (ui->profile_focus == i) {
                cursor_y = row;
                cursor_x = 16 + text_width(value, offset, cursor);
            }
        }
        int col = 2, buttons_row = h - 2;
        button(ui, win, &buttons_row, &col, w, "[Save]", 5,
               ui->profile_focus == PROFILE_FIELD_COUNT);
        button(ui, win, &buttons_row, &col, w, "[Cancel]", 6,
               ui->profile_focus == PROFILE_FIELD_COUNT + 1);
    } else {
        profile_info_t *p = &ui->profile;
        char lines[10][768];
        int count = 0;
        snprintf(lines[count++], 768, "%s",
                 p->fields.display_name[0] ? p->fields.display_name : p->account);
        snprintf(lines[count++], 768, "Account: %s%s | %s%s%s", p->account,
                 p->is_admin ? " [admin]" : "",
                 !p->presence       ? "offline"
                 : p->presence == 2 ? "away"
                                    : "online",
                 p->nick[0] ? " | @" : "", p->nick);
        time_t joined = (time_t)p->member_since;
        struct tm date;
        char since[32] = "unknown";
        if (joined && localtime_r(&joined, &date))
            strftime(since, sizeof(since), "%Y-%m-%d", &date);
        snprintf(lines[count++], 768, "Member since: %s", since);
        if (p->presence == 2)
            snprintf(lines[count++], 768, "Away: %s", p->away_msg);
        for (int i = 1; i < PROFILE_FIELD_COUNT; i++)
            snprintf(lines[count++], 768, "%s: %s", PROFILE_FIELDS[i].label,
                     *profile_field(&p->fields, i) ? profile_field(&p->fields, i) : "(not set)");
        int total = 0, visible = h - 5;
        for (int i = 0; i < count; i++)
            total += text_wrap_rows(lines[i], w - 4);
        int maximum = total > visible ? total - visible : 0;
        if (ui->profile_offset > maximum)
            ui->profile_offset = maximum;
        if (ui->profile_offset < 0)
            ui->profile_offset = 0;
        int logical = 0;
        for (int i = 0; i < count; i++)
            put_line(win, &logical, ui->profile_offset, visible, w - 4, lines[i]);
        int col = 2, focus = 0, buttons_row = h - 3;
        if (p->is_self)
            button(ui, win, &buttons_row, &col, w, "[Edit]", 0, ui->profile_focus == focus++);
        else {
            button(ui, win, &buttons_row, &col, w, "[Message]", 1, ui->profile_focus == focus++);
            bool buddy = false;
            for (int i = 0; i < m->buddy_count; i++)
                if (!strcasecmp(m->buddies[i].account, p->account))
                    buddy = true;
            button(ui, win, &buttons_row, &col, w, buddy ? "[Unbuddy]" : "[Add buddy]", 2,
                   ui->profile_focus == focus++);
        }
        button(ui, win, &buttons_row, &col, w, "[Refresh]", 3, ui->profile_focus == focus++);
        button(ui, win, &buttons_row, &col, w, "[Close]", 4, ui->profile_focus == focus);
    }
    const char *message = ui->profile_discard    ? "Discard changes? Enter/Y: discard; Esc/N: keep"
                          : ui->profile_saving   ? "Saving..."
                          : ui->profile_error[0] ? ui->profile_error
                          : ui->profile_editing  ? "Tab: next | Ctrl+U: clear | Esc: cancel"
                                                 : "Arrows: scroll | Tab: actions | Esc: close";
    mvwaddnstr(win, h - 1, 1, message, w - 2);
    if (cursor_y >= 0)
        wmove(win, cursor_y, cursor_x);
    wnoutrefresh(win);
    delwin(win);
}
