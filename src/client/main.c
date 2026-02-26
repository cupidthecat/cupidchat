/*
 * src/client/main.c - cupid-chat TUI client entry point
 *
 * Event loop:
 *   poll() on socket fd + timer
 *   network frames -> model updates
 *   keyboard input -> commands / send messages
 *   model changes  -> UI redraws
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasestr for mention detection */
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <ncurses.h>

#include "client/sound.h"

/* Generate a default nick like "guest7342" when no --nick is given. */
static void make_default_nick(char *buf, size_t bufsz) {
    /* seed from time + pid so two terminals opened at the same second
     * still get different numbers */
    unsigned seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
    srand(seed);
    int n = (rand() % 9000) + 1000;   /* 1000-9999 */
    snprintf(buf, bufsz, "guest%d", n);
}

#include "client/client_config.h"
#include "client/client_conn.h"
#include "client/model.h"
#include "client/ui.h"
#include "client/history.h"
#include "proto/types.h"
#include "proto/tlv.h"
#include "proto/frame.h"

/* Signal helpers */

static volatile int g_sigwinch = 0;
static volatile int g_quit     = 0;

static void on_sigwinch(int s) { (void)s; g_sigwinch = 1; }
static void on_sigint  (int s) { (void)s; g_quit     = 1; }

/* Config parsing */

void client_config_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --host HOST        Server hostname (default: 127.0.0.1)\n"
        "  --port PORT        Server port    (default: 5555)\n"
        "  --nick NICK        Your nickname  (default: guest)\n"
        "  --tls              Use TLS connection\n"
        "  --verbose          Debug output\n"
        "  -h, --help\n", prog);
}

int client_config_parse(int argc, char **argv, client_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->host, "127.0.0.1", sizeof(cfg->host) - 1);
    cfg->port = 5555;
    make_default_nick(cfg->nick, sizeof(cfg->nick));

    static const struct option opts[] = {
        {"host",    required_argument, NULL, 'H'},
        {"port",    required_argument, NULL, 'p'},
        {"nick",    required_argument, NULL, 'n'},
        {"tls",     no_argument,       NULL, 't'},
        {"verbose", no_argument,       NULL, 'v'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (c) {
            case 'H': strncpy(cfg->host, optarg, sizeof(cfg->host)-1); break;
            case 'p': cfg->port = (uint16_t)atoi(optarg); break;
            case 'n': strncpy(cfg->nick, optarg, sizeof(cfg->nick)-1); break;
            case 't': cfg->use_tls = true; break;
            case 'v': cfg->verbose = true; break;
            case 'h': return 1;
            default:  return -1;
        }
    }
    return 0;
}

/* Frame handler (network -> model) */

typedef struct {
    cli_model_t   *model;
    client_conn_t *conn;
    ui_state_t    *ui;
    time_t        *last_users_refresh;
    time_t        *last_rooms_refresh;
} frame_ctx_t;

