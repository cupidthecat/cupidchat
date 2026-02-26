/*
 * src/client/ui/render.c - ncurses pane rendering
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ncurses.h>

#include "client/ui.h"
#include "client/model.h"
/* history.h not needed chat uses direct ring arithmetic */

#define CP_TOPBAR   1
#define CP_ROOM     2
#define CP_SELF     3
#define CP_PEER     4   /* fallback only */
#define CP_ERROR    5
#define CP_NORMAL   6
#define CP_BORDER   7
#define CP_SYSTEM   8   /* system / presence events */
#define CP_AWAY     16  /* dim yellow away tag */

/* Per-nick color cycling */
/* Pairs 9-15: retro IRC palette (cyan yellow magenta red blue white green) */
#define NICK_PALETTE_BASE  9
#define NICK_PALETTE_SIZE  7

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

static WINDOW *W(const ui_state_t *ui, void *win) { (void)ui; return (WINDOW *)win; }

/* Topic bar */

void ui_redraw_topic(ui_state_t *ui, const cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_topic);
    if (!w) return;
    werase(w);
    wattron(w, COLOR_PAIR(CP_TOPBAR));   /* same cyan palette, but no bold */
    char line[512] = {0};
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        const cli_room_t *r = &m->rooms[m->active_room];
        if (r->topic[0]) {
            if (r->topic_set_by[0])
                snprintf(line, sizeof(line), " Topic: %s  | set by %s",
                         r->topic, r->topic_set_by);
            else
                snprintf(line, sizeof(line), " Topic: %s", r->topic);
        } else {
            snprintf(line, sizeof(line), " No topic set - type /topic <text>");
        }
    } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
        snprintf(line, sizeof(line), " Direct message with @%s",
                 m->dms[m->active_dm].peer_nick);
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
    if (!w) return;
    werase(w);
    wattron(w, COLOR_PAIR(CP_TOPBAR) | A_BOLD);
    const char *status =
        m->status == CLI_ONLINE       ? "[*]" :
        m->status == CLI_CONNECTING   ? "[~]" : "[ ]";
    char line[512];
    const char *room_str = (m->active_room >= 0 && m->active_room < m->room_count)
                         ? m->rooms[m->active_room].name : "(no room)";
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
    snprintf(line, sizeof(line), " %s %s | %s | #%s ",
             status, m->server_name[0] ? m->server_name : "cupidchat",
             nick_display, room_str);

    /* status message */
    if (m->status_msg[0] && time(NULL) < m->status_msg_until) {
        size_t base = strlen(line);
        snprintf(line + base, sizeof(line) - base,
                 " | %s", m->status_msg);
    }
    mvwaddnstr(w, 0, 0, line, ui->layout.cols);
    /* overlay [away] in dim yellow if the user is away */
    if (m->is_away) {
        const char *badge_str = "[away]";
        const char *pos = strstr(line, badge_str);
        if (pos) {
            int bx = (int)(pos - line);
            mvwchgat(w, 0, bx, (int)strlen(badge_str),
                     A_DIM | A_BOLD, CP_AWAY, NULL);
        }
    }
    wattroff(w, COLOR_PAIR(CP_TOPBAR) | A_BOLD);
    wnoutrefresh(w);
}

/* Rooms pane */

