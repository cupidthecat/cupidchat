/*
 * src/client/ui/render.c - ncurses pane rendering
 */

#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "client/history.h"
#include "client/model.h"
#include "client/ui.h"

#define CP_TOPBAR 1
#define CP_ROOM 2
#define CP_SELF 3
#define CP_PEER 4 /* fallback only */
#define CP_ERROR 5
#define CP_NORMAL 6
#define CP_BORDER 7
#define CP_SYSTEM 8 /* system / presence events */
#define CP_AWAY 16  /* dim yellow away tag */

/* Per-nick color cycling */
/* Pairs 9-15: retro IRC palette (cyan yellow magenta red blue white green) */
#define NICK_PALETTE_BASE 9
#define NICK_PALETTE_SIZE 7

/* djb2 hash of nick, folded into the peer palette.
 * Own nick always maps to CP_SELF (green, pair 3) so the user can always
 * identify their own messages at a glance. */
static int nick_color(const char *nick, const char *my_nick) {
    if (my_nick && strcmp(nick, my_nick) == 0)
        return CP_SELF;
    uint32_t h = 5381;
    for (const unsigned char *s = (const unsigned char *)nick; *s; s++)
        h = h * 33 ^ (uint32_t)*s;
    return NICK_PALETTE_BASE + (int)(h % (uint32_t)NICK_PALETTE_SIZE);
}

static WINDOW *W(const ui_state_t *ui, void *win) {
    (void)ui;
    return (WINDOW *)win;
}

/* Topic bar */

void ui_redraw_topic(ui_state_t *ui, const cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_topic);
    if (!w)
        return;
    werase(w);
    wattron(w, COLOR_PAIR(CP_TOPBAR)); /* same cyan palette, but no bold */
    char line[512] = {0};
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        const cli_room_t *r = &m->rooms[m->active_room];
        if (r->topic[0]) {
            if (r->topic_set_by[0])
                snprintf(line, sizeof(line), " Topic: %s  | set by %s", r->topic, r->topic_set_by);
            else
                snprintf(line, sizeof(line), " Topic: %s", r->topic);
        } else {
            snprintf(line, sizeof(line), " No topic set - type /topic <text>");
        }
    } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
        snprintf(line, sizeof(line), " Direct message with @%s", m->dms[m->active_dm].peer_nick);
    } else {
        snprintf(line, sizeof(line), " CupidChat - /join <room> to start chatting");
    }
    mvwaddnstr(w, 0, 0, line, ui->layout.cols);
    wattroff(w, COLOR_PAIR(CP_TOPBAR));
    wnoutrefresh(w);
}

/* Top bar */

void ui_redraw_topbar(ui_state_t *ui, const cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_topbar);
    if (!w)
        return;
    werase(w);
    wattron(w, COLOR_PAIR(CP_TOPBAR) | A_BOLD);
    const char *status = m->status == CLI_ONLINE       ? "[*]"
                         : m->status == CLI_CONNECTING ? "[~]"
                                                       : "[ ]";
    char line[512];
    char chat[80];
    if (m->active_room >= 0 && m->active_room < m->room_count)
        snprintf(chat, sizeof(chat), "#%.64s", m->rooms[m->active_room].name);
    else if (m->active_dm >= 0 && m->active_dm < m->dm_count)
        snprintf(chat, sizeof(chat), "@%.32s", m->dms[m->active_dm].peer_nick);
    else
        snprintf(chat, sizeof(chat), "(no chat)");
    /* show "nick (base_nick)" in topbar when user has renamed */
    char nick_display[70] = {0};
    if (m->base_nick[0] && strcmp(m->nick, m->base_nick) != 0)
        snprintf(nick_display, sizeof(nick_display), "%s (%s)", m->nick, m->base_nick);
    else
        snprintf(nick_display, sizeof(nick_display), "%s", m->nick[0] ? m->nick : "-");
    /* append [away] marker so we can chgat it with yellow later */
    if (m->is_away) {
        size_t nd_len = strlen(nick_display);
        snprintf(nick_display + nd_len, sizeof(nick_display) - nd_len, " [away]");
    }
    snprintf(line, sizeof(line), " %s %s | %s | %s ", status,
             m->server_name[0] ? m->server_name : "cupidchat", nick_display, chat);

    /* status message */
    if (m->status_msg[0] && time(NULL) < m->status_msg_until) {
        size_t base = strlen(line);
        snprintf(line + base, sizeof(line) - base, " | %s", m->status_msg);
    }
    mvwaddnstr(w, 0, 0, line, ui->layout.cols);
    /* overlay [away] in dim yellow if the user is away */
    if (m->is_away) {
        const char *badge_str = "[away]";
        const char *pos = strstr(line, badge_str);
        if (pos) {
            int bx = (int)(pos - line);
            mvwchgat(w, 0, bx, (int)strlen(badge_str), A_DIM | A_BOLD, CP_AWAY, NULL);
        }
    }
    wattroff(w, COLOR_PAIR(CP_TOPBAR) | A_BOLD);
    wnoutrefresh(w);
}

