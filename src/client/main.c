/*
 * src/client/main.c - cupid-chat TUI client entry point
 *
 * Event loop:
 *   poll() on socket fd + timer
 *   network frames -> model updates
 *   keyboard input -> commands / send messages
 *   model changes  -> UI redraws
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasestr for mention detection */
#include <time.h>
#include <unistd.h>

#include "client/sound.h"

#include "client/client_config.h"
#include "client/client_conn.h"
#include "client/history.h"
#include "client/model.h"
#include "client/ui.h"
#include "proto/frame.h"
#include "proto/tlv.h"
#include "proto/types.h"

/* Signal helpers */

static volatile int g_sigwinch = 0;
static volatile int g_quit = 0;

static void on_sigwinch(int s) {
    (void)s;
    g_sigwinch = 1;
}

static void on_sigint(int s) {
    (void)s;
    g_quit = 1;
}

/* Config parsing */

void client_config_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --host HOST        Server hostname (default: 127.0.0.1)\n"
            "  --port PORT        Server port    (default: 5555)\n"
            "  --nick NICK        Initial account name (optional)\n"
            "  --tls              Use TLS connection\n"
            "  --verbose          Debug output\n"
            "  --sound-test       Play a sound and show audio errors without connecting\n"
            "  -h, --help\n",
            prog);
}

int client_config_parse(int argc, char **argv, client_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host, "127.0.0.1", sizeof(cfg->host) - 1);
    cfg->port = 5555;

    static const struct option opts[] = {
        {"host", required_argument, NULL, 'H'}, {"port", required_argument, NULL, 'p'},
        {"nick", required_argument, NULL, 'n'}, {"tls", no_argument, NULL, 't'},
        {"verbose", no_argument, NULL, 'v'},    {"sound-test", no_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},       {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (c) {
            case 'H':
                strncpy(cfg->host, optarg, sizeof(cfg->host) - 1);
                break;
            case 'p':
                cfg->port = (uint16_t)atoi(optarg);
                break;
            case 'n':
                if (strlen(optarg) > MAX_NICK_LEN)
                    return -1;
                strncpy(cfg->nick, optarg, sizeof(cfg->nick) - 1);
                break;
            case 't':
                cfg->use_tls = true;
                break;
            case 'v':
                cfg->verbose = true;
                break;
            case 's':
                cfg->sound_test = true;
                break;
            case 'h':
                return 1;
            default:
                return -1;
        }
    }
    return 0;
}

/* Frame handler (network -> model) */

typedef struct {
    cli_model_t *model;
    client_conn_t *conn;
    ui_state_t *ui;
    time_t *last_users_refresh;
    time_t *last_rooms_refresh;
} frame_ctx_t;

static void select_dm(cli_model_t *m, ui_state_t *ui, uint32_t uid, const char *nick) {
    cli_dm_t *dm = model_get_or_create_dm(m, uid, nick);
    if (!dm) {
        model_set_status(m, "Cannot open another DM (limit 64)", 5);
        return;
    }
    m->active_room = -1;
    m->active_dm = (int)(dm - m->dms);
    dm->unread = dm->scroll_offset = 0;
    ui->nav_cursor = m->room_count + m->active_dm;
    ui->dirty = true;
}

static void buddy_frames(const frame_t *f, cli_model_t *m, ui_state_t *ui) {
    cli_buddy_t entries[MAX_BUDDIES] = {0};
    int count = 0;
    tlv_reader_t reader;
    tlv_field_t field;
    tlv_reader_init(&reader, f->payload, f->hdr.length);
    while (tlv_reader_next(&reader, &field)) {
        if (field.tag == TAG_BASE_NICK) {
            if (count == MAX_BUDDIES)
                break;
            size_t length = field.len < CLI_NICK_MAX ? field.len : CLI_NICK_MAX - 1;
            memcpy(entries[count++].account, field.val, length);
            continue;
        }
        if (!count)
            continue;
        cli_buddy_t *b = &entries[count - 1];
        if (field.tag == TAG_NICK) {
            size_t length = field.len < CLI_NICK_MAX ? field.len : CLI_NICK_MAX - 1;
            memcpy(b->nick, field.val, length);
        } else if (field.tag == TAG_AWAY_MSG) {
            size_t length = field.len < sizeof(b->away_msg) ? field.len : sizeof(b->away_msg) - 1;
            memcpy(b->away_msg, field.val, length);
        } else if (field.tag == TAG_USER_ID && field.len == 4) {
            uint32_t value;
            memcpy(&value, field.val, 4);
            b->user_id = ntohl(value);
        } else if (field.tag == TAG_PRESENCE && field.len == 1) {
            b->presence = field.val[0] <= 2 ? field.val[0] : 0;
        }
    }
    if (f->hdr.type == SMSG_BUDDY_LIST) {
        memcpy(m->buddies, entries, sizeof(entries));
        m->buddy_count = count;
    } else if (count == 1) {
        cli_buddy_t *incoming = &entries[0];
        for (int i = 0; i < m->buddy_count; i++) {
            cli_buddy_t *saved = &m->buddies[i];
            if (strcasecmp(saved->account, incoming->account))
                continue;
            bool changed = !!saved->presence != !!incoming->presence;
            *saved = *incoming;
            if (incoming->user_id == m->last_dm_uid)
                snprintf(m->last_dm_nick, sizeof(m->last_dm_nick), "%s", incoming->nick);
            for (int j = 0; j < m->dm_count; j++)
                if (incoming->presence && m->dms[j].peer_uid == incoming->user_id)
                    snprintf(m->dms[j].peer_nick, sizeof(m->dms[j].peer_nick), "%s",
                             incoming->nick);
            if (changed && !model_is_ignored(m, incoming->account) &&
                !model_is_ignored(m, incoming->nick)) {
                char status[128];
                snprintf(status, sizeof(status), "%s signed %s", incoming->account,
                         incoming->presence ? "on" : "off");
                model_set_status(m, status, 6);
                sound_play(incoming->presence ? SND_BUDDY_ONLINE : SND_BUDDY_OFFLINE);
            }
            break;
        }
    }
    ui->dirty = true;
}

static bool buddies_available(cli_model_t *m) {
    if (!(m->server_caps & CAP_BUDDIES)) {
        model_set_status(m, "This server does not support buddy lists", 5);
        return false;
    }
    if (!m->logged_in) {
        model_set_status(m, "Log in first to use your persistent buddy list", 5);
        return false;
    }
    return true;
}

static void open_buddies(cli_model_t *m, client_conn_t *cc, ui_state_t *ui) {
    if (buddies_available(m)) {
        ui->show_buddies_modal = true;
        client_send_buddy(cc, CMSG_BUDDY_LIST, NULL);
    }
    ui->dirty = true;
}

static void open_profile(cli_model_t *m, client_conn_t *cc, ui_state_t *ui, bool refresh) {
    if (!(m->server_caps & CAP_PROFILES)) {
        model_set_status(m, "This server does not support profiles", 5);
        ui->dirty = true;
        return;
    }
    char account[33];
    snprintf(account, sizeof(account), "%s",
             refresh ? ui->profile.account : ui->profile_target_account);
    uint32_t uid = refresh ? 0 : ui->profile_target_uid;
    char nick[33];
    snprintf(nick, sizeof(nick), "%s", refresh ? "" : ui->profile_target_nick);
    ui->show_buddies_modal = false;
    ui->show_profile = ui->profile_loading = ui->dirty = true;
    ui->profile_editing = ui->profile_saving = ui->profile_discard = false;
    ui->profile_focus = ui->profile_offset = 0;
    ui->profile_error[0] = '\0';
    memset(&ui->profile, 0, sizeof(ui->profile));
    explicit_bzero(&ui->profile_draft, sizeof(ui->profile_draft));
    if (++ui->profile_request == 0)
        ui->profile_request++;
    if (client_get_profile(cc, ui->profile_request, uid, account, nick) < 0) {
        ui->profile_loading = false;
        snprintf(ui->profile_error, sizeof(ui->profile_error), "Could not request profile");
    }
}

static void server_text(cli_model_t *m, const char *text) {
    const char *room = m->active_room >= 0 ? m->rooms[m->active_room].name : "general";
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", text);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        model_push_system_msg(m, room, line, (uint64_t)time(NULL) * 1000, MSG_SYSTEM);
}