void ui_redraw_rooms(ui_state_t *ui, const cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_rooms);
    if (!w) return;
    werase(w);
    wattron(w, COLOR_PAIR(CP_BORDER));
    mvwvline(w, 0, ui->layout.rooms_w - 1, ACS_VLINE, ui->layout.chat_h);
    wattroff(w, COLOR_PAIR(CP_BORDER));

    int row = 0;
    wattron(w, A_DIM);
    mvwaddstr(w, row++, 1, "ROOMS");
    wattroff(w, A_DIM);

    for (int i = 0; i < m->room_count && row < ui->layout.chat_h; i++) {
        const cli_room_t *r = &m->rooms[i];
        if (!r->active) continue;

        bool is_active  = (i == m->active_room);
        bool is_nav     = (i == ui->nav_cursor);

        if (is_nav)    wattron(w, A_REVERSE);
        if (is_active) wattron(w, A_BOLD | COLOR_PAIR(CP_ROOM));
        else if (r->unread > 0) wattron(w, A_BOLD);

        char label[26];
        if (r->unread > 0)
            snprintf(label, sizeof(label), "%s%-*s %d",
                     r->owner_uid && r->owner_uid == m->user_id ? "~" : "#",
                     ui->layout.rooms_w - 6, r->name, r->unread);
        else
            snprintf(label, sizeof(label), "%s%s",
                     r->owner_uid && r->owner_uid == m->user_id ? "~" : "#",
                     r->name);
        mvwaddnstr(w, row, 1, label, ui->layout.rooms_w - 2);
        row++;

        if (is_active) wattroff(w, A_BOLD | COLOR_PAIR(CP_ROOM));
        else if (r->unread > 0) wattroff(w, A_BOLD);
        if (is_nav)    wattroff(w, A_REVERSE);
    }

    /* DM entries */
    for (int i = 0; i < m->dm_count && row < ui->layout.chat_h; i++) {
        bool is_active = (i == m->active_dm && m->active_room < 0);
        bool is_nav    = (i + m->room_count == ui->nav_cursor);
        const cli_dm_t *d = &m->dms[i];
        if (is_nav)    wattron(w, A_REVERSE);
        if (is_active) wattron(w, A_BOLD | COLOR_PAIR(CP_PEER));
        char label[24];
        snprintf(label, sizeof(label), "@%s", d->peer_nick);
        mvwaddnstr(w, row++, 1, label, ui->layout.rooms_w - 2);
        if (is_active) wattroff(w, A_BOLD | COLOR_PAIR(CP_PEER));
        if (is_nav)    wattroff(w, A_REVERSE);
    }

    wnoutrefresh(w);
}

/* Chat pane */

/* Integer calendar day (UTC) from a millisecond timestamp. */
static int msg_day(const cli_msg_t *msg) {
    return (int)(msg->timestamp_ms / 86400000ULL);
}

/* Draw one row:    Wednesday Feb 26, 2026   (A_DIM, ACS_HLINE) */
static void render_date_separator(WINDOW *w, int row, int chat_w, uint64_t ts_ms) {
    time_t ts = (time_t)(ts_ms / 1000);
    struct tm tm_buf;
    localtime_r(&ts, &tm_buf);
    char date_str[40];
    strftime(date_str, sizeof(date_str), "%A %b %d, %Y", &tm_buf);

    int label_len = (int)strlen(date_str) + 2;  /* spaces on each side */
    int line_w    = (chat_w - label_len) / 2;
    if (line_w < 1) line_w = 1;

    wmove(w, row, 0);
    wattron(w, A_DIM);
    for (int x = 0; x < line_w; x++)
        waddch(w, ACS_HLINE);
    waddch(w, ' ');
    waddstr(w, date_str);
    waddch(w, ' ');
    for (int x = 0; x < line_w; x++)
        waddch(w, ACS_HLINE);
    wattroff(w, A_DIM);
}

/* True when msg should be grouped under prev (no repeated timestamp/nick). */
static bool can_group(const cli_msg_t *prev, const cli_msg_t *msg) {
    if (!prev) return false;
    if (msg->type != MSG_CHAT && msg->type != MSG_ACTION) return false;
    if (msg->type != prev->type) return false;
    if (strcmp(msg->nick, prev->nick) != 0) return false;
    /* same minute */
    if (msg->timestamp_ms / 60000 != prev->timestamp_ms / 60000) return false;
    return true;
}

/* Returns the number of terminal rows msg will occupy in a chat_w-wide pane.
 * Pass ignored=true to get the 1-row stub height. */
static int msg_row_count(const cli_msg_t *msg, int chat_w, bool ignored) {
    if (ignored) return 1;   /* stubbed to one dim line */
    int nick_len      = (int)strnlen(msg->nick,      16);
    int base_nick_len = (int)strnlen(msg->base_nick, 16);
    bool has_base = (msg->base_nick[0] != '\0');
    int prefix_w;
    switch (msg->type) {
        case MSG_JOIN: case MSG_PART: case MSG_QUIT: case MSG_SYSTEM:
            prefix_w = 10;               /* "HH:MM *** " */
            break;
        case MSG_ACTION:
            /* "HH:MM * nick " or "HH:MM * nick (base) " */
            prefix_w = has_base ? 12 + nick_len + base_nick_len
                                :  9 + nick_len;
            break;
        case MSG_CHAT: default:
            /* "HH:MM nick: " or "HH:MM nick (base): " */
            prefix_w = has_base ? 11 + nick_len + base_nick_len
                                :  8 + nick_len;
            break;
    }
    int text_w = chat_w - prefix_w;
    if (text_w < 1) text_w = 1;
    int tlen = (int)strlen(msg->text);
    if (tlen == 0) return 1;
    return (tlen + text_w - 1) / text_w;
}