/* Rooms pane */

void ui_redraw_rooms(ui_state_t *ui, const cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_rooms);
    if (!w)
        return;
    ui_clamp_navigation(ui, m);
    werase(w);
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwvline(w, 0, ui->layout.rooms_w - 1, ACS_VLINE, ui->layout.chat_h);
    wattroff(w, COLOR_PAIR(CP_BORDER));
    mvwaddnstr(w, 0, 1, "CHATS", ui->layout.rooms_w - 2);
    int total = m->room_count + m->dm_count;
    for (int row = 1, index = ui->sidebar_offset; row < ui->layout.chat_h && index < total;
         row++, index++) {
        bool room = index < m->room_count;
        bool active = room ? index == m->active_room
                           : m->active_room < 0 && index - m->room_count == m->active_dm;
        char label[96];
        if (room) {
            const cli_room_t *r = &m->rooms[index];
            char marker = !r->joined ? '+' : r->owner_uid && r->owner_uid == m->user_id ? '~' : '#';
            snprintf(label, sizeof(label), "%c%.64s%s", marker, r->name, r->unread ? " *" : "");
            if (!r->joined)
                wattron(w, A_DIM);
        } else {
            const cli_dm_t *d = &m->dms[index - m->room_count];
            snprintf(label, sizeof(label), "@%.32s%s", d->peer_nick, d->unread ? " *" : "");
        }
        if (active)
            wattron(w, A_BOLD | COLOR_PAIR(CP_ROOM));
        if (index == ui->nav_cursor)
            wattron(w, A_REVERSE);
        mvwaddnstr(w, row, 1, label, ui->layout.rooms_w - 2);
        wattroff(w, A_DIM | A_BOLD | A_REVERSE | COLOR_PAIR(CP_ROOM));
    }
    wnoutrefresh(w);
}

/* Chat pane */

static void render_date_separator(WINDOW *w, int row, int width, uint64_t ts_ms) {
    time_t ts = (time_t)(ts_ms / 1000);
    struct tm day;
    localtime_r(&ts, &day);
    char label[64];
    strftime(label, sizeof(label), " %A %b %d, %Y ", &day);
    int length = (int)strlen(label);
    if (length > width)
        length = width;
    wattron(w, A_DIM);
    mvwhline(w, row, 0, ACS_HLINE, width);
    mvwaddnstr(w, row, (width - length) / 2, label, length);
    wattroff(w, A_DIM);
}

/* True when msg should be grouped under prev (no repeated timestamp/nick). */
static bool can_group(const cli_msg_t *prev, const cli_msg_t *msg) {
    if (!prev)
        return false;
    if (msg->type != MSG_CHAT && msg->type != MSG_ACTION)
        return false;
    if (msg->type != prev->type)
        return false;
    if (strcmp(msg->nick, prev->nick) != 0)
        return false;
    /* same minute */
    if (msg->timestamp_ms / 60000 != prev->timestamp_ms / 60000)
        return false;
    return true;
}