static void enter_chat(frame_ctx_t *ctx) {
    cli_model_t *m = ctx->model;
    client_conn_t *cc = ctx->conn;
    ctx->ui->show_auth = false;
    ctx->ui->auth_pending = false;
    ui_auth_clear_secrets(ctx->ui);
    m->status = CLI_ONLINE;
    sound_play(SND_WELCOME);
    /* clear any stale history before auto-join */
    model_clear_room_msgs(m, "general");
    /* push ASCII art MOTD lines as system messages into #general */
    if (m->motd[0]) {
        uint64_t motd_ts = (uint64_t)time(NULL) * 1000;
        char motd_copy[2048];
        snprintf(motd_copy, sizeof(motd_copy), "%.2047s", m->motd);
        char *saveptr = NULL;
        char *line = strtok_r(motd_copy, "\n", &saveptr);
        while (line) {
            model_push_system_msg(m, "general", line, motd_ts, MSG_SYSTEM);
            motd_ts += 1; /* keep them in order */
            line = strtok_r(NULL, "\n", &saveptr);
        }
    }
    /* fetch room list and auto-join default room */
    client_send_list_rooms(cc);
    client_send_join(cc, "general");
    /* optimistically track the join in the model */
    cli_room_t *gr = model_get_or_create_room(m, "general");
    if (gr) {
        gr->joined = true;
        m->active_room = (int)(gr - m->rooms);
        m->active_dm = -1;
    }
    client_send_list_users(cc, "general");
    ctx->ui->nav_cursor = (m->active_room >= 0) ? m->active_room : 0;
    model_set_status(m, "Connected", 8);
    if (m->server_caps & CAP_POLLS)
        client_send_poll(cc, 0, 0);
}