/* Render msg starting at `row`, wrapping onto subsequent rows.
 * Does not write beyond row + rows_avail - 1.
 * When grouped=true the timestamp+nick header is replaced by blank indent.
 * When ignored=true renders a single dim stub line.
 * Returns the number of rows consumed. */
static int render_msg_line(WINDOW *w, int row, int rows_avail, int chat_w,
                            const cli_msg_t *msg, const char *my_nick,
                            bool grouped, bool ignored) {
    if (ignored) {
        /* single dim stub: "HH:MM <nick> [ignored]" */
        time_t ts = (time_t)(msg->timestamp_ms / 1000);
        struct tm tm_buf;
        localtime_r(&ts, &tm_buf);
        char tstr[8]; strftime(tstr, sizeof(tstr), "%H:%M", &tm_buf);
        wmove(w, row, 0);
        wattron(w, A_DIM);
        waddnstr(w, tstr, 5); waddch(w, ' ');
        waddnstr(w, msg->nick, 16);
        waddstr(w, " [ignored]");
        wattroff(w, A_DIM);
        return 1;
    }
    time_t ts = (time_t)(msg->timestamp_ms / 1000);
    struct tm tm_buf;
    localtime_r(&ts, &tm_buf);
    char tstr[8];
    strftime(tstr, sizeof(tstr), "%H:%M", &tm_buf);

    int nick_len = (int)strnlen(msg->nick, 16);
    int rows_written = 0;

    switch (msg->type) {
        case MSG_JOIN:
        case MSG_PART:
        case MSG_QUIT:
        case MSG_SYSTEM: {
            int prefix_w = 10;  /* "HH:MM *** " */
            int text_w   = chat_w - prefix_w;
            if (text_w < 1) text_w = 1;
            const char *p  = msg->text;
            int         rem = (int)strlen(p);
            while (rows_written < rows_avail) {
                wmove(w, row + rows_written, 0);
                if (rows_written == 0) {
                    wattron(w, A_DIM);
                    waddnstr(w, tstr, 5); waddch(w, ' ');
                    wattroff(w, A_DIM);
                    wattron(w, COLOR_PAIR(CP_SYSTEM) | A_DIM);
                    waddstr(w, "*** ");
                    wattroff(w, COLOR_PAIR(CP_SYSTEM) | A_DIM);
                } else {
                    wmove(w, row + rows_written, prefix_w);
                }
                int chunk = rem < text_w ? rem : text_w;
                wattron(w, COLOR_PAIR(CP_SYSTEM) | A_DIM);
                waddnstr(w, p, chunk);
                wattroff(w, COLOR_PAIR(CP_SYSTEM) | A_DIM);
                p += chunk; rem -= chunk; rows_written++;
                if (rem <= 0) break;
            }
            break;
        }

        case MSG_ACTION: {
            bool has_base = (msg->base_nick[0] != '\0');
            int base_nick_len = has_base ? (int)strnlen(msg->base_nick, 16) : 0;
            /* "HH:MM * nick " or "HH:MM * nick (base) " */
            int prefix_w = has_base ? 12 + nick_len + base_nick_len
                                    :  9 + nick_len;
            int text_w   = chat_w - prefix_w;
            if (text_w < 1) text_w = 1;
            int         cp  = nick_color(msg->nick, my_nick);
            const char *p   = msg->text;
            int         rem = (int)strlen(p);
            while (rows_written < rows_avail) {
                wmove(w, row + rows_written, 0);
                if (rows_written == 0) {
                    if (grouped) {
                        for (int k = 0; k < prefix_w; k++) waddch(w, ' ');
                    } else {
                        wattron(w, A_DIM);
                        waddnstr(w, tstr, 5); waddch(w, ' ');
                        wattroff(w, A_DIM);
                        wattron(w, COLOR_PAIR(cp));
                        waddstr(w, "* ");
                        waddnstr(w, msg->nick, 16);
                        if (has_base) {
                            waddstr(w, " (");
                            waddnstr(w, msg->base_nick, 16);
                            waddch(w, ')');
                        }
                        waddch(w, ' ');
                        wattroff(w, COLOR_PAIR(cp));
                    }
                } else {
                    wmove(w, row + rows_written, prefix_w);
                }
                int chunk = rem < text_w ? rem : text_w;
                if (msg->is_highlight) wattron(w, A_BOLD | A_REVERSE);
                waddnstr(w, p, chunk);
                if (msg->is_highlight) wattroff(w, A_BOLD | A_REVERSE);
                p += chunk; rem -= chunk; rows_written++;
                if (rem <= 0) break;
            }
            break;
        }

        case MSG_CHAT:
        default: {
            bool has_base = (msg->base_nick[0] != '\0');
            int base_nick_len = has_base ? (int)strnlen(msg->base_nick, 16) : 0;
            /* "HH:MM nick: " or "HH:MM nick (base): " */
            int prefix_w = has_base ? 11 + nick_len + base_nick_len
                                    :  8 + nick_len;
            int text_w   = chat_w - prefix_w;
            if (text_w < 1) text_w = 1;
            int         cp  = nick_color(msg->nick, my_nick);
            const char *p   = msg->text;
            int         rem = (int)strlen(p);
            while (rows_written < rows_avail) {
                wmove(w, row + rows_written, 0);
                if (rows_written == 0) {
                    if (grouped) {
                        for (int k = 0; k < prefix_w; k++) waddch(w, ' ');
                    } else {
                        wattron(w, A_DIM);
                        waddnstr(w, tstr, 5); waddch(w, ' ');
                        wattroff(w, A_DIM);
                        wattron(w, COLOR_PAIR(cp) | A_BOLD);
                        waddnstr(w, msg->nick, 16);
                        wattroff(w, COLOR_PAIR(cp) | A_BOLD);
                        if (has_base) {
                            wattron(w, A_DIM);
                            waddstr(w, " (");
                            waddnstr(w, msg->base_nick, 16);
                            waddch(w, ')');
                            wattroff(w, A_DIM);
                        }
                        waddstr(w, ": ");
                    }
                } else {
                    wmove(w, row + rows_written, prefix_w);
                }
                int chunk = rem < text_w ? rem : text_w;
                if (msg->is_highlight) wattron(w, A_BOLD | A_REVERSE);
                waddnstr(w, p, chunk);
                if (msg->is_highlight) wattroff(w, A_BOLD | A_REVERSE);
                p += chunk; rem -= chunk; rows_written++;
                if (rem <= 0) break;
            }
            break;
        }
    }
    return rows_written > 0 ? rows_written : 1;
}

