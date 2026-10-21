/*
 * src/client/ui/render.c - ncurses pane rendering
 */

#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "client/history.h"
#include "client/model.h"
#include "client/text.h"
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

static void draw_text(WINDOW *w, int row, int column, const char *text, int columns) {
    char line[CLI_MSG_MAX];
    text_wrap_line(text, columns, 0, line, sizeof(line));
    mvwaddstr(w, row, column, line);
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
    draw_text(w, 0, 0, line, ui->layout.cols);
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
    draw_text(w, 0, 0, line, ui->layout.cols);
    /* overlay [away] in dim yellow if the user is away */
    if (m->is_away) {
        const char *badge_str = "[away]";
        const char *pos = strstr(line, badge_str);
        if (pos) {
            int bx = text_width(line, 0, (int)(pos - line));
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
            if (r->unread)
                snprintf(label, sizeof(label), "%c(%d) %.64s", marker, r->unread, r->name);
            else
                snprintf(label, sizeof(label), "%c%.64s", marker, r->name);
            if (!r->joined)
                wattron(w, A_DIM);
        } else {
            const cli_dm_t *d = &m->dms[index - m->room_count];
            if (d->unread)
                snprintf(label, sizeof(label), "@(%d) %.32s", d->unread, d->peer_nick);
            else
                snprintf(label, sizeof(label), "@%.32s", d->peer_nick);
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
    text_wrap_line(msg->text, text_width, line, text, sizeof(text));
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
    mvwaddstr(w, row, indent, text);
    wattroff(w, A_DIM | A_BOLD | A_REVERSE | COLOR_PAIR(CP_SYSTEM));
}

void ui_redraw_chat(ui_state_t *ui, cli_model_t *m) {
    ui->name_hit_count = 0;
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
        draw_text(w, height, 0, m->status_msg[0] ? m->status_msg : "Ready | F7 profile", width);
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
        for (int line = begin; line < rows && logical_row + line < last; line++) {
            bool grouped = !separator && !prev_ignored && can_group(prev, msg);
            if (!line && !grouped && !ignored && !msg->is_history && msg->user_id &&
                (msg->type == MSG_CHAT || msg->type == MSG_ACTION) &&
                ui->name_hit_count < CLI_HISTORY_CAP) {
                int hit = ui->name_hit_count++;
                ui->name_hits[hit].row = 2 + padding + logical_row - first;
                ui->name_hits[hit].left = ui->layout.rooms_w + 6;
                ui->name_hits[hit].right = ui->layout.rooms_w + history_prefix_width(msg, width);
                ui->name_hits[hit].uid = msg->is_self ? m->user_id : msg->user_id;
                snprintf(ui->name_hits[hit].nick, sizeof(ui->name_hits[hit].nick), "%s", msg->nick);
            }
            render_message_row(w, padding + logical_row + line - first, width, msg, m->nick, line,
                               grouped, ignored);
        }
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
    draw_text(w, height, 0, m->status_msg[0] ? m->status_msg : status, width);
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

    int maximum = m->user_count - (ui->layout.chat_h - 1);
    if (maximum < 0)
        maximum = 0;
    if (ui->users_offset > maximum)
        ui->users_offset = maximum;
    if (ui->users_offset < 0)
        ui->users_offset = 0;
    for (int i = ui->users_offset; i < m->user_count && row < ui->layout.chat_h; i++) {
        bool is_self = strcmp(m->users[i].nick, m->nick) == 0;
        if (is_self)
            wattron(w, COLOR_PAIR(CP_SELF));
        if (m->active_room >= 0 && strcmp(m->users_room, m->rooms[m->active_room].name))
            break;
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
    if (ui->input_offset > ui->input_len)
        ui->input_offset = ui->input_len;
    while (text_width(ui->input_buf, ui->input_offset, ui->input_cursor) >= visible)
        ui->input_offset = text_next(ui->input_buf, ui->input_offset);
    /* History recall can replace the draft while retaining an old byte offset. */
    while (((unsigned char)ui->input_buf[ui->input_offset] & 0xc0) == 0x80)
        ui->input_offset--;
    int end = text_cell_offset(ui->input_buf, ui->input_offset, visible);
    waddnstr(w, ui->input_buf + ui->input_offset, end - ui->input_offset);
    wmove(w, 0, 2 + text_width(ui->input_buf, ui->input_offset, ui->input_cursor));
    wnoutrefresh(w);
}

/* Help modal */

static const char *HELP_LINES[] = {"  ACCOUNT",
                                   "  F7 /profile [nick/@account]  View or edit profile",
                                   "  Click users or buddies to view their profile",
                                   "  /register <nick> <pass>   Create account & log in",
                                   "  /login    <nick> <pass>   Log in to existing account",
                                   "  /logout                   Return to sign-in screen",
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
                                   "  /msg     <nick> <message> IRC alias for /dm",
                                   "  /query   <nick>           Open a DM without sending",
                                   "  /reply   <message>        Reply to the last incoming DM",
                                   "  /unread or F5             Next unread conversation",
                                   "  /buddies or F6            Open the buddy list",
                                   "  /buddy add <account>      Watch an account's presence",
                                   "  /buddy remove <account>   Remove a buddy",
                                   "  /clear                    Clear the local chat buffer",
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
                                   "  /sound on|off|test|status  Notification audio controls",
                                   "  /emoji or F2              Insert an emoji",
                                   "  /quit                     Exit CupidChat",
                                   "",
                                   "  F1 help, F3/Ctrl+W close chat, F4 latest",
                                   "  Tab completes nicknames while typing",
                                   "  /part = /leave, /list = /rooms, /names = /users",
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

static void ui_redraw_emoji_modal(ui_state_t *ui) {
    int width = 36;
    int height = ui->layout.rows - 2;
    if (height > EMOJI_COUNT + 4)
        height = EMOJI_COUNT + 4;
    int visible = height - 4;
    int first = ui->emoji_cursor / visible * visible;
    WINDOW *w =
        newwin(height, width, (ui->layout.rows - height) / 2, (ui->layout.cols - width) / 2);
    if (!w)
        return;
    box(w, 0, 0);
    mvwaddstr(w, 0, 2, " Emoji ");
    for (int i = first; i < EMOJI_COUNT && i < first + visible; i++) {
        if (i == ui->emoji_cursor)
            wattron(w, A_REVERSE);
        char label[64];
        snprintf(label, sizeof(label), "%s  :%s:", EMOJIS[i].glyph, EMOJIS[i].name);
        draw_text(w, i - first + 1, 2, label, width - 4);
        wattroff(w, A_REVERSE);
    }
    mvwaddstr(w, height - 2, 2, "Arrows choose; Enter adds; Esc");
    wnoutrefresh(w);
    delwin(w);
}

static void ui_redraw_buddies_modal(ui_state_t *ui, const cli_model_t *m) {
    int width = ui->layout.cols - 2;
    if (width > 64)
        width = 64;
    int height = ui->layout.rows - 2;
    if (height > m->buddy_count + 5)
        height = m->buddy_count + 5;
    if (height < 6)
        height = 6;
    int visible = height - 4;
    if (ui->buddy_cursor >= m->buddy_count)
        ui->buddy_cursor = m->buddy_count ? m->buddy_count - 1 : 0;
    int first = ui->buddy_cursor / visible * visible;
    WINDOW *w =
        newwin(height, width, (ui->layout.rows - height) / 2, (ui->layout.cols - width) / 2);
    if (!w)
        return;
    box(w, 0, 0);
    mvwaddstr(w, 0, 2, " Buddies ");
    if (!m->buddy_count)
        draw_text(w, 1, 2, "/buddy add <account> to add someone", width - 4);
    for (int i = first; i < m->buddy_count && i < first + visible; i++) {
        const cli_buddy_t *b = &m->buddies[i];
        char label[128];
        const char *state = !b->presence ? "offline" : b->presence == 2 ? "away" : "online";
        snprintf(label, sizeof(label), "%s %s%s%s", state, b->account,
                 b->presence && strcmp(b->nick, b->account) ? " @" : "",
                 b->presence && strcmp(b->nick, b->account) ? b->nick : "");
        if (i == ui->buddy_cursor)
            wattron(w, A_REVERSE);
        draw_text(w, i - first + 1, 2, label, width - 4);
        wattroff(w, A_REVERSE);
    }
    if (m->buddy_count && m->buddies[ui->buddy_cursor].presence == 2)
        draw_text(w, height - 3, 2, m->buddies[ui->buddy_cursor].away_msg, width - 4);
    draw_text(w, height - 2, 2, "Enter: DM | P/click: profile | R: refresh", width - 4);
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
    if (ui->show_auth) {
        wnoutrefresh(stdscr);
        ui_redraw_auth(ui, m);
        doupdate();
        ui->dirty = false;
        return;
    }
    mvaddnstr(ui->layout.rows - 2, 0,
              ui->layout.cols >= 59   ? "[Close] [Help] [Latest] [Quit] [Emoji] [Buddies] [Profile]"
              : ui->layout.cols >= 49 ? "[Close] [Help] [Latest] [Quit] [Emoji] [Buddies]"
                                      : "[Close] [Help] [Latest] [Quit] [Emoji]",
              ui->layout.cols);
    wnoutrefresh(stdscr);
    ui_redraw_topbar(ui, m);
    ui_redraw_topic(ui, m);
    ui_redraw_chat(ui, m);
    ui_redraw_rooms(ui, m);
    ui_redraw_users(ui, m);
    ui_redraw_input(ui);
    if (ui->show_help_modal)
        ui_redraw_help_modal(ui);
    if (ui->show_emoji_modal)
        ui_redraw_emoji_modal(ui);
    if (ui->show_buddies_modal)
        ui_redraw_buddies_modal(ui, m);
    if (ui->show_profile)
        ui_redraw_profile(ui, m);
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