static void render_message_row(WINDOW *w, int row, int width, const cli_msg_t *msg,
                               const char *my_nick, int line, bool grouped, bool ignored) {
    char prefix[96], text[CLI_MSG_MAX];
    time_t ts = (time_t)(msg->timestamp_ms / 1000);
    struct tm stamp;
    localtime_r(&ts, &stamp);
    char clock[8];
    strftime(clock, sizeof(clock), "%H:%M", &stamp);
    bool system = msg->type == MSG_SYSTEM || msg->type == MSG_JOIN || msg->type == MSG_PART ||
                  msg->type == MSG_QUIT;
    if (ignored) {
        snprintf(prefix, sizeof(prefix), "%s %.16s [ignored]", clock, msg->nick);
        wattron(w, A_DIM);
        mvwaddnstr(w, row, 0, prefix, width);
        wattroff(w, A_DIM);
        return;
    }
    if (system)
        snprintf(prefix, sizeof(prefix), "%s *** ", clock);
    else if (msg->base_nick[0])
        snprintf(prefix, sizeof(prefix), "%s %s%.16s (%.16s)%s", clock,
                 msg->type == MSG_ACTION ? "* " : "", msg->nick, msg->base_nick,
                 msg->type == MSG_ACTION ? " " : ": ");
    else
        snprintf(prefix, sizeof(prefix), "%s %s%.16s%s", clock, msg->type == MSG_ACTION ? "* " : "",
                 msg->nick, msg->type == MSG_ACTION ? " " : ": ");
    int indent = history_prefix_width(msg, width);
    int text_width = width - indent;
    int offset = line * text_width;
    int remaining = (int)strlen(msg->text) - offset;
    int count = remaining < text_width ? remaining : text_width;
    if (count < 0)
        count = 0;
    memcpy(text, msg->text + offset, (size_t)count);
    text[count] = '\0';
    for (int i = 0; i < count; i++)
        if ((unsigned char)text[i] < 32 || text[i] == 127)
            text[i] = ' ';
    if (!line && !grouped) {
        int color = system ? CP_SYSTEM : nick_color(msg->nick, my_nick);
        wattron(w, COLOR_PAIR(color) | (system ? A_DIM : A_BOLD));
        mvwaddnstr(w, row, 0, prefix, indent);
        wattroff(w, COLOR_PAIR(color) | A_DIM | A_BOLD);
    }
    if (system)
        wattron(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
    if (msg->is_highlight)
        wattron(w, A_BOLD | A_REVERSE);
    mvwaddnstr(w, row, indent, text, count);
    wattroff(w, A_DIM | A_BOLD | A_REVERSE | COLOR_PAIR(CP_SYSTEM));
}

void ui_redraw_chat(ui_state_t *ui, cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_chat);
    if (!w)
        return;
    werase(w);
    const cli_msg_t *buf = NULL;
    int cap = 0, head = 0, len = 0, *offset = NULL, *view_width = NULL, *unread = NULL;
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        cli_room_t *r = &m->rooms[m->active_room];
        buf = r->msgs;
        cap = r->msgs_cap;
        head = r->msgs_head;
        len = r->msgs_len;
        offset = &r->scroll_offset;
        view_width = &r->view_width;
        unread = &r->unread;
    } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
        cli_dm_t *d = &m->dms[m->active_dm];
        buf = d->msgs;
        cap = d->msgs_cap;
        head = d->msgs_head;
        len = d->msgs_len;
        offset = &d->scroll_offset;
        view_width = &d->view_width;
        unread = &d->unread;
    }
    int width = ui->layout.chat_w, height = ui->layout.chat_h - 1;
    if (!buf || len == 0) {
        mvwaddnstr(w, 0, 0, "Select a chat. F1 opens help.", width);
        wnoutrefresh(w);
        return;
    }
    *offset = history_reflow_scroll(buf, cap, head, len, *view_width, width, *offset, m);
    *view_width = width;
    int total = history_total_rows(buf, cap, head, len, width, m);
    int maximum = total > height ? total - height : 0;
    if (*offset > maximum)
        *offset = maximum;
    if (*offset < 0)
        *offset = 0;
    if (!*offset)
        *unread = 0;
    int first = total - height - *offset;
    if (first < 0)
        first = 0;
    int last = first + height;
    int padding = total < height ? height - total : 0;
    int oldest = (head - len + cap) % cap, logical_row = 0;
    const cli_msg_t *prev = NULL;
    bool prev_ignored = false;
    for (int i = 0; i < len && logical_row < last; i++) {
        const cli_msg_t *msg = &buf[(oldest + i) % cap];
        bool separator = !prev || !history_same_day(prev->timestamp_ms, msg->timestamp_ms);
        if (separator) {
            if (logical_row >= first && logical_row < last)
                render_date_separator(w, padding + logical_row - first, width, msg->timestamp_ms);
            logical_row++;
        }
        bool ignored =
            (msg->type == MSG_CHAT || msg->type == MSG_ACTION) && model_is_ignored(m, msg->nick);
        int rows = history_message_rows(msg, width, ignored);
        int begin = first > logical_row ? first - logical_row : 0;
        for (int line = begin; line < rows && logical_row + line < last; line++)
            render_message_row(w, padding + logical_row + line - first, width, msg, m->nick, line,
                               !separator && !prev_ignored && can_group(prev, msg), ignored);
        logical_row += rows;
        prev = msg;
        prev_ignored = ignored;
    }
    char status[160];
    if (*offset)
        snprintf(status, sizeof(status), " %d lines below | %d messages retained | F4 latest",
                 *offset, len);
    else
        snprintf(status, sizeof(status), " Live | %d messages retained", len);
    if (!*offset && m->active_room >= 0) {
        cli_room_t *r = &m->rooms[m->active_room];
        if (r->typing_nick[0] && time(NULL) < r->typing_until)
            snprintf(status, sizeof(status), " %.32s is typing...", r->typing_nick);
    }
    wattron(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
    mvwaddnstr(w, height, 0, status, width);
    wattroff(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
    wnoutrefresh(w);
}

/* Users pane */

void ui_redraw_users(ui_state_t *ui, const cli_model_t *m) {
    if (!ui->show_users || !ui->win_users)
        return;
    WINDOW *w = W(ui, ui->win_users);
    werase(w);
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwvline(w, 0, 0, ACS_VLINE, ui->layout.chat_h);
    wattroff(w, COLOR_PAIR(CP_BORDER));

    int row = 0;
    wattron(w, A_DIM);
    mvwaddstr(w, row++, 2, "USERS");
    wattroff(w, A_DIM);

    for (int i = 0; i < m->user_count && row < ui->layout.chat_h; i++) {
        bool is_self = strcmp(m->users[i].nick, m->nick) == 0;
        if (is_self)
            wattron(w, COLOR_PAIR(CP_SELF));
        mvwaddnstr(w, row++, 2, m->users[i].nick, ui->layout.users_w - 3);
        if (is_self)
            wattroff(w, COLOR_PAIR(CP_SELF));
    }
    wnoutrefresh(w);
}

/* Input line */

void ui_redraw_input(ui_state_t *ui) {
    WINDOW *w = W(ui, ui->win_input);
    if (!w)
        return;
    werase(w);
    wattron(w, A_BOLD);
    waddstr(w, "> ");
    wattroff(w, A_BOLD);
    int visible = ui->layout.cols - 3;
    if (ui->input_cursor < ui->input_offset)
        ui->input_offset = ui->input_cursor;
    if (ui->input_cursor >= ui->input_offset + visible)
        ui->input_offset = ui->input_cursor - visible + 1;
    if (ui->input_offset > ui->input_len)
        ui->input_offset = ui->input_len;
    waddnstr(w, ui->input_buf + ui->input_offset, visible);
    wmove(w, 0, 2 + ui->input_cursor - ui->input_offset);
    wnoutrefresh(w);
}

/* Help modal */

static const char *HELP_LINES[] = {"  ACCOUNT",
                                   "  /register <nick> <pass>   Create account & log in",
                                   "  /login    <nick> <pass>   Log in to existing account",
                                   "  /logout                   Return to guest session",
                                   "  /nick    <newnick>        Change your display name",
                                   "",
                                   "  ROOMS  (~name = room you own, deletable)",
                                   "  /join  <room>             Join or create a room",
                                   "  /leave [room]             Leave current or named room",
                                   "  /topic [text]             Set or show room topic",
                                   "  /delroom [room]           Delete a room you own",
                                   "",
                                   "  MESSAGES",
                                   "  /me      <action>         Emote (* you wave)",
                                   "  /dm      <nick> <message> Send a direct message",
                                   "  /closedm [nick]           Close a DM conversation",
                                   "  /away    [message]        Set away with optional message",
                                   "  /back                     Return from away",
                                   "  /ignore  <nick>           Suppress messages from nick",
                                   "  /unignore <nick>          Remove nick from ignore list",
                                   "",
                                   "  MISC",
                                   "  /rooms                    Refresh room list",
                                   "  /users [room]             Refresh user list",
                                   "  /help                     Show this help",
                                   "  /quit                     Exit CupidChat",
                                   "",
                                   "  F1 help, F3/Ctrl+W close chat, F4 latest",
                                   "  F10/Ctrl+Q quit, mouse wheel scrolls chat",
                                   "  Up/Down scroll help; Esc closes it",
                                   NULL};

static void ui_redraw_help_modal(ui_state_t *ui) {
    int rows = ui->layout.rows;
    int cols = ui->layout.cols;

    /* count lines and find the longest one to size the box dynamically */
    int nlines = 0;
    int max_line = 0;
    for (const char **l = HELP_LINES; *l; l++) {
        int len = (int)strlen(*l);
        if (len > max_line)
            max_line = len;
        nlines++;
    }

    /* +2 for the left/right border columns; inner content area = box_w - 2 */
    int box_w = max_line + 2;
    int box_h = nlines + 4; /* 2 border rows + 1 title row + 1 blank */
    if (box_w > cols - 2)
        box_w = cols - 2;
    if (box_h > rows - 2)
        box_h = rows - 2;

    int starty = (rows - box_h) / 2;
    int startx = (cols - box_w) / 2;

    WINDOW *w = newwin(box_h, box_w, starty, startx);
    if (!w)
        return;

    wbkgd(w, COLOR_PAIR(CP_TOPBAR));
    wattron(w, COLOR_PAIR(CP_TOPBAR) | A_BOLD);
    box(w, 0, 0);

    /* title line */
    const char *title = " CUPIDCHAT HELP ";
    int tx = (box_w - (int)strlen(title)) / 2;
    if (tx < 1)
        tx = 1;
    mvwaddnstr(w, 0, tx, title, box_w - tx - 1);

    wattroff(w, A_BOLD);

    int row = 2;
    int maximum = nlines - (box_h - 3);
    if (maximum < 0)
        maximum = 0;
    if (ui->help_offset > maximum)
        ui->help_offset = maximum;
    for (const char **l = HELP_LINES + ui->help_offset; *l && row < box_h - 1; l++, row++) {
        mvwaddnstr(w, row, 1, *l, box_w - 2);
    }

    wattroff(w, COLOR_PAIR(CP_TOPBAR));
    wnoutrefresh(w);
    delwin(w);
}

/* Full redraw */

void ui_redraw(ui_state_t *ui, cli_model_t *m) {
    erase();
    if (ui->layout.too_small) {
        mvaddnstr(0, 0, "Resize to at least 40x8. Ctrl+Q quits.", ui->layout.cols);
        if (ui->confirm_quit && ui->layout.rows > 1)
            mvaddnstr(1, 0, "Quit? Enter=yes, Esc=no", ui->layout.cols);
        refresh();
        ui->dirty = false;
        return;
    }
    mvaddnstr(ui->layout.rows - 2, 0, "[Close] [Help] [Latest] [Quit]", ui->layout.cols);
    wnoutrefresh(stdscr);
    ui_redraw_topbar(ui, m);
    ui_redraw_topic(ui, m);
    ui_redraw_chat(ui, m);
    ui_redraw_rooms(ui, m);
    ui_redraw_users(ui, m);
    ui_redraw_input(ui);
    if (ui->show_help_modal)
        ui_redraw_help_modal(ui);
    if (ui->confirm_quit) {
        int width = ui->layout.cols < 48 ? ui->layout.cols - 2 : 46;
        WINDOW *dialog = newwin(5, width, (ui->layout.rows - 5) / 2, (ui->layout.cols - width) / 2);
        if (dialog) {
            box(dialog, 0, 0);
            mvwaddnstr(dialog, 1, 2, "Quit CupidChat?", width - 4);
            mvwaddnstr(dialog, 3, 2, "Enter: quit   Esc: keep chatting", width - 4);
            wnoutrefresh(dialog);
            delwin(dialog);
        }
    }
    doupdate();
    ui->dirty = false;
}