static void on_frame(const frame_t *f, void *ctx_ptr) {
    frame_ctx_t *ctx   = ctx_ptr;
    cli_model_t *m     = ctx->model;
    client_conn_t *cc  = ctx->conn;
    const uint8_t *p   = f->payload;
    uint32_t       pl  = f->hdr.length;

    switch ((server_msg_t)f->hdr.type) {

        case SMSG_WELCOME: {
            uint32_t sid = 0;
            tlv_read_u32(p, pl, TAG_SESSION_ID,  &sid);
            tlv_read_str(p, pl, TAG_SERVER_NAME, m->server_name,
                         sizeof(m->server_name));
            tlv_read_str(p, pl, TAG_MOTD,        m->motd, sizeof(m->motd));
            m->session_id = sid;
            m->user_id    = sid;   /* our own UID for self-message detection */
            m->status     = CLI_ONLINE;
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
                    motd_ts += 1;   /* keep them in order */
                    line = strtok_r(NULL, "\n", &saveptr);
                }
            }
            /* fetch room list and auto-join default room */
            client_send_list_rooms(cc);
            client_send_join(cc, "general");
            /* optimistically track the join in the model */
            cli_room_t *gr = model_get_or_create_room(m, "general");
            if (gr) {
                gr->joined     = true;
                m->active_room = (int)(gr - m->rooms);
                m->active_dm   = -1;
            }
            client_send_list_users(cc, "general");
            ctx->ui->nav_cursor = (m->active_room >= 0) ? m->active_room : 0;
            model_set_status(m, "Connected", 8);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ERROR: {
            uint16_t code = 0;
            char msg[256] = {0};
            tlv_read_u16(p, pl, TAG_ERROR_CODE, &code);
            tlv_read_str(p, pl, TAG_ERROR_MSG,  msg, sizeof(msg));
            model_set_status(m, msg, 5);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_MSG:
        case SMSG_ROOM_HISTORY: {
            char room[65] = {0}, nick[33] = {0}, text[4097] = {0};
            char base_nick[33] = {0};
            uint32_t uid = 0; uint64_t ts = 0;
            tlv_read_str(p, pl, TAG_ROOM,      room, sizeof(room));
            tlv_read_str(p, pl, TAG_NICK,      nick, sizeof(nick));
            tlv_read_u32(p, pl, TAG_USER_ID,   &uid);
            tlv_read_str(p, pl, TAG_TEXT,      text, sizeof(text));
            tlv_read_u64(p, pl, TAG_TIMESTAMP, &ts);
            tlv_read_str(p, pl, TAG_BASE_NICK, base_nick, sizeof(base_nick));
            if (!ts) ts = (uint64_t)time(NULL) * 1000;
            bool self = (uid == m->user_id);
            /* use our own base_nick for self-echo messages */
            const char *bn = self ? m->base_nick : base_nick;
            /* Detect CTCP ACTION:  \x01ACTION <text>\x01 */
            if (text[0] == '\x01' &&
                strncmp(text + 1, "ACTION ", 7) == 0) {
                /* strip wrapper: cut leading \x01ACTION  and trailing \x01 */
                char action_text[4097];
                size_t src_len = strlen(text);
                size_t body_start = 8;   /* 1 + 7 */
                size_t body_end   = (src_len > 0 && text[src_len-1] == '\x01')
                                    ? src_len - 1 : src_len;
                size_t body_len   = (body_end > body_start)
                                    ? body_end - body_start : 0;
                if (body_len >= sizeof(action_text))
                    body_len = sizeof(action_text) - 1;
                memcpy(action_text, text + body_start, body_len);
                action_text[body_len] = '\0';
                model_push_room_msg_typed(m, room, uid, nick,
                                          bn, action_text, ts, self, MSG_ACTION,
                                          m->nick);
            } else {
                model_push_room_msg(m, room, uid, nick, bn, text, ts, self,
                                    m->nick);
            }
            /* auto-scroll to bottom if this is the active room */
            cli_room_t *tr = model_find_room(m, room);
            if (tr && m->active_room >= 0 && tr == &m->rooms[m->active_room]) {
                tr->scroll_offset = 0;
            }
            /* nick-mention highlight: sound + bell */
            if (!self && m->nick[0]) {
                const char *check = (text[0] == '\x01') ? text + 8 : text;
                if (strcasestr(check, m->nick)) {
                    sound_play(SND_IM);
                    beep();
                }
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_DM: {
            char sender_nick[33] = {0}, peer_nick[33] = {0}, text[4097] = {0};
            uint32_t sender_uid = 0, peer_uid2 = 0; uint64_t ts = 0;
            tlv_read_str(p, pl, TAG_NICK,      sender_nick, sizeof(sender_nick));
            tlv_read_u32(p, pl, TAG_USER_ID,   &sender_uid);
            tlv_read_str(p, pl, TAG_PEER_NICK, peer_nick,   sizeof(peer_nick));
            tlv_read_u32(p, pl, TAG_PEER_UID,  &peer_uid2);
            tlv_read_str(p, pl, TAG_TEXT,      text,        sizeof(text));
            tlv_read_u64(p, pl, TAG_TIMESTAMP, &ts);
            if (!ts) ts = (uint64_t)time(NULL) * 1000;
            bool self = (sender_uid == m->user_id);
            /* Both sender and receiver now know who the other party is:
             *   self==true  (echo):  peer = peer_nick / peer_uid2
             *   self==false (inbound): peer = sender_nick / sender_uid */
            uint32_t   conv_uid  = self ? peer_uid2   : sender_uid;
            const char *conv_nick = self ? peer_nick  : sender_nick;
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
            /* auto-scroll to bottom if this is the active DM */
            cli_dm_t *td = model_get_or_create_dm(m, conv_uid, conv_nick);
            if (td && m->active_dm >= 0 && m->active_dm < m->dm_count &&
                td == &m->dms[m->active_dm]) {
                td->scroll_offset = 0;
            }
            if (!self && is_first_msg) sound_play(SND_DM);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_USER_JOINED: {
            char room[65] = {0}, nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_ROOM,    room, sizeof(room));
            tlv_read_str(p, pl, TAG_NICK,    nick, sizeof(nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            char info[128];
            snprintf(info, sizeof(info), "%s joined #%s", nick, room);
            model_push_system_msg(m, room, info, (uint64_t)time(NULL)*1000, MSG_JOIN);
            /* BuddyIn sound for others joining (suppress for our own join echo) */
            if (uid != m->user_id) sound_play(SND_BUDDY_IN);
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
            model_push_system_msg(m, room, info, (uint64_t)time(NULL)*1000, MSG_PART);
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
            tlv_read_str(p, pl, TAG_NICK,     nick,     sizeof(nick));
            tlv_read_u32(p, pl, TAG_USER_ID,  &uid);
            tlv_read_str(p, pl, TAG_AWAY_MSG, away_msg, sizeof(away_msg));
            bool going_away = (away_msg[0] != '\0');
            /* update our own model when the server echoes our own away state */
            if (uid == m->user_id) {
                m->is_away = going_away;
                snprintf(m->away_msg, sizeof(m->away_msg), "%s", away_msg);
            }
            /* push a system message to every room that user is in */
            char info[320];
            if (going_away)
                snprintf(info, sizeof(info), "%s is now away: %s", nick, away_msg);
            else
                snprintf(info, sizeof(info), "%s is back", nick);
            for (int i = 0; i < m->room_count; i++) {
                /* only post in rooms we are actually a member of */
                if (m->rooms[i].active && m->rooms[i].joined)
                    model_push_system_msg(m, m->rooms[i].name, info,
                                         (uint64_t)time(NULL)*1000, MSG_SYSTEM);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_CREATED: {
            /* another client created a room -  add it to our list */
            char name[65] = {0};
            uint32_t owner = 0;
            tlv_read_str(p, pl, TAG_ROOM,      name,  sizeof(name));
            tlv_read_u32(p, pl, TAG_OWNER_UID, &owner);
            cli_room_t *nr = model_get_or_create_room(m, name);
            if (nr) nr->owner_uid = owner;
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_DELETED: {
            char name[65] = {0};
            tlv_read_str(p, pl, TAG_ROOM, name, sizeof(name));
            /* remove from model */
            for (int i = 0; i < m->room_count; i++) {
                if (strcmp(m->rooms[i].name, name) == 0) {
                    /* if it was active, fall back to first available room */
                    if (m->active_room == i) {
                        m->active_room = -1;
                        for (int j = 0; j < m->room_count; j++) {
                            if (j != i && m->rooms[j].active) {
                                m->active_room = j; break;
                            }
                        }
                    } else if (m->active_room > i) {
                        m->active_room--;
                    }
                    /* compact the room array */
                    free(m->rooms[i].msgs);
                    memmove(&m->rooms[i], &m->rooms[i + 1],
                            (size_t)(m->room_count - i - 1) * sizeof(cli_room_t));
                    m->room_count--;
                    /* re-clamp nav cursor */
                    int total = m->room_count + m->dm_count;
                    if (ctx->ui->nav_cursor >= total && total > 0)
                        ctx->ui->nav_cursor = total - 1;
                    break;
                }
            }
            char sb[80];
            snprintf(sb, sizeof(sb), "Room #%.60s was deleted", name);
            model_set_status(m, sb, 6);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_LIST: {
            /* iterate TAG_ROOM / TAG_OWNER_UID pairs */
            tlv_reader_t r;
            tlv_field_t  fld;
            tlv_reader_init(&r, p, pl);
            char pending_name[65] = {0};
            while (tlv_reader_next(&r, &fld)) {
                if (fld.tag == TAG_ROOM) {
                    size_t l = fld.len < 64 ? fld.len : 64;
                    memset(pending_name, 0, sizeof(pending_name));
                    memcpy(pending_name, fld.val, l);
                    model_get_or_create_room(m, pending_name);
                } else if (fld.tag == TAG_OWNER_UID && fld.len >= 4 &&
                           pending_name[0]) {
                    uint32_t ow; memcpy(&ow, fld.val, 4);
                    cli_room_t *rr = model_find_room(m, pending_name);
                    if (rr) rr->owner_uid = ntohl(ow);
                }
            }
            /* if still not in any room, make general (or first room) active */
            if (m->active_room < 0) {
                int idx = -1;
                for (int i = 0; i < m->room_count; i++) {
                    if (strcmp(m->rooms[i].name, "general") == 0) { idx = i; break; }
                }
                if (idx < 0 && m->room_count > 0) idx = 0;
                if (idx >= 0) {
                    m->active_room = idx;
                    m->active_dm   = -1;
                }
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_USER_LIST: {
            cli_user_t tmp[CLI_MAX_USERS];
            int count = 0;
            tlv_reader_t r;
            tlv_field_t  fld;
            tlv_reader_init(&r, p, pl);
            char pending_nick[33] = {0};
            while (tlv_reader_next(&r, &fld) && count < CLI_MAX_USERS) {
                if (fld.tag == TAG_NICK) {
                    size_t l = fld.len < 32 ? fld.len : 32;
                    memcpy(pending_nick, fld.val, l);
                    pending_nick[l] = '\0';
                } else if (fld.tag == TAG_USER_ID && fld.len >= 4) {
                    uint32_t uid; memcpy(&uid, fld.val, 4);
                    tmp[count].user_id = ntohl(uid);
                    snprintf(tmp[count].nick, sizeof(tmp[count].nick), "%s", pending_nick);
                    count++;
                    memset(pending_nick, 0, sizeof(pending_nick));
                }
            }
            /* find current room name */
            const char *cur_room = "";
            if (m->active_room >= 0 && m->active_room < m->room_count)
                cur_room = m->rooms[m->active_room].name;
            model_update_users(m, cur_room, tmp, count);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_NICK_CHANGED: {
            char new_nick[33] = {0}, old_nick[33] = {0}, room[65] = {0};
            char base_nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK,      new_nick,  sizeof(new_nick));
            tlv_read_str(p, pl, TAG_OLD_NICK,  old_nick,  sizeof(old_nick));
            tlv_read_str(p, pl, TAG_ROOM,      room,      sizeof(room));
            tlv_read_u32(p, pl, TAG_USER_ID,   &uid);
            tlv_read_str(p, pl, TAG_BASE_NICK, base_nick, sizeof(base_nick));

            if (room[0]) {
                /* room broadcast: other users see the rename */
                char info[128];
                snprintf(info, sizeof(info), "%.32s is now known as %.32s",
                         old_nick, new_nick);
                model_push_system_msg(m, room, info,
                                      (uint64_t)time(NULL)*1000, MSG_SYSTEM);
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
                            snprintf(m->dms[i].peer_nick,
                                     sizeof(m->dms[i].peer_nick),
                                     "%s", new_nick);
                    }
                }
            } else {
                /* private confirmation: we renamed ourselves */
                snprintf(m->nick, sizeof(m->nick), "%s", new_nick);
                /* base_nick stays unchanged -  it's the account/original name */
                /* push the rename notice into all our joined rooms */
                char info[128];
                snprintf(info, sizeof(info), "%.32s is now known as %.32s",
                         old_nick, new_nick);
                for (int i = 0; i < m->room_count; i++) {
                    if (m->rooms[i].active && m->rooms[i].joined)
                        model_push_system_msg(m, m->rooms[i].name, info,
                                              (uint64_t)time(NULL)*1000, MSG_SYSTEM);
                }
                char sb[80];
                snprintf(sb, sizeof(sb), "You are now known as %.32s", new_nick);
                model_set_status(m, sb, 6);
            }
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_REGISTER_OK: {
            char new_nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK,    new_nick, sizeof(new_nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            snprintf(m->nick,      sizeof(m->nick),      "%s", new_nick);
            snprintf(m->base_nick, sizeof(m->base_nick), "%s", new_nick);
            if (uid) m->user_id = uid;
            char sb[80];
            snprintf(sb, sizeof(sb), "Registered & logged in as %.32s", new_nick);
            model_set_status(m, sb, 8);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_LOGIN_OK: {
            char new_nick[33] = {0};
            uint32_t uid = 0;
            tlv_read_str(p, pl, TAG_NICK,    new_nick, sizeof(new_nick));
            tlv_read_u32(p, pl, TAG_USER_ID, &uid);
            snprintf(m->nick,      sizeof(m->nick),      "%s", new_nick);
            snprintf(m->base_nick, sizeof(m->base_nick), "%s", new_nick);
            if (uid) m->user_id = uid;
            char sb[80];
            snprintf(sb, sizeof(sb), "Logged in as %.32s", new_nick);
            model_set_status(m, sb, 8);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_LOGOUT_OK: {
            char guest_nick[33] = {0};
            tlv_read_str(p, pl, TAG_NICK, guest_nick, sizeof(guest_nick));
            snprintf(m->nick,      sizeof(m->nick),      "%s", guest_nick);
            snprintf(m->base_nick, sizeof(m->base_nick), "%s", guest_nick);
            model_set_status(m, "Logged out - back to guest session", 6);
            ctx->ui->dirty = true;
            break;
        }

        case SMSG_ROOM_TOPIC: {
            char room[65] = {0}, topic[257] = {0}, setter[33] = {0};
            tlv_read_str(p, pl, TAG_ROOM,  room,   sizeof(room));
            tlv_read_str(p, pl, TAG_TOPIC, topic,  sizeof(topic));
            tlv_read_str(p, pl, TAG_NICK,  setter, sizeof(setter));
            cli_room_t *tr = model_find_room(m, room);
            if (tr) {
                snprintf(tr->topic,        sizeof(tr->topic),        "%s", topic);
                snprintf(tr->topic_set_by, sizeof(tr->topic_set_by), "%s", setter);
            }
            /* emit a system message only when someone actively set the topic */
            if (setter[0] && topic[0]) {
                char info[320];
                snprintf(info, sizeof(info), "%.32s set the topic: %.200s",
                         setter, topic);
                model_push_system_msg(m, room, info,
                                     (uint64_t)time(NULL)*1000, MSG_SYSTEM);
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
                tr->typing_until = time(NULL) + 3;  /* show for 3 seconds */
                ctx->ui->dirty = true;
            }
            break;
        }

        default:
            break;
    }
}

/* Command dispatcher */

static void handle_command(const char *line, cli_model_t *m,
                            client_conn_t *cc, ui_state_t *ui) {
    /* skip leading '/' */
    const char *cmd = line + 1;

    if (strncmp(cmd, "me ", 3) == 0) {
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
    } else if (strncmp(cmd, "topic", 5) == 0) {
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
        while (*room == ' ') room++;   /* strip accidental leading spaces */
        if (!*room) {
            model_set_status(m, "Usage: /join <roomname>", 4);
        } else {
        /* clear stale history so server replay won't duplicate */
        model_clear_room_msgs(m, room);
        client_send_join(cc, room);
        /* optimistically set active room */
        cli_room_t *r = model_get_or_create_room(m, room);
        if (r) {
            r->joined = true;
            m->active_room = (int)(r - m->rooms);
            m->active_dm   = -1;
            ui->nav_cursor = m->active_room;
        }
        client_send_list_users(cc, room);
        char sb[80]; snprintf(sb, sizeof(sb), "Joining #%.60s...", room);
        model_set_status(m, sb, 4);
        }
    } else if (strncmp(cmd, "leave", 5) == 0) {
        const char *room = (strlen(cmd) > 6) ? cmd + 6 : "";
        if (!*room && m->active_room >= 0)
            room = m->rooms[m->active_room].name;
        if (*room) {
            client_send_leave(cc, room);
            char sb[80]; snprintf(sb, sizeof(sb), "Left #%.64s", room);
            model_set_status(m, sb, 4);
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
        while (*new_nick == ' ') new_nick++;
        if (*new_nick) {
            client_send_nick(cc, new_nick);
            model_set_status(m, "Changing nick...", 3);
        } else {
            model_set_status(m, "Usage: /nick <newnick>", 4);
        }
    } else if (strncmp(cmd, "away", 4) == 0) {
        /* /away [message]  -  set away with optional message */
        const char *msg = (strlen(cmd) > 5) ? cmd + 5 : "away";
        while (*msg == ' ') msg++;
        if (!*msg) msg = "away";
        client_send_away(cc, msg);
        m->is_away = true;
        snprintf(m->away_msg, sizeof(m->away_msg), "%.255s", msg);
        char sb[300]; snprintf(sb, sizeof(sb), "Away: %.255s", msg);
        model_set_status(m, sb, 5);
    } else if (strcmp(cmd, "back") == 0) {
        /* /back  -  return from away */
        client_send_away(cc, "");
        m->is_away = false;
        m->away_msg[0] = '\0';
        model_set_status(m, "You are no longer away", 4);
    } else if (strncmp(cmd, "ignore", 6) == 0 &&
               (cmd[6] == ' ' || cmd[6] == '\0')) {
        /* /ignore           -  list ignored nicks
         * /ignore <nick>    -  add nick to ignore list */
        const char *nick = (cmd[6] == ' ') ? cmd + 7 : "";
        while (*nick == ' ') nick++;
        if (!*nick) {
            /* list current ignores */
            if (m->ignore_count == 0) {
                model_set_status(m, "Ignore list is empty", 4);
            } else {
                char sb[256] = "Ignoring: ";
                for (int i = 0; i < m->ignore_count; i++) {
                    if (i) strncat(sb, ", ", sizeof(sb) - strlen(sb) - 1);
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
            snprintf(m->ignore_list[m->ignore_count++],
                     CLI_NICK_MAX, "%.32s", nick);
            char sb[80]; snprintf(sb, sizeof(sb), "Now ignoring %.32s", nick);
            model_set_status(m, sb, 5);
            ignore_done:;
        }
    } else if (strncmp(cmd, "unignore ", 9) == 0) {
        /* /unignore <nick> */
        const char *nick = cmd + 9;
        while (*nick == ' ') nick++;
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
            char sb[80]; snprintf(sb, sizeof(sb), "No longer ignoring %.32s", nick);
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
            char sb[80]; snprintf(sb, sizeof(sb), "DM sent to @%s", target);
            model_set_status(m, sb, 4);
        } else {
            model_set_status(m, "Usage: /dm <nick> <message>", 4);
        }
    } else if (strncmp(cmd, "delroom", 7) == 0) {
        /* /delroom [name]  -  delete a room you own */
        const char *room = (strlen(cmd) > 8) ? cmd + 8 : "";
        if (!*room && m->active_room >= 0)
            room = m->rooms[m->active_room].name;
        if (*room) {
            client_send_delete_room(cc, room);
            char sb[80]; snprintf(sb, sizeof(sb), "Deleting #%.60s...", room);
            model_set_status(m, sb, 4);
        } else {
            model_set_status(m, "Usage: /delroom <name>", 4);
        }
    } else if (strncmp(cmd, "closedm", 7) == 0) {
        /* /closedm [nick]  -  remove a DM conversation from the sidebar */
        const char *nick = (strlen(cmd) > 8) ? cmd + 8 : "";
        bool found = false;
        for (int i = 0; i < m->dm_count; i++) {
            if (!*nick || strcmp(m->dms[i].peer_nick, nick) == 0) {
                if (m->active_dm == i) {
                    m->active_dm   = -1;
                    /* fall back to first room, or -1 if none exist */
                    m->active_room = (m->room_count > 0) ? 0 : -1;
                }
                else if (m->active_dm > i) m->active_dm--;
                free(m->dms[i].msgs);
                memmove(&m->dms[i], &m->dms[i + 1],
                        (size_t)(m->dm_count - i - 1) * sizeof(cli_dm_t));
                m->dm_count--;
                found = true;
                break;
            }
        }
        if (found) model_set_status(m, "DM conversation closed", 3);
        else        model_set_status(m, !*nick ? "Usage: /closedm <nick>" : "No such DM", 3);
        ui->dirty = true;
    } else if (strcmp(cmd, "rooms") == 0) {
        client_send_list_rooms(cc);
        model_set_status(m, "Fetching room list...", 3);
    } else if (strncmp(cmd, "users", 5) == 0) {
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
    if (rc == 1)  { client_config_usage(argv[0]); return 0; }
    if (rc == -1) { client_config_usage(argv[0]); return 1; }

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
    snprintf(model.nick,      sizeof(model.nick),      "%s", cfg.nick);
    snprintf(model.base_nick, sizeof(model.base_nick), "%s", cfg.nick);

    /* init sounds (relative to CWD where the binary is run from) */
    sound_init("sounds");

    /* send HELLO */
    if (client_send_hello(&cc, cfg.nick) < 0) {
        fprintf(stderr, "Failed to send HELLO\n");
        client_disconnect(&cc);
        return 1;
    }

    /* init UI */
    ui_state_t ui;
    ui_init(&ui);

    signal(SIGWINCH, on_sigwinch);
    signal(SIGINT,   on_sigint);
    signal(SIGTERM,  on_sigint);
    signal(SIGPIPE,  SIG_IGN);

    time_t last_users_refresh = 0;
    time_t last_typing_sent   = 0;   /* throttle CMSG_TYPING to once per 2s */
    time_t last_rooms_refresh = 0;

    frame_ctx_t fctx = {&model, &cc, &ui,
                        &last_users_refresh, &last_rooms_refresh};

    char input_line[4096];

    while (!g_quit) {
        if (g_sigwinch) {
            g_sigwinch = 0;
            ui_resize(&ui);
        }

        if (ui.dirty)
            ui_redraw(&ui, &model);

        struct pollfd pfd = {.fd = cc.fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 50);   /* 50ms timeout for keyboard */

        if (pr < 0) {
            if (errno == EINTR) continue;
            model_set_status(&model, "poll() error", 5);
            break;
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            model_set_status(&model, "Server disconnected", 999);
            model.status = CLI_DISCONNECTED;
            ui.dirty     = true;
            /* Redraw the disconnect notice, then break.
             * Reconnect support is not implemented; exit cleanly. */
            ui_redraw(&ui, &model);
            sleep(2);
            break;
        }

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
                ui.dirty     = true;
                /* Redraw the final state (disconnect notice) before exiting
                 * the loop.  Without this the user sees a blank last frame. */
                ui_redraw(&ui, &model);
                break;  /* stop spinning on a closed socket */
            }
        }

        /* keyboard (non-blocking) */
        int kr;
        while ((kr = ui_input_tick(&ui, input_line, sizeof(input_line))) != ERR) {
            /* clamp nav_cursor to valid range after every keystroke */
            {
                int total = model.room_count + model.dm_count;
                if (total > 0 && ui.nav_cursor >= total)
                    ui.nav_cursor = total - 1;
                if (ui.nav_cursor < 0 && total > 0)
                    ui.nav_cursor = 0;
            }

            if (kr == 1) {
                /* Enter: command or message */
                if (input_line[0] == '/') {
                    handle_command(input_line, &model, &cc, &ui);
                } else if (model.status == CLI_ONLINE) {
                    if (model.active_room >= 0) {
                        const char *room = model.rooms[model.active_room].name;
                        client_send_room_msg(&cc, room, input_line);
                        /* snap to bottom so the echo is visible */
                        model.rooms[model.active_room].scroll_offset = 0;
                    } else if (model.active_dm >= 0 &&
                               model.active_dm < model.dm_count) {
                        const char *peer = model.dms[model.active_dm].peer_nick;
                        client_send_dm(&cc, peer, input_line);
                        model.dms[model.active_dm].scroll_offset = 0;
                    } else {
                        model_set_status(&model, "Join a room first: /join <name>", 4);
                        ui.dirty = true;
                    }
                }
            } else if (kr == 0) {
                /* regular keystroke -  send typing notification (throttled) */
                time_t now = time(NULL);
                if (model.status == CLI_ONLINE &&
                    model.active_room >= 0 &&
                    ui.input_len > 0 &&
                    now - last_typing_sent >= 2) {
                    const char *room = model.rooms[model.active_room].name;
                    client_send_typing(&cc, room);
                    last_typing_sent = now;
                }
            } else if (kr == 2) {
                /* PgUp -  scroll up one full page */
                int page = ui.layout.chat_h - 2;
                if (page < 3) page = 3;
                ui_scroll(&ui, &model, page);
            } else if (kr == 3) {
                /* PgDn -  scroll down one full page */
                int page = ui.layout.chat_h - 2;
                if (page < 3) page = 3;
                ui_scroll(&ui, &model, -page);
            } else if (kr == 4) {
                /* nav-enter: switch to / join the room or DM at nav_cursor */
                int nc = ui.nav_cursor;
                if (nc >= 0 && nc < model.room_count) {
                    const char *rname = model.rooms[nc].name;
                    if (!model.rooms[nc].joined) {
                        client_send_join(&cc, rname);
                        model.rooms[nc].joined = true;
                    }
                    model.active_room = nc;
                    model.active_dm   = -1;
                    client_send_list_users(&cc, rname);
                    last_users_refresh = time(NULL);
                } else if (nc >= model.room_count &&
                           nc < model.room_count + model.dm_count) {
                    model.active_room = -1;
                    model.active_dm   = nc - model.room_count;
                }
                ui.dirty = true;
            }
        }

        /* periodic refreshes -  fast enough to feel live, not so fast as to spam */
        time_t now = time(NULL);
        if (model.status == CLI_ONLINE) {
            /* user list: every 15 s for the active room */
            if (model.active_room >= 0 &&
                now - last_users_refresh >= 15) {
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
                    if (i == model.active_room) ui.dirty = true;
                }
            }
        }
    }

    ui_free(&ui);
    sound_play_sync(SND_GOODBYE);   /* plays to completion before teardown */
    client_disconnect(&cc);
    model_free(&model);
    return 0;
}