void ui_redraw_chat(ui_state_t *ui, const cli_model_t *m) {
    WINDOW *w = W(ui, ui->win_chat);
    if (!w) return;
    werase(w);

    const cli_msg_t *buf   = NULL;
    int cap = 0, head = 0, len = 0, scroll = 0;

    if (m->active_room >= 0 && m->active_room < m->room_count) {
        const cli_room_t *r = &m->rooms[m->active_room];
        buf    = r->msgs;
        cap    = r->msgs_cap;
        head   = r->msgs_head;
        len    = r->msgs_len;
        scroll = r->scroll_offset;
    } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
        const cli_dm_t *d = &m->dms[m->active_dm];
        buf    = d->msgs;
        cap    = d->msgs_cap;
        head   = d->msgs_head;
        len    = d->msgs_len;
        scroll = d->scroll_offset;
    }

    const int chat_h = ui->layout.chat_h;
    const int chat_w = ui->layout.chat_w;

    if (!buf || len == 0) {
        wattron(w, A_DIM);
        mvwaddstr(w, 0, 1, "No messages yet.");
        wattroff(w, A_DIM);
        wnoutrefresh(w);
        return;
    }

    /* Index of the oldest message in the ring buffer */
    int oldest_idx = ((head - len) % cap + cap) % cap;

    /* end_msg: 0-based offset from oldest of the most recent visible message.
     * scroll=0 -> newest; scroll=N -> skip N from the end. */
    int end_msg = len - 1 - scroll;
    if (end_msg < 0) end_msg = 0;

    /* Walk backwards from end_msg, collecting messages whose row heights
     * fit within chat_h rows.  sel[] stores ring indices, newest first. */
    int  sel[1024];
    bool sel_ignored[1024];
    int sel_count   = 0;
    int rows_accum  = 0;

    for (int i = end_msg; i >= 0; i--) {
        int ri = (oldest_idx + i) % cap;
        bool ign = ((buf[ri].type == MSG_CHAT || buf[ri].type == MSG_ACTION)
                    && model_is_ignored(m, buf[ri].nick));
        int h  = msg_row_count(&buf[ri], chat_w, ign);
        /* count one extra row for a day-boundary separator above this message:
         * between two adjacent messages when their calendar day differs
         * before the very oldest message in the buffer (session start)
         *
         * IMPORTANT: the renderer always draws a session-start separator before
         * the first (oldest) visible message when skip_above==0, using the
         * prev_day==-1 branch even when all messages share the same calendar
         * day.  We must account for that row here or the newest message(s) get
         * pushed off the bottom of the pane. */
        if (sel_count > 0) {
            int newer_ri = sel[sel_count - 1];
            if (msg_day(&buf[ri]) != msg_day(&buf[newer_ri])) {
                h += 1;  /* day-boundary separator */
            } else if (i == 0) {
                h += 1;  /* session-start separator (same day, still rendered) */
            }
        } else if (i == 0) {
            /* this is the oldest message in the whole buffer separator before it */
            h += 1;
        }
        rows_accum += h;
        sel[sel_count]         = ri;
        sel_ignored[sel_count] = ign;
        sel_count++;
        if (sel_count >= 1024) break;
        /* stop collecting only AFTER the message has been added, so the oldest
         * visible message is never left out due to an exact-fill boundary */
        if (rows_accum >= chat_h) break;
    }

    /* How many messages sit above the viewport (not shown) */
    int skip_above = (end_msg + 1) - sel_count;

    /* If messages don't fill the pane, push them to the bottom */
    int row = (rows_accum < chat_h) ? (chat_h - rows_accum) : 0;

    /* Render oldest -> newest (sel is newest-first, so iterate reversed) */
    int               prev_day = -1;
    const cli_msg_t  *prev_msg = NULL;
    for (int i = sel_count - 1; i >= 0; i--) {
        const cli_msg_t *msg     = &buf[sel[i]];
        bool             ignored = sel_ignored[i];
        int this_day = msg_day(msg);

        /* Decide whether to draw a date separator before this message:
         *  * day changed from the previous rendered message
         *  * OR this is the first message and it sits at the top of all history */
        bool need_sep = (prev_day >= 0)
                        ? (this_day != prev_day)
                        : (skip_above == 0);   /* session/history start */

        if (need_sep) {
            int avail = chat_h - row;
            if (avail > 0) {
                render_date_separator(w, row, chat_w, msg->timestamp_ms);
                row++;
            }
            prev_msg = NULL;   /* separator always breaks grouping */
        }
        prev_day = this_day;

        bool grouped = !ignored && can_group(prev_msg, msg);

        int avail = chat_h - row;
        if (avail <= 0) break;
        int used = render_msg_line(w, row, avail, chat_w, msg, m->nick, grouped, ignored);
        row += used;
        prev_msg = ignored ? NULL : msg;   /* ignored msgs don't influence grouping */
    }

    /* scroll indicators */
    if (skip_above > 0) {
        char ind[80];
        snprintf(ind, sizeof(ind), "  --- %d earlier messages (PgUp) ---", skip_above);
        wattron(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
        mvwaddnstr(w, 0, 0, ind, chat_w);
        wattroff(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
    }
    if (scroll > 0) {
        char ind[80];
        snprintf(ind, sizeof(ind), "  --- %d more messages below (PgDn) ---", scroll);
        wattron(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
        mvwaddnstr(w, chat_h - 1, 0, ind, chat_w);
        wattroff(w, A_DIM | COLOR_PAIR(CP_SYSTEM));

        /* top-right corner: "^ 47 lines" */
        char badge[32];
        snprintf(badge, sizeof(badge), " ^ %d lines ", scroll);
        int blen = (int)strlen(badge);
        wattron(w, A_REVERSE | A_BOLD);
        mvwaddnstr(w, 0, chat_w - blen, badge, blen);
        wattroff(w, A_REVERSE | A_BOLD);
    }

    /* typing indicator */
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        const cli_room_t *tr = &m->rooms[m->active_room];
        if (tr->typing_nick[0] && time(NULL) < tr->typing_until) {
            char tind[80];
            snprintf(tind, sizeof(tind), " * %s is typing...", tr->typing_nick);
            wattron(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
            mvwaddnstr(w, chat_h - 1, 0, tind, chat_w);
            wattroff(w, A_DIM | COLOR_PAIR(CP_SYSTEM));
        }
    }

    wnoutrefresh(w);
}

/* Users pane */

void ui_redraw_users(ui_state_t *ui, const cli_model_t *m) {
    if (!ui->show_users || !ui->win_users) return;
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
        if (is_self) wattron(w, COLOR_PAIR(CP_SELF));
        mvwaddnstr(w, row++, 2, m->users[i].nick, ui->layout.users_w - 3);
        if (is_self) wattroff(w, COLOR_PAIR(CP_SELF));
    }
    wnoutrefresh(w);
}

/* Input line */

void ui_redraw_input(ui_state_t *ui) {
    WINDOW *w = W(ui, ui->win_input);
    if (!w) return;
    werase(w);
    wattron(w, A_BOLD);
    waddstr(w, "> ");
    wattroff(w, A_BOLD);
    waddnstr(w, ui->input_buf, ui->layout.cols - 3);
    /* position cursor */
    wmove(w, 0, 2 + ui->input_cursor);
    wnoutrefresh(w);
}

/* Help modal */

static const char *HELP_LINES[] = {
    "  ACCOUNT",
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
    "  Press any key to close",
    NULL
};

static void ui_redraw_help_modal(ui_state_t *ui) {
    int rows = ui->layout.rows;
    int cols = ui->layout.cols;

    /* count lines and find the longest one to size the box dynamically */
    int nlines = 0;
    int max_line = 0;
    for (const char **l = HELP_LINES; *l; l++) {
        int len = (int)strlen(*l);
        if (len > max_line) max_line = len;
        nlines++;
    }

    /* +2 for the left/right border columns; inner content area = box_w - 2 */
    int box_w = max_line + 2;
    int box_h = nlines + 4;   /* 2 border rows + 1 title row + 1 blank */
    if (box_w > cols - 4) box_w = cols - 4;
    if (box_h > rows - 4) box_h = rows - 4;

    int starty = (rows - box_h) / 2;
    int startx = (cols - box_w) / 2;

    WINDOW *w = newwin(box_h, box_w, starty, startx);
    if (!w) return;

    wbkgd(w, COLOR_PAIR(CP_TOPBAR));
    wattron(w, COLOR_PAIR(CP_TOPBAR) | A_BOLD);
    box(w, 0, 0);

    /* title line */
    const char *title = " CUPIDCHAT HELP ";
    int tx = (box_w - (int)strlen(title)) / 2;
    mvwaddstr(w, 0, tx, title);

    wattroff(w, A_BOLD);

    int row = 2;
    for (const char **l = HELP_LINES; *l && row < box_h - 1; l++, row++) {
        mvwaddnstr(w, row, 1, *l, box_w - 2);
    }

    wattroff(w, COLOR_PAIR(CP_TOPBAR));
    wnoutrefresh(w);
    delwin(w);
}

/* Full redraw */

void ui_redraw(ui_state_t *ui, const cli_model_t *m) {
    ui_redraw_topbar(ui, m);
    ui_redraw_topic(ui, m);
    ui_redraw_rooms(ui, m);
    ui_redraw_chat(ui, m);
    ui_redraw_users(ui, m);
    ui_redraw_input(ui);
    if (ui->show_help_modal) ui_redraw_help_modal(ui);
    doupdate();
    ui->dirty = false;
}