static void on_frame(const frame_t *f, void *ctx_ptr) {
    frame_ctx_t *ctx = ctx_ptr;
    cli_model_t *m = ctx->model;
    client_conn_t *cc = ctx->conn;
    const uint8_t *p = f->payload;
    uint32_t pl = f->hdr.length;

    switch ((server_msg_t)f->hdr.type) {

        case SMSG_WELCOME: {
            uint32_t sid = 0;
            tlv_read_u32(p, pl, TAG_SESSION_ID, &sid);
            tlv_read_str(p, pl, TAG_SERVER_NAME, m->server_name, sizeof(m->server_name));
            tlv_read_str(p, pl, TAG_MOTD, m->motd, sizeof(m->motd));
            tlv_read_str(p, pl, TAG_BANNER, m->banner, sizeof(m->banner));
            m->session_id = sid;
            tlv_read_u32(p, pl, TAG_CAPS, &m->server_caps);
            m->user_id = sid; /* our own UID for self-message detection */
            m->status = CLI_SIGNIN;
            ctx->ui->show_auth = true;
            ctx->ui->auth_pending = false;
            model_set_status(m, "Create an account or log in to chat", 999);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ERROR: {
            uint16_t code = 0;
            char msg[256] = {0};
            tlv_read_u16(p, pl, TAG_ERROR_CODE, &code);
            tlv_read_str(p, pl, TAG_ERROR_MSG, msg, sizeof(msg));
            uint32_t request = 0;
            if (tlv_read_u32(p, pl, TAG_REQUEST_ID, &request)) {
                if (ctx->ui->show_profile && request == ctx->ui->profile_request) {
                    ctx->ui->profile_loading = ctx->ui->profile_saving = false;
                    snprintf(ctx->ui->profile_error, sizeof(ctx->ui->profile_error), "%.191s", msg);
                }
            }
            if (ctx->ui->show_auth) {
                ctx->ui->auth_pending = false;
                snprintf(ctx->ui->auth_error, sizeof(ctx->ui->auth_error), "%.191s", msg);
                ui_auth_clear_secrets(ctx->ui);
                ctx->ui->auth_focus = 1;
            }
            ctx->ui->poll_pending = ctx->ui->poll_requested = false;
            model_set_status(m, msg, 5);
            sound_play(SND_ERROR);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ACCOUNT_FLAGS: {
            uint8_t admin = 0, muted = 0;
            tlv_read_u8(p, pl, TAG_IS_ADMIN, &admin);
            tlv_read_u8(p, pl, TAG_MUTED, &muted);
            m->is_admin = admin != 0;
            m->is_muted = muted != 0;
            if (m->is_muted)
                model_set_status(m, "Your account is muted", 999);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_SERVER_NOTICE: {
            char scope[33] = {0}, text[2048] = {0}, actor[33] = {0};
            tlv_read_str(p, pl, TAG_SCOPE, scope, sizeof(scope));
            tlv_read_str(p, pl, TAG_TEXT, text, sizeof(text));
            tlv_read_str(p, pl, TAG_NICK, actor, sizeof(actor));
            char message[2200];
            if (!strcmp(scope, "banner")) {
                snprintf(m->banner, sizeof(m->banner), "%.512s", text);
                snprintf(message, sizeof(message), "[Server banner / %s] %s", actor,
                         text[0] ? text : "cleared");
                sound_play(SND_NOTICE);
            } else if (!strcmp(scope, "motd")) {
                snprintf(m->motd, sizeof(m->motd), "%s", text);
                snprintf(message, sizeof(message),
                         "[Server / %s] MOTD updated. Use /motd to read it.", actor);
                sound_play(SND_NOTICE);
            } else
                snprintf(message, sizeof(message), "[Admin / %s] %s", actor, text);
            server_text(m, message);
            model_set_status(m, message, 10);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_POLL: {
            poll_info_t poll = {0};
            tlv_read_u64(p, pl, TAG_POLL_ID, &poll.id);
            tlv_read_str(p, pl, TAG_TEXT, poll.question, sizeof(poll.question));
            uint8_t closed = 0;
            tlv_read_u8(p, pl, TAG_CLOSED, &closed);
            poll.closed = closed != 0;
            bool personal = tlv_read_u32(p, pl, TAG_VOTE, &poll.my_vote);
            if (!personal && poll.id == m->poll.id)
                poll.my_vote = m->poll.my_vote;
            tlv_reader_t reader;
            tlv_field_t field;
            tlv_reader_init(&reader, p, pl);
            bool valid = true;
            while (tlv_reader_next(&reader, &field)) {
                if (field.tag == TAG_OPTION) {
                    if (poll.option_count == POLL_OPTIONS_MAX || !field.len ||
                        field.len >= sizeof(poll.options[0]) ||
                        !proto_text_valid(field.val, field.len)) {
                        valid = false;
                        break;
                    }
                    memcpy(poll.options[poll.option_count++], field.val, field.len);
                } else if (field.tag == TAG_VOTES && poll.option_count && field.len == 4) {
                    uint32_t count;
                    memcpy(&count, field.val, 4);
                    poll.votes[poll.option_count - 1] = ntohl(count);
                }
            }
            if (!valid || (poll.id && poll.option_count < 2))
                break;
            bool just_closed = poll.id && poll.id == m->poll.id && poll.closed && !m->poll.closed;
            m->poll = poll;
            if (just_closed) {
                server_text(m, "[Server] Poll closed. F8 shows the final results.");
                model_set_status(m, "Poll closed: F8 for final results", 10);
                sound_play(SND_NOTICE);
            }
            if (personal && ctx->ui->poll_requested) {
                ctx->ui->poll_requested = false;
                if (poll.id) {
                    server_text(m, poll.question);
                    for (int i = 0; i < poll.option_count; i++) {
                        char option[160];
                        snprintf(option, sizeof(option), "%d. [%u votes] %s", i + 1, poll.votes[i],
                                 poll.options[i]);
                        server_text(m, option);
                    }
                }
            }
            if (personal)
                ctx->ui->poll_pending = false;
            char scope[16] = {0};
            tlv_read_str(p, pl, TAG_SCOPE, scope, sizeof(scope));
            if (!strcmp(scope, "new")) {
                server_text(m, "[Server] A new poll is open. F8 or /poll to vote.");
                model_set_status(m, "New server poll: F8 to vote", 10);
                sound_play(SND_NOTICE);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_PROFILE:
        case SMSG_PROFILE_SAVED: {
            ui_state_t *ui = ctx->ui;
            uint32_t request = 0;
            tlv_read_u32(p, pl, TAG_REQUEST_ID, &request);
            if (!ui->show_profile || request != ui->profile_request)
                break;
            profile_info_t info = {0};
            if (profile_fields_read(p, pl, &info.fields) < 0) {
                ui->profile_loading = ui->profile_saving = false;
                snprintf(ui->profile_error, sizeof(ui->profile_error), "Invalid profile response");
                ui->dirty = true;
                break;
            }
            tlv_read_str(p, pl, TAG_BASE_NICK, info.account, sizeof(info.account));
            tlv_read_str(p, pl, TAG_NICK, info.nick, sizeof(info.nick));
            tlv_read_str(p, pl, TAG_AWAY_MSG, info.away_msg, sizeof(info.away_msg));
            tlv_read_u32(p, pl, TAG_USER_ID, &info.user_id);
            tlv_read_u64(p, pl, TAG_MEMBER_SINCE, &info.member_since);
            tlv_read_u8(p, pl, TAG_PRESENCE, &info.presence);
            uint8_t self = 0;
            tlv_read_u8(p, pl, TAG_IS_SELF, &self);
            info.is_self = self != 0;
            uint8_t admin = 0;
            tlv_read_u8(p, pl, TAG_IS_ADMIN, &admin);
            info.is_admin = admin != 0;
            info.is_account = true;
            ui->profile = info;
            ui->profile_loading = ui->profile_saving = ui->profile_editing = false;
            explicit_bzero(&ui->profile_draft, sizeof(ui->profile_draft));
            ui->profile_focus = ui->profile_offset = 0;
            snprintf(ui->profile_error, sizeof(ui->profile_error), "%s",
                     f->hdr.type == SMSG_PROFILE_SAVED ? "Profile saved" : "");
            if (f->hdr.type == SMSG_PROFILE_SAVED)
                sound_play(SND_PROFILE_SAVED);
            ui->dirty = true;
            break;
        }

        case SMSG_BUDDY_LIST:
        case SMSG_BUDDY_PRESENCE:
            buddy_frames(f, m, ctx->ui);
            break;

        case SMSG_ROOM_MSG:
        case SMSG_ROOM_HISTORY: {
            char room[65] = {0}, nick[33] = {0}, text[4097] = {0};
            char base_nick[33] = {0};
            uint32_t uid = 0;
            uint64_t ts = 0;
            tlv_read_str(p, pl, TAG_ROOM, room, sizeof(room));
            tlv_read_str(p, pl, TAG_NICK, nick, sizeof(nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            tlv_read_str(p, pl, TAG_TEXT, text, sizeof(text));
            tlv_read_u64(p, pl, TAG_TIMESTAMP, &ts);
            tlv_read_str(p, pl, TAG_BASE_NICK, base_nick, sizeof(base_nick));
            if (!ts)
                ts = (uint64_t)time(NULL) * 1000;
            bool self = f->hdr.type != SMSG_ROOM_HISTORY && uid == m->user_id;
            /* use our own base_nick for self-echo messages */
            const char *bn = self ? m->base_nick : base_nick;
            /* Detect CTCP ACTION:  \x01ACTION <text>\x01 */
            if (text[0] == '\x01' && strncmp(text + 1, "ACTION ", 7) == 0) {
                /* strip wrapper: cut leading \x01ACTION  and trailing \x01 */
                char action_text[4097];
                size_t src_len = strlen(text);
                size_t body_start = 8; /* 1 + 7 */
                size_t body_end =
                    (src_len > 0 && text[src_len - 1] == '\x01') ? src_len - 1 : src_len;
                size_t body_len = (body_end > body_start) ? body_end - body_start : 0;
                if (body_len >= sizeof(action_text))
                    body_len = sizeof(action_text) - 1;
                memcpy(action_text, text + body_start, body_len);
                action_text[body_len] = '\0';
                model_push_room_msg_typed(m, room, uid, nick, bn, action_text, ts, self, MSG_ACTION,
                                          m->nick);
            } else {
                model_push_room_msg(m, room, uid, nick, bn, text, ts, self, m->nick);
            }
            cli_room_t *received_room = model_find_room(m, room);
            if (received_room && received_room->msgs_len)
                received_room
                    ->msgs[(received_room->msgs_head + received_room->msgs_cap - 1) %
                           received_room->msgs_cap]
                    .is_history = f->hdr.type == SMSG_ROOM_HISTORY;
            /* nick-mention highlight: sound + bell */
            if (f->hdr.type == SMSG_ROOM_MSG && !self && m->nick[0] && !model_is_ignored(m, nick)) {
                const char *check = (text[0] == '\x01') ? text + 8 : text;
                if (strcasestr(check, m->nick)) {
                    sound_play(SND_MENTION);
                }
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_DM: {
            char sender_nick[33] = {0}, peer_nick[33] = {0}, text[4097] = {0};
            uint32_t sender_uid = 0, peer_uid2 = 0;
            uint64_t ts = 0;
            tlv_read_str(p, pl, TAG_NICK, sender_nick, sizeof(sender_nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &sender_uid);
            tlv_read_str(p, pl, TAG_PEER_NICK, peer_nick, sizeof(peer_nick));
            tlv_read_u32(p, pl, TAG_PEER_UID, &peer_uid2);
            tlv_read_str(p, pl, TAG_TEXT, text, sizeof(text));
            tlv_read_u64(p, pl, TAG_TIMESTAMP, &ts);
            if (!ts)
                ts = (uint64_t)time(NULL) * 1000;
            bool self = (sender_uid == m->user_id);
            /* Both sender and receiver now know who the other party is:
             *   self==true  (echo):  peer = peer_nick / peer_uid2
             *   self==false (inbound): peer = sender_nick / sender_uid */
            uint32_t conv_uid = self ? peer_uid2 : sender_uid;
            const char *conv_nick = self ? peer_nick : sender_nick;
            if (!self && model_is_ignored(m, sender_nick))
                break;
            if (!self) {
                snprintf(m->last_dm_nick, sizeof(m->last_dm_nick), "%s", sender_nick);
                m->last_dm_uid = sender_uid;
            }
            /* Check if this is the first message in this DM conversation */
            bool is_first_msg = false;
            if (!self) {
                cli_dm_t *existing_dm = NULL;
                for (int i = 0; i < m->dm_count; i++) {
                    if (m->dms[i].peer_uid == conv_uid) {
                        existing_dm = &m->dms[i];
                        break;
                    }
                }
                is_first_msg = (!existing_dm || existing_dm->msgs_len == 0);
            }
            model_push_dm_msg(m, conv_uid, conv_nick, text, ts, self);
            if (!self)
                sound_play(is_first_msg ? SND_DM : SND_IM);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_USER_JOINED: {
            char room[65] = {0}, nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_ROOM, room, sizeof(room));
            tlv_read_str(p, pl, TAG_NICK, nick, sizeof(nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            char info[128];
            snprintf(info, sizeof(info), "%s joined #%s", nick, room);
            model_push_system_msg(m, room, info, (uint64_t)time(NULL) * 1000, MSG_JOIN);
            /* BuddyIn sound for others joining (suppress for our own join echo) */
            if (uid != m->user_id)
                sound_play(SND_BUDDY_IN);
            /* refresh user list if this is the active room */
            if (m->active_room >= 0 && m->active_room < m->room_count &&
                strcmp(m->rooms[m->active_room].name, room) == 0) {
                client_send_list_users(cc, room);
                *ctx->last_users_refresh = time(NULL);
            }
            /* if the room is new to us, refresh the room list too */
            if (!model_find_room(m, room)) {
                client_send_list_rooms(cc);
                *ctx->last_rooms_refresh = time(NULL);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_USER_LEFT: {
            char room[65] = {0}, nick[33] = {0};
            tlv_read_str(p, pl, TAG_ROOM, room, sizeof(room));
            tlv_read_str(p, pl, TAG_NICK, nick, sizeof(nick));
            char info[128];
            snprintf(info, sizeof(info), "%s left #%s", nick, room);
            model_push_system_msg(m, room, info, (uint64_t)time(NULL) * 1000, MSG_PART);
            sound_play(SND_BUDDY_OUT);
            /* refresh user list if this is the active room */
            if (m->active_room >= 0 && m->active_room < m->room_count &&
                strcmp(m->rooms[m->active_room].name, room) == 0) {
                client_send_list_users(cc, room);
                *ctx->last_users_refresh = time(NULL);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_USER_AWAY: {
            char nick[33] = {0}, away_msg[256] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK, nick, sizeof(nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            tlv_read_str(p, pl, TAG_AWAY_MSG, away_msg, sizeof(away_msg));
            bool going_away = (away_msg[0] != '\0');
            /* update our own model when the server echoes our own away state */
            if (uid == m->user_id) {
                m->is_away = going_away;
                snprintf(m->away_msg, sizeof(m->away_msg), "%s", away_msg);
            }
            /* Room tags scope the notice; older servers send no room tags. */
            bool scoped = tlv_find(p, pl, TAG_ROOM, NULL) != NULL;
            char info[320];
            if (going_away)
                snprintf(info, sizeof(info), "%s is now away: %s", nick, away_msg);
            else
                snprintf(info, sizeof(info), "%s is back", nick);
            for (int i = 0; i < m->room_count; i++) {
                bool affected = !scoped;
                if (scoped) {
                    tlv_reader_t reader;
                    tlv_field_t field;
                    tlv_reader_init(&reader, p, pl);
                    while (tlv_reader_next(&reader, &field))
                        if (field.tag == TAG_ROOM && field.len == strlen(m->rooms[i].name) &&
                            !memcmp(field.val, m->rooms[i].name, field.len)) {
                            affected = true;
                            break;
                        }
                }
                /* only post in rooms we are actually a member of */
                if (affected && m->rooms[i].active && m->rooms[i].joined)
                    model_push_system_msg(m, m->rooms[i].name, info, (uint64_t)time(NULL) * 1000,
                                          MSG_SYSTEM);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_CREATED: {
            /* another client created a room -  add it to our list */
            char name[65] = {0};
            uint32_t owner = 0;
            tlv_read_str(p, pl, TAG_ROOM, name, sizeof(name));
            tlv_read_u32(p, pl, TAG_OWNER_UID, &owner);
            cli_room_t *nr = model_get_or_create_room(m, name);
            if (nr)
                nr->owner_uid = owner;
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_DELETED: {
            char name[65] = {0};
            tlv_read_str(p, pl, TAG_ROOM, name, sizeof(name));
            model_remove_room(m, name);
            ctx->ui->nav_cursor = m->active_room >= 0 ? m->active_room
                                  : m->active_dm >= 0 ? m->room_count + m->active_dm
                                                      : -1;
            ui_clamp_navigation(ctx->ui, m);
            char sb[80];
            snprintf(sb, sizeof(sb), "Room #%.60s was deleted", name);
            model_set_status(m, sb, 6);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_LIST: {
            /* iterate TAG_ROOM / TAG_OWNER_UID pairs */
            tlv_reader_t r;
            tlv_field_t fld;
            tlv_reader_init(&r, p, pl);
            char pending_name[65] = {0};
            while (tlv_reader_next(&r, &fld)) {
                if (fld.tag == TAG_ROOM) {
                    size_t l = fld.len < 64 ? fld.len : 64;
                    memset(pending_name, 0, sizeof(pending_name));
                    memcpy(pending_name, fld.val, l);
                    model_get_or_create_room(m, pending_name);
                } else if (fld.tag == TAG_OWNER_UID && fld.len >= 4 && pending_name[0]) {
                    uint32_t ow;
                    memcpy(&ow, fld.val, 4);
                    cli_room_t *rr = model_find_room(m, pending_name);
                    if (rr)
                        rr->owner_uid = ntohl(ow);
                }
            }
            if (m->active_room < 0 && m->active_dm < 0)
                model_select_fallback(m);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_USER_LIST: {
            cli_user_t tmp[CLI_MAX_USERS];
            int count = 0;
            tlv_reader_t r;
            tlv_field_t fld;
            tlv_reader_init(&r, p, pl);
            char pending_nick[33] = {0};
            while (tlv_reader_next(&r, &fld) && count < CLI_MAX_USERS) {
                if (fld.tag == TAG_NICK) {
                    size_t l = fld.len < 32 ? fld.len : 32;
                    memcpy(pending_nick, fld.val, l);
                    pending_nick[l] = '\0';
                } else if (fld.tag == TAG_USER_ID && fld.len >= 4) {
                    uint32_t uid;
                    memcpy(&uid, fld.val, 4);
                    tmp[count].user_id = ntohl(uid);
                    snprintf(tmp[count].nick, sizeof(tmp[count].nick), "%s", pending_nick);
                    count++;
                    memset(pending_nick, 0, sizeof(pending_nick));
                }
            }
            char response_room[CLI_ROOM_MAX] = {0};
            bool scoped = tlv_read_str(p, pl, TAG_ROOM, response_room, sizeof(response_room));
            const char *cur_room = "";
            if (m->active_room >= 0 && m->active_room < m->room_count)
                cur_room = m->rooms[m->active_room].name;
            if (!scoped)
                snprintf(response_room, sizeof(response_room), "%s", cur_room);
            if (!strcmp(response_room, cur_room) && response_room[0])
                model_update_users(m, response_room, tmp, count);
            if (scoped && !response_room[0] && m->pending_query[0]) {
                bool found = false;
                for (int i = 0; i < count; i++) {
                    if (!strcmp(tmp[i].nick, m->pending_query) && tmp[i].user_id != m->user_id) {
                        select_dm(m, ctx->ui, tmp[i].user_id, tmp[i].nick);
                        found = true;
                        break;
                    }
                }
                model_set_status(m, found ? "DM opened" : "That nick is not online", 5);
                m->pending_query[0] = '\0';
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_NICK_CHANGED: {
            char new_nick[33] = {0}, old_nick[33] = {0}, room[65] = {0};
            char base_nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK, new_nick, sizeof(new_nick));
            tlv_read_str(p, pl, TAG_OLD_NICK, old_nick, sizeof(old_nick));
            tlv_read_str(p, pl, TAG_ROOM, room, sizeof(room));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            if (uid && uid == m->last_dm_uid)
                snprintf(m->last_dm_nick, sizeof(m->last_dm_nick), "%s", new_nick);
            tlv_read_str(p, pl, TAG_BASE_NICK, base_nick, sizeof(base_nick));

            if (room[0]) {
                /* room broadcast: other users see the rename */
                char info[128];
                snprintf(info, sizeof(info), "%.32s is now known as %.32s", old_nick, new_nick);
                model_push_system_msg(m, room, info, (uint64_t)time(NULL) * 1000, MSG_SYSTEM);
                /* refresh user list so the sidebar reflects the new name */
                if (m->active_room >= 0 && m->active_room < m->room_count &&
                    strcmp(m->rooms[m->active_room].name, room) == 0) {
                    client_send_list_users(cc, room);
                    *ctx->last_users_refresh = time(NULL);
                }
                /* keep DM peer_nick in sync: if we have an open DM with the
                 * user who just renamed, update the stored nick so the
                 * sidebar and topic bar show the new name immediately. */
                if (uid) {
                    for (int i = 0; i < m->dm_count; i++) {
                        if (m->dms[i].peer_uid == uid)
                            snprintf(m->dms[i].peer_nick, sizeof(m->dms[i].peer_nick), "%s",
                                     new_nick);
                    }
                }
            } else {
                /* private confirmation: we renamed ourselves */
                snprintf(m->nick, sizeof(m->nick), "%s", new_nick);
                /* base_nick stays unchanged -  it's the account/original name */
                /* push the rename notice into all our joined rooms */
                char info[128];
                snprintf(info, sizeof(info), "%.32s is now known as %.32s", old_nick, new_nick);
                for (int i = 0; i < m->room_count; i++) {
                    if (m->rooms[i].active && m->rooms[i].joined)
                        model_push_system_msg(m, m->rooms[i].name, info,
                                              (uint64_t)time(NULL) * 1000, MSG_SYSTEM);
                }
                char sb[80];
                snprintf(sb, sizeof(sb), "You are now known as %.32s", new_nick);
                model_set_status(m, sb, 6);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_REGISTER_OK: {
            m->logged_in = true;
            char new_nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK, new_nick, sizeof(new_nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            snprintf(m->nick, sizeof(m->nick), "%s", new_nick);
            snprintf(m->base_nick, sizeof(m->base_nick), "%s", new_nick);
            if (uid)
                m->user_id = uid;
            char sb[80];
            snprintf(sb, sizeof(sb), "Registered & logged in as %.32s", new_nick);
            model_set_status(m, sb, 8);
            enter_chat(ctx);
            client_send_list_rooms(cc);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_LOGIN_OK: {
            m->logged_in = true;
            char new_nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK, new_nick, sizeof(new_nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            snprintf(m->nick, sizeof(m->nick), "%s", new_nick);
            snprintf(m->base_nick, sizeof(m->base_nick), "%s", new_nick);
            if (uid)
                m->user_id = uid;
            char sb[80];
            snprintf(sb, sizeof(sb), "Logged in as %.32s", new_nick);
            model_set_status(m, sb, 8);
            enter_chat(ctx);
            client_send_list_rooms(cc);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_LOGOUT_OK: {
            uint32_t uid = 0, caps = m->server_caps;
            char server[128], motd[2048], banner[513];
            snprintf(banner, sizeof(banner), "%s", m->banner);
            snprintf(server, sizeof(server), "%s", m->server_name);
            snprintf(motd, sizeof(motd), "%s", m->motd);
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            model_free(m);
            model_init(m);
            m->status = CLI_SIGNIN;
            m->server_caps = caps;
            m->user_id = m->session_id = uid;
            snprintf(m->server_name, sizeof(m->server_name), "%s", server);
            snprintf(m->motd, sizeof(m->motd), "%s", motd);
            snprintf(m->banner, sizeof(m->banner), "%s", banner);
            ui_state_t *ui = ctx->ui;
            for (int i = 0; i < ui->hist_count; i++) {
                explicit_bzero(ui->hist[i], strlen(ui->hist[i]));
                free(ui->hist[i]);
                ui->hist[i] = NULL;
            }
            ui->hist_count = 0;
            ui->hist_idx = -1;
            explicit_bzero(ui->input_buf, sizeof(ui->input_buf));
            ui->input_len = ui->input_cursor = ui->input_offset = 0;
            ui->show_buddies_modal = ui->show_help_modal = ui->show_emoji_modal = false;
            ui->show_auth = true;
            ui->auth_pending = ui->auth_register = false;
            ui->show_profile = ui->profile_editing = ui->profile_saving = ui->profile_loading =
                false;
            ui->show_poll = ui->poll_pending = ui->poll_requested = false;
            ui->profile_discard = false;
            memset(&ui->profile, 0, sizeof(ui->profile));
            explicit_bzero(&ui->profile_draft, sizeof(ui->profile_draft));
            ui->auth_error[0] = '\0';
            ui->auth_focus = 0;
            ui_auth_clear_secrets(ui);
            ui->dirty = true;
            break;
        }

        case SMSG_ROOM_TOPIC: {
            char room[65] = {0}, topic[257] = {0}, setter[33] = {0};
            tlv_read_str(p, pl, TAG_ROOM, room, sizeof(room));
            tlv_read_str(p, pl, TAG_TOPIC, topic, sizeof(topic));
            tlv_read_str(p, pl, TAG_NICK, setter, sizeof(setter));
            cli_room_t *tr = model_find_room(m, room);
            if (tr) {
                snprintf(tr->topic, sizeof(tr->topic), "%s", topic);
                snprintf(tr->topic_set_by, sizeof(tr->topic_set_by), "%s", setter);
            }
            /* emit a system message only when someone actively set the topic */
            if (setter[0] && topic[0]) {
                char info[320];
                snprintf(info, sizeof(info), "%.32s set the topic: %.200s", setter, topic);
                model_push_system_msg(m, room, info, (uint64_t)time(NULL) * 1000, MSG_SYSTEM);
                if (strcasecmp(setter, m->nick) != 0 && !model_is_ignored(m, setter))
                    sound_play(SND_TOPIC);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_PING:
            client_send_pong(cc);
            break;

        case SMSG_PONG:
            break;

        case SMSG_TYPING_NOTIFY: {
            char room[65] = {0}, nick[33] = {0};
            tlv_read_str(p, pl, TAG_ROOM, room, sizeof(room));
            tlv_read_str(p, pl, TAG_NICK, nick, sizeof(nick));
            cli_room_t *tr = model_find_room(m, room);
            if (tr) {
                snprintf(tr->typing_nick, sizeof(tr->typing_nick), "%s", nick);
                tr->typing_until = time(NULL) + 3; /* show for 3 seconds */
                ctx->ui->dirty = true;
            }
            break;
        }

        default:
            break;
    }
}

/* Command dispatcher */

static void close_active_chat(cli_model_t *m, client_conn_t *cc, ui_state_t *ui) {
    if (m->active_room >= 0 && m->active_room < m->room_count) {
        cli_room_t *room = &m->rooms[m->active_room];
        if (room->joined)
            client_send_leave(cc, room->name);
        room->joined = false;
        model_select_fallback(m);
        model_set_status(m, "Room left. Select it again to rejoin.", 4);
    } else if (model_close_dm(m, m->active_dm)) {
        model_set_status(m, "DM conversation closed", 3);
    } else
        model_set_status(m, "No active chat to close", 3);
    ui->nav_cursor = m->active_room >= 0 ? m->active_room
                     : m->active_dm >= 0 ? m->room_count + m->active_dm
                                         : -1;
    ui_clamp_navigation(ui, m);
    ui->dirty = true;
}

static void next_unread(cli_model_t *m, client_conn_t *cc, ui_state_t *ui) {
    int total = m->room_count + m->dm_count;
    int current = m->active_room >= 0 ? m->active_room : m->room_count + m->active_dm;
    for (int step = 1; step <= total; step++) {
        int index = (current + step + total) % total;
        if (index < m->room_count) {
            cli_room_t *room = &m->rooms[index];
            if (!room->joined || !room->unread)
                continue;
            m->active_room = index;
            m->active_dm = -1;
            room->scroll_offset = room->unread = 0;
            m->user_count = 0;
            client_send_list_users(cc, room->name);
        } else {
            cli_dm_t *dm = &m->dms[index - m->room_count];
            if (!dm->unread)
                continue;
            select_dm(m, ui, dm->peer_uid, dm->peer_nick);
        }
        ui->nav_cursor = index;
        ui->dirty = true;
        return;
    }
    model_set_status(m, "No unread conversations", 3);
    ui->dirty = true;
}

static void admin_command(const char *command, cli_model_t *m, client_conn_t *cc) {
    if (!*command || !strcmp(command, "help")) {
        server_text(m, "Admin: /admin kick|ban|unban|mute|unmute <account> [reason]");
        server_text(m, "Roles: /admin grant|revoke <account> (the last admin is protected)");
        server_text(m, "Messages: /admin banner <text or clear>; /admin motd <text or clear>");
        server_text(m, "MOTD: use | between lines. /motd and /banner read the current messages.");
        server_text(
            m, "Polls: /admin poll Question | Choice one | Choice two [| more]; /admin pollclose");
        return;
    }
    if (!m->is_admin) {
        model_set_status(m, "This command requires a server admin", 5);
        return;
    }
    char copy[2048];
    if (strlen(command) >= sizeof(copy)) {
        model_set_status(m, "Admin command is too long", 5);
        return;
    }
    snprintf(copy, sizeof(copy), "%s", command);
    char *body = strchr(copy, ' ');
    if (body) {
        *body++ = '\0';
        while (*body == ' ')
            body++;
    } else
        body = copy + strlen(copy);
    char *account = NULL;
    bool setting = !strcmp(copy, "banner") || !strcmp(copy, "motd") || !strcmp(copy, "poll");
    if (!setting && strcmp(copy, "pollclose")) {
        account = body;
        body = strchr(account, ' ');
        if (body) {
            *body++ = '\0';
            while (*body == ' ')
                body++;
        } else
            body = account + strlen(account);
        if (!*account || strlen(account) > MAX_NICK_LEN) {
            model_set_status(m, "Usage: /admin <action> <account> [reason]", 5);
            return;
        }
    } else if (setting && !*body) {
        model_set_status(m, "Provide text, or use clear to remove a server message", 5);
        return;
    }
    if (client_send_admin(cc, copy, account, body) < 0)
        model_set_status(m, "Could not send admin command", 5);
}

static void handle_command(const char *line, cli_model_t *m, client_conn_t *cc, ui_state_t *ui) {
    /* skip leading '/' */
    const char *cmd = line + 1;

    static const struct {
        const char *alias, *command;
    } aliases[] = {{"msg", "dm"},      {"part", "leave"}, {"list", "rooms"},
                   {"names", "users"}, {"r", "reply"},    {"j", "join"}};

    char expanded[CLI_MSG_MAX];
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        size_t length = strlen(aliases[i].alias);
        if (!strncmp(cmd, aliases[i].alias, length) && (!cmd[length] || cmd[length] == ' ')) {
            int size =
                snprintf(expanded, sizeof(expanded), "%s%s", aliases[i].command, cmd + length);
            if (size < 0 || size >= (int)sizeof(expanded)) {
                model_set_status(m, "Command is too long", 4);
                ui->dirty = true;
                return;
            }
            cmd = expanded;
            break;
        }
    }

    if (!strcmp(cmd, "admin") || !strncmp(cmd, "admin ", 6)) {
        admin_command(cmd[5] ? cmd + 6 : "", m, cc);
    } else if (!strcmp(cmd, "poll")) {
        if (m->server_caps & CAP_POLLS) {
            ui->show_poll = ui->poll_requested = true;
            ui->poll_question_row = 0;
            client_send_poll(cc, 0, 0);
        } else
            model_set_status(m, "This server does not support polls", 5);
    } else if (!strncmp(cmd, "vote ", 5)) {
        if (strlen(cmd + 5) != 1 || cmd[5] < '1' || cmd[5] > '4' || !m->poll.id || m->poll.closed)
            model_set_status(m, "Open /poll, then use /vote <1-4> on an open poll", 5);
        else
            client_send_poll(cc, m->poll.id, (uint32_t)(cmd[5] - '0'));
    } else if (!strcmp(cmd, "motd")) {
        server_text(m, m->motd[0] ? m->motd : "The server MOTD is empty.");
    } else if (!strcmp(cmd, "banner")) {
        server_text(m, m->banner[0] ? m->banner : "No server banner is set.");
    } else if (!strcmp(cmd, "profile") || !strncmp(cmd, "profile ", 8) ||
               !strncmp(cmd, "whois ", 6)) {
        const char *name = !strncmp(cmd, "whois ", 6) ? cmd + 6 : cmd[7] ? cmd + 8 : "";
        while (*name == ' ')
            name++;
        if (strlen(name) > MAX_NICK_LEN + (*name == '@') || strchr(name, ' '))
            model_set_status(m, "Usage: /profile [nick or @account]", 5);
        else {
            ui_profile_target(ui, 0, *name == '@' ? NULL : name, *name == '@' ? name + 1 : NULL);
            open_profile(m, cc, ui, false);
        }
    } else if (!strcmp(cmd, "buddies") || !strcmp(cmd, "buddy")) {
        open_buddies(m, cc, ui);
    } else if (!strncmp(cmd, "buddy ", 6)) {
        if (buddies_available(m)) {
            const char *arg = cmd + 6;
            bool add = !strncmp(arg, "add ", 4);
            bool remove = !strncmp(arg, "remove ", 7);
            const char *name = add ? arg + 4 : remove ? arg + 7 : "";
            if (!*name || strlen(name) >= CLI_NICK_MAX || strchr(name, ' '))
                model_set_status(m, "Usage: /buddy add|remove <account>", 5);
            else {
                client_send_buddy(cc, add ? CMSG_BUDDY_ADD : CMSG_BUDDY_REMOVE, name);
                model_set_status(m, "Updating buddy list...", 4);
            }
        }
    } else if (!strcmp(cmd, "query") || !strncmp(cmd, "query ", 6)) {
        const char *nick = cmd[5] ? cmd + 6 : "";
        while (*nick == ' ')
            nick++;
        if (!*nick || strlen(nick) >= CLI_NICK_MAX || strchr(nick, ' '))
            model_set_status(m, "Usage: /query <online-nick>", 4);
        else if (!strcmp(nick, m->nick))
            model_set_status(m, "Choose another user for a DM", 4);
        else {
            snprintf(m->pending_query, sizeof(m->pending_query), "%s", nick);
            client_send_list_users(cc, "");
            model_set_status(m, "Looking up that user...", 4);
        }
    } else if (!strncmp(cmd, "reply ", 6)) {
        if (!m->last_dm_nick[0])
            model_set_status(m, "No incoming DM to reply to yet", 4);
        else {
            client_send_dm_to(cc, m->last_dm_nick, cmd + 6, m->last_dm_uid);
            select_dm(m, ui, m->last_dm_uid, m->last_dm_nick);
        }
    } else if (!strcmp(cmd, "unread")) {
        next_unread(m, cc, ui);
    } else if (!strcmp(cmd, "clear")) {
        if (m->active_room >= 0)
            model_clear_room_msgs(m, m->rooms[m->active_room].name);
        else if (m->active_dm >= 0) {
            cli_dm_t *dm = &m->dms[m->active_dm];
            dm->msgs_len = dm->msgs_head = dm->unread = dm->scroll_offset = 0;
        }
        model_set_status(m, "Local chat buffer cleared", 4);
    } else if (strcmp(cmd, "sound") == 0 || strncmp(cmd, "sound ", 6) == 0) {
        const char *arg = cmd[5] == ' ' ? cmd + 6 : "status";
        if (!strcmp(arg, "on"))
            sound_set_enabled(1);
        else if (!strcmp(arg, "off"))
            sound_set_enabled(0);
        else if (!strcmp(arg, "test"))
            sound_play(SND_MENTION);
        else if (strcmp(arg, "status")) {
            model_set_status(m, "Usage: /sound on|off|test|status", 6);
            ui->dirty = true;
            return;
        }
        model_set_status(m, sound_status(), 8);
    } else if (strncmp(cmd, "me ", 3) == 0) {
        /* /me <action text> -  send an IRC-style action */
        const char *action = cmd + 3;
        if (m->active_room >= 0) {
            const char *room = m->rooms[m->active_room].name;
            client_send_room_action(cc, room, action);
        } else if (m->active_dm >= 0 && m->active_dm < m->dm_count) {
            model_set_status(m, "/me is not supported in direct messages", 4);
        } else {
            model_set_status(m, "Join a room first to use /me", 4);
        }
    } else if (strncmp(cmd, "topic", 5) == 0 && (!cmd[5] || cmd[5] == ' ')) {
        /* /topic [text] -  set or display the current room topic */
        if (m->active_room < 0) {
            model_set_status(m, "Join a room first to use /topic", 3);
        } else if (strlen(cmd) > 6) {
            const char *room = m->rooms[m->active_room].name;
            client_send_set_topic(cc, room, cmd + 6);
        } else {
            const cli_room_t *r = &m->rooms[m->active_room];
            if (r->topic[0]) {
                char sb[300];
                snprintf(sb, sizeof(sb), "Topic: %.256s", r->topic);
                model_set_status(m, sb, 8);
            } else {
                model_set_status(m, "No topic set. Use /topic <text>", 4);
            }
        }
    } else if (strncmp(cmd, "join ", 5) == 0) {
        const char *room = cmd + 5;
        while (*room == ' ')
            room++; /* strip accidental leading spaces */
        if (*room == '#')
            room++;
        if (!*room) {
            model_set_status(m, "Usage: /join <roomname>", 4);
        } else {
            /* Rejoining requests a replay; selecting a joined room keeps its buffer. */
            cli_room_t *existing = model_find_room(m, room);
            if (!existing || !existing->joined) {
                model_clear_room_msgs(m, room);
                client_send_join(cc, room);
            }
            cli_room_t *r = model_get_or_create_room(m, room);
            if (r) {
                r->joined = true;
                m->active_room = (int)(r - m->rooms);
                m->active_dm = -1;
                ui->nav_cursor = m->active_room;
            }
            client_send_list_users(cc, room);
            char sb[80];
            snprintf(sb, sizeof(sb), "Joining #%.60s...", room);
            model_set_status(m, sb, 4);
        }
    } else if (strncmp(cmd, "leave", 5) == 0 && (!cmd[5] || cmd[5] == ' ')) {
        const char *room = (strlen(cmd) > 6) ? cmd + 6 : "";
        if (!*room && m->active_room >= 0)
            room = m->rooms[m->active_room].name;
        if (*room) {
            client_send_leave(cc, room);
            cli_room_t *left = model_find_room(m, room);
            if (left)
                left->joined = false;
            char sb[80];
            snprintf(sb, sizeof(sb), "Left #%.64s", room);
            model_set_status(m, sb, 4);
            if (left && m->active_room >= 0 && left == &m->rooms[m->active_room])
                model_select_fallback(m);
        } else {
            model_set_status(m, "Not in any room", 3);
        }
    } else if (strncmp(cmd, "register ", 9) == 0) {
        /* /register <nick> <pass> */
        const char *rest = cmd + 9;
        char nick[33] = {0};
        const char *sp = strchr(rest, ' ');
        if (sp && sp[1]) {
            size_t l = (size_t)(sp - rest) < 32 ? (size_t)(sp - rest) : 32;
            memcpy(nick, rest, l);
            client_send_register(cc, nick, sp + 1);
            model_set_status(m, "Registering account...", 4);
        } else {
            model_set_status(m, "Usage: /register <nick> <password>", 5);
        }
    } else if (strncmp(cmd, "login ", 6) == 0) {
        /* /login <nick> <pass> */
        const char *rest = cmd + 6;
        char nick[33] = {0};
        const char *sp = strchr(rest, ' ');
        if (sp && sp[1]) {
            size_t l = (size_t)(sp - rest) < 32 ? (size_t)(sp - rest) : 32;
            memcpy(nick, rest, l);
            client_send_login(cc, nick, sp + 1);
            model_set_status(m, "Authenticating...", 4);
        } else {
            model_set_status(m, "Usage: /login <nick> <password>", 5);
        }
    } else if (strcmp(cmd, "logout") == 0) {
        client_send_logout(cc);
        model_set_status(m, "Logging out...", 4);
    } else if (strncmp(cmd, "nick ", 5) == 0) {
        /* /nick <newnick> */
        const char *new_nick = cmd + 5;
        while (*new_nick == ' ')
            new_nick++;
        if (*new_nick) {
            client_send_nick(cc, new_nick);
            model_set_status(m, "Changing nick...", 3);
        } else {
            model_set_status(m, "Usage: /nick <newnick>", 4);
        }
    } else if (strncmp(cmd, "away", 4) == 0 && (!cmd[4] || cmd[4] == ' ')) {
        /* /away [message]  -  set away with optional message */
        const char *msg = (strlen(cmd) > 5) ? cmd + 5 : "away";
        while (*msg == ' ')
            msg++;
        if (!*msg)
            msg = "away";
        client_send_away(cc, msg);
        m->is_away = true;
        snprintf(m->away_msg, sizeof(m->away_msg), "%.255s", msg);
        char sb[300];
        snprintf(sb, sizeof(sb), "Away: %.255s", msg);
        model_set_status(m, sb, 5);
    } else if (strcmp(cmd, "back") == 0) {
        /* /back  -  return from away */
        client_send_away(cc, "");
        m->is_away = false;
        m->away_msg[0] = '\0';
        model_set_status(m, "You are no longer away", 4);
    } else if (strncmp(cmd, "ignore", 6) == 0 && (cmd[6] == ' ' || cmd[6] == '\0')) {
        /* /ignore           -  list ignored nicks
         * /ignore <nick>    -  add nick to ignore list */
        const char *nick = (cmd[6] == ' ') ? cmd + 7 : "";
        while (*nick == ' ')
            nick++;
        if (!*nick) {
            /* list current ignores */
            if (m->ignore_count == 0) {
                model_set_status(m, "Ignore list is empty", 4);
            } else {
                char sb[256] = "Ignoring: ";
                for (int i = 0; i < m->ignore_count; i++) {
                    if (i)
                        strncat(sb, ", ", sizeof(sb) - strlen(sb) - 1);
                    strncat(sb, m->ignore_list[i], sizeof(sb) - strlen(sb) - 1);
                }
                model_set_status(m, sb, 6);
            }
        } else if (m->ignore_count >= 32) {
            model_set_status(m, "Ignore list full (max 32)", 4);
        } else {
            for (int i = 0; i < m->ignore_count; i++) {
                if (strcasecmp(m->ignore_list[i], nick) == 0) {
                    model_set_status(m, "Already ignoring that nick", 3);
                    goto ignore_done;
                }
            }
            snprintf(m->ignore_list[m->ignore_count++], CLI_NICK_MAX, "%.32s", nick);
            char sb[80];
            snprintf(sb, sizeof(sb), "Now ignoring %.32s", nick);
            model_set_status(m, sb, 5);
        ignore_done:;
        }
    } else if (strncmp(cmd, "unignore ", 9) == 0) {
        /* /unignore <nick> */
        const char *nick = cmd + 9;
        while (*nick == ' ')
            nick++;
        bool found = false;
        for (int i = 0; i < m->ignore_count; i++) {
            if (strcasecmp(m->ignore_list[i], nick) == 0) {
                memmove(m->ignore_list[i], m->ignore_list[i + 1],
                        (size_t)(m->ignore_count - i - 1) * CLI_NICK_MAX);
                m->ignore_count--;
                found = true;
                break;
            }
        }
        if (found) {
            char sb[80];
            snprintf(sb, sizeof(sb), "No longer ignoring %.32s", nick);
            model_set_status(m, sb, 5);
        } else {
            model_set_status(m, "Nick not in ignore list", 3);
        }
    } else if (strncmp(cmd, "dm ", 3) == 0) {
        /* /dm <nick> <text> */
        const char *rest = cmd + 3;
        char target[33] = {0};
        const char *sp = strchr(rest, ' ');
        if (sp) {
            size_t l = (size_t)(sp - rest) < 32 ? (size_t)(sp - rest) : 32;
            memcpy(target, rest, l);
            client_send_dm(cc, target, sp + 1);
            char sb[80];
            snprintf(sb, sizeof(sb), "DM sent to @%s", target);
            model_set_status(m, sb, 4);
        } else {
            model_set_status(m, "Usage: /dm <nick> <message>", 4);
        }
    } else if (strncmp(cmd, "delroom", 7) == 0 && (!cmd[7] || cmd[7] == ' ')) {
        /* /delroom [name]  -  delete a room you own */
        const char *room = (strlen(cmd) > 8) ? cmd + 8 : "";
        if (!*room && m->active_room >= 0)
            room = m->rooms[m->active_room].name;
        if (*room) {
            client_send_delete_room(cc, room);
            char sb[80];
            snprintf(sb, sizeof(sb), "Deleting #%.60s...", room);
            model_set_status(m, sb, 4);
        } else {
            model_set_status(m, "Usage: /delroom <name>", 4);
        }
    } else if (strcmp(cmd, "close") == 0) {
        close_active_chat(m, cc, ui);
    } else if (strcmp(cmd, "latest") == 0) {
        if (m->active_room >= 0)
            m->rooms[m->active_room].scroll_offset = 0;
        else if (m->active_dm >= 0)
            m->dms[m->active_dm].scroll_offset = 0;
    } else if (strncmp(cmd, "closedm", 7) == 0 && (!cmd[7] || cmd[7] == ' ')) {
        const char *nick = (strlen(cmd) > 8) ? cmd + 8 : "";
        int index = m->active_dm;
        if (*nick) {
            index = -1;
            for (int i = 0; i < m->dm_count; i++)
                if (!strcmp(m->dms[i].peer_nick, nick)) {
                    index = i;
                    break;
                }
        }
        bool closed = model_close_dm(m, index);
        model_set_status(m, closed ? "DM conversation closed" : "No matching DM to close", 3);
        ui_clamp_navigation(ui, m);
    } else if (strcmp(cmd, "rooms") == 0) {
        client_send_list_rooms(cc);
        model_set_status(m, "Fetching room list...", 3);
    } else if (strncmp(cmd, "users", 5) == 0 && (!cmd[5] || cmd[5] == ' ')) {
        const char *room = (strlen(cmd) > 6) ? cmd + 6 : "";
        if (!*room && m->active_room >= 0)
            room = m->rooms[m->active_room].name;
        client_send_list_users(cc, room);
        model_set_status(m, "Fetching user list...", 3);
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
        g_quit = 1;
    } else if (strcmp(cmd, "help") == 0) {
        ui->show_help_modal = true;
        ui->dirty = true;
    } else if (strcmp(cmd, "emoji") == 0) {
        ui->show_emoji_modal = true;
        ui->dirty = true;
    } else if (strncmp(cmd, "send", 4) == 0) {
        model_set_status(m, "File transfer not yet supported", 4);
    } else {
        model_set_status(m, "Unknown command. Try /help", 3);
    }
    ui->dirty = true;
}

/* Main */

int main(int argc, char **argv) {
    client_config_t cfg;
    int rc = client_config_parse(argc, argv, &cfg);
    if (rc == 1) {
        client_config_usage(argv[0]);
        return 0;
    }
    if (rc == -1) {
        client_config_usage(argv[0]);
        return 1;
    }

    sound_init(NULL);
    if (cfg.sound_test)
        return sound_diagnose() == 0 ? 0 : 1;

    signal(SIGPIPE, SIG_IGN);

    /* connect */
    client_conn_t cc;
    fprintf(stderr, "Connecting to %s:%u...\n", cfg.host, cfg.port);
    if (client_connect(&cc, cfg.host, cfg.port, cfg.use_tls) < 0) {
        fprintf(stderr, "Connection failed: %s:%u\n", cfg.host, cfg.port);
        return 1;
    }

    /* init model */
    cli_model_t model;
    model_init(&model);
    model.status = CLI_CONNECTING;
    snprintf(model.nick, sizeof(model.nick), "%s", cfg.nick);
    snprintf(model.base_nick, sizeof(model.base_nick), "%s", cfg.nick);

    /* send HELLO */
    if (client_send_hello(&cc, cfg.nick) < 0) {
        fprintf(stderr, "Failed to send HELLO\n");
        client_disconnect(&cc);
        return 1;
    }

    /* init UI */
    ui_state_t ui;
    ui_init(&ui);
    ui.show_auth = true;
    snprintf(ui.auth_name, sizeof(ui.auth_name), "%s", cfg.nick);
    ui.auth_cursors[0] = (int)strlen(ui.auth_name);

    signal(SIGWINCH, on_sigwinch);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    time_t last_users_refresh = 0;
    time_t last_typing_sent = 0; /* throttle CMSG_TYPING to once per 2s */
    time_t last_rooms_refresh = 0;

    frame_ctx_t fctx = {&model, &cc, &ui, &last_users_refresh, &last_rooms_refresh};

    char input_line[4096];

    while (!g_quit) {
        if (cc.send_failed) {
            model_set_status(&model, "Send failed or outgoing queue full; reconnect to continue",
                             999);
            model.status = CLI_DISCONNECTED;
            ui_redraw(&ui, &model);
            sleep(2);
            break;
        }
        if (g_sigwinch) {
            g_sigwinch = 0;
            ui_resize(&ui);
        }

        if (ui.dirty)
            ui_redraw(&ui, &model);

        struct pollfd pfd = {.fd = cc.fd, .events = POLLIN | (cc.output_head ? POLLOUT : 0)};
        int pr = poll(&pfd, 1, 50); /* 50ms timeout for keyboard */

        if (pr < 0) {
            if (errno == EINTR)
                continue;
            model_set_status(&model, "poll() error", 5);
            break;
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            model_set_status(&model, "Server disconnected", 999);
            model.status = CLI_DISCONNECTED;
            ui.dirty = true;
            /* Redraw the disconnect notice, then break.
             * Reconnect support is not implemented; exit cleanly. */
            ui_redraw(&ui, &model);
            sleep(2);
            break;
        }

        if ((pfd.revents & POLLOUT) && client_flush(&cc) < 0)
            continue;

        if (pfd.revents & POLLIN) {
            int rr = client_read_frames(&cc, on_frame, &fctx);
            if (rr != 0) {
                /* rr==1: clean close (server sent FIN -  may have preceded an
                 *        error frame we already displayed via on_frame).
                 * rr==-1: I/O / framing error -  show a generic message. */
                if (rr < 0)
                    model_set_status(&model, "Read error \u2014 disconnected", 999);
                /* For a clean close (rr==1) leave any status message the
                 * server sent (e.g. "nick already in use") intact so the
                 * user knows why they were disconnected. */
                model.status = CLI_DISCONNECTED;
                ui.dirty = true;
                /* Redraw the final state (disconnect notice) before exiting
                 * the loop.  Without this the user sees a blank last frame. */
                ui_redraw(&ui, &model);
                break; /* stop spinning on a closed socket */
            }
        }

        /* keyboard (non-blocking) */
        int kr;
        while ((kr = ui_input_tick(&ui, &model, input_line, sizeof(input_line))) != ERR) {
            ui_clamp_navigation(&ui, &model);
            if (kr == UI_RESIZE) {
                ui_resize(&ui);
                continue;
            }
            if (kr == UI_QUIT) {
                g_quit = 1;
                break;
            }
            if (kr == UI_POLL_OPEN) {
                if (!(model.server_caps & CAP_POLLS))
                    model_set_status(&model, "This server does not support polls", 5);
                else {
                    ui.show_poll = true;
                    ui.poll_requested = true;
                    ui.poll_question_row = 0;
                    ui.poll_pending = false;
                    client_send_poll(&cc, 0, 0);
                }
                ui.dirty = true;
                continue;
            }
            if (kr == UI_POLL_VOTE) {
                if (model.poll.id && !model.poll.closed && !model.is_muted && !ui.poll_pending) {
                    ui.poll_pending =
                        client_send_poll(&cc, model.poll.id, (uint32_t)ui.poll_cursor + 1) == 0;
                    ui.dirty = true;
                }
                continue;
            }
            if (kr == UI_PROFILE_OPEN || kr == UI_PROFILE_REFRESH) {
                open_profile(&model, &cc, &ui, kr == UI_PROFILE_REFRESH);
                continue;
            }
            if (kr == UI_PROFILE_SAVE) {
                if (ui.profile.is_self && ui.profile_editing && !ui.profile_saving) {
                    if (++ui.profile_request == 0)
                        ui.profile_request++;
                    ui.profile_saving =
                        client_save_profile(&cc, ui.profile_request, &ui.profile_draft) == 0;
                    snprintf(ui.profile_error, sizeof(ui.profile_error), "%s",
                             ui.profile_saving ? "" : "Could not send profile");
                    ui.dirty = true;
                }
                continue;
            }
            if (kr == UI_PROFILE_MESSAGE) {
                if (ui.profile.presence && ui.profile.user_id) {
                    select_dm(&model, &ui, ui.profile.user_id, ui.profile.nick);
                    ui.show_profile = false;
                } else
                    snprintf(ui.profile_error, sizeof(ui.profile_error), "That account is offline");
                ui.dirty = true;
                continue;
            }
            if (kr == UI_PROFILE_BUDDY) {
                if (ui.profile.account[0] && !ui.profile.is_self) {
                    bool remove = false;
                    for (int i = 0; i < model.buddy_count; i++)
                        if (!strcasecmp(model.buddies[i].account, ui.profile.account))
                            remove = true;
                    client_send_buddy(&cc, remove ? CMSG_BUDDY_REMOVE : CMSG_BUDDY_ADD,
                                      ui.profile.account);
                    snprintf(ui.profile_error, sizeof(ui.profile_error), "Updating buddy list...");
                }
                ui.dirty = true;
                continue;
            }
            if (kr == UI_AUTH_SUBMIT) {
                if (model.status != CLI_SIGNIN || ui.auth_pending)
                    continue;
                const char *error = !ui.auth_name[0]       ? "Enter an account name"
                                    : !ui.auth_password[0] ? "Enter a password"
                                    : ui.auth_register && strcmp(ui.auth_password, ui.auth_confirm)
                                        ? "Passwords do not match"
                                        : NULL;
                if (error) {
                    snprintf(ui.auth_error, sizeof(ui.auth_error), "%s", error);
                    ui.dirty = true;
                    continue;
                }
                int rc = ui.auth_register
                             ? client_send_register(&cc, ui.auth_name, ui.auth_password)
                             : client_send_login(&cc, ui.auth_name, ui.auth_password);
                ui.auth_pending = rc == 0;
                snprintf(ui.auth_error, sizeof(ui.auth_error), "%s",
                         rc < 0 ? "Could not send sign-in request" : "");
                ui_auth_clear_secrets(&ui);
                ui.dirty = true;
                continue;
            }
            if (kr == UI_CLOSE) {
                close_active_chat(&model, &cc, &ui);
                continue;
            }
            if (kr == UI_LATEST) {
                if (model.active_room >= 0)
                    model.rooms[model.active_room].scroll_offset = 0;
                else if (model.active_dm >= 0)
                    model.dms[model.active_dm].scroll_offset = 0;
                ui.dirty = true;
                continue;
            }
            if (kr == UI_NEXT_UNREAD) {
                next_unread(&model, &cc, &ui);
                continue;
            }
            if (kr == UI_BUDDY_OPEN) {
                open_buddies(&model, &cc, &ui);
                continue;
            }
            if (kr == UI_BUDDY_SELECT) {
                if (ui.buddy_cursor >= 0 && ui.buddy_cursor < model.buddy_count) {
                    const cli_buddy_t *buddy = &model.buddies[ui.buddy_cursor];
                    if (buddy->presence && buddy->user_id) {
                        select_dm(&model, &ui, buddy->user_id, buddy->nick);
                        ui.show_buddies_modal = false;
                    } else {
                        model_set_status(&model, "That buddy is offline", 4);
                        ui.show_buddies_modal = false;
                    }
                }
                ui.dirty = true;
                continue;
            }
            if (kr == UI_SCROLL) {
                ui_scroll(&ui, &model, ui.scroll_delta);
                continue;
            }

            if (kr == UI_LINE) {
                /* Enter: command or message */
                if (input_line[0] == '/') {
                    handle_command(input_line, &model, &cc, &ui);
                } else if (model.status == CLI_ONLINE) {
                    if (model.active_room >= 0) {
                        const char *room = model.rooms[model.active_room].name;
                        client_send_room_msg(&cc, room, input_line);
                        /* snap to bottom so the echo is visible */
                        model.rooms[model.active_room].scroll_offset = 0;
                    } else if (model.active_dm >= 0 && model.active_dm < model.dm_count) {
                        const char *peer = model.dms[model.active_dm].peer_nick;
                        client_send_dm_to(&cc, peer, input_line,
                                          model.dms[model.active_dm].peer_uid);
                        model.dms[model.active_dm].scroll_offset = 0;
                    } else {
                        model_set_status(&model, "Join a room first: /join <name>", 4);
                        ui.dirty = true;
                    }
                }
                explicit_bzero(input_line, sizeof(input_line));
            } else if (kr == UI_NONE) {
                /* regular keystroke -  send typing notification (throttled) */
                time_t now = time(NULL);
                if (model.status == CLI_ONLINE && !model.is_muted && !ui.show_profile &&
                    !ui.show_poll && !ui.show_buddies_modal && !ui.show_help_modal &&
                    model.active_room >= 0 && ui.input_len > 0 && now - last_typing_sent >= 2) {
                    const char *room = model.rooms[model.active_room].name;
                    client_send_typing(&cc, room);
                    last_typing_sent = now;
                }
            } else if (kr == UI_PAGE_UP) {
                /* PgUp -  scroll up one full page */
                int page = ui.layout.chat_h - 2;
                if (page < 3)
                    page = 3;
                ui_scroll(&ui, &model, page);
            } else if (kr == UI_PAGE_DOWN) {
                /* PgDn -  scroll down one full page */
                int page = ui.layout.chat_h - 2;
                if (page < 3)
                    page = 3;
                ui_scroll(&ui, &model, -page);
            } else if (kr == UI_SELECT) {
                /* nav-enter: switch to / join the room or DM at nav_cursor */
                int nc = ui.nav_cursor;
                if (nc >= 0 && nc < model.room_count) {
                    const char *rname = model.rooms[nc].name;
                    if (!model.rooms[nc].joined) {
                        model_clear_room_msgs(&model, rname);
                        client_send_join(&cc, rname);
                        model.rooms[nc].joined = true;
                    }
                    model.active_room = nc;
                    model.active_dm = -1;
                    model.user_count = 0;
                    client_send_list_users(&cc, rname);
                    last_users_refresh = time(NULL);
                } else if (nc >= model.room_count && nc < model.room_count + model.dm_count) {
                    model.active_room = -1;
                    model.active_dm = nc - model.room_count;
                }
                ui.dirty = true;
            }
        }

        /* periodic refreshes -  fast enough to feel live, not so fast as to spam */
        time_t now = time(NULL);
        if (model.status == CLI_ONLINE) {
            /* user list: every 15 s for the active room */
            if (model.active_room >= 0 && now - last_users_refresh >= 15) {
                client_send_list_users(&cc, model.rooms[model.active_room].name);
                last_users_refresh = now;
            }
            /* room list: every 60 s so newly created rooms appear */
            if (now - last_rooms_refresh >= 60) {
                client_send_list_rooms(&cc);
                last_rooms_refresh = now;
            }
            /* expire typing indicators so they clear without a new message.
             * Scan ALL rooms so background-room indicators don't live
             * forever and corrupt the display when switching back. */
            for (int i = 0; i < model.room_count; i++) {
                cli_room_t *tr = &model.rooms[i];
                if (tr->typing_nick[0] && now >= tr->typing_until) {
                    tr->typing_nick[0] = '\0';
                    if (i == model.active_room)
                        ui.dirty = true;
                }
            }
        }
    }

    ui_free(&ui);
    sound_play_sync(SND_GOODBYE); /* plays to completion before teardown */
    client_disconnect(&cc);
    model_free(&model);
    return 0;
}
