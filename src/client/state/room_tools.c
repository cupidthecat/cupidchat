#include "client/room_flow.h"
#include "client/sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void message(ui_state_t *ui, cli_model_t *m, const char *text) {
    snprintf(ui->room_control.message, sizeof(ui->room_control.message), "%.191s", text);
    if (ui->room_control.from_profile && ui->show_profile)
        snprintf(ui->profile_error, sizeof(ui->profile_error), "%.191s", text);
    model_set_status(m, text, 7);
    ui->dirty = true;
}

void room_control_send(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const char *room,
                       const char *action, const char *account, const char *reason, bool enabled,
                       bool from_profile) {
    room_control_state_t *r = &ui->room_control;
    if (!(m->server_caps & CAP_ROOM_TOOLS)) {
        message(ui, m, "This server does not support room tools");
        return;
    }
    if (r->pending) {
        message(ui, m, "Waiting for the previous room action");
        return;
    }
    if (!room_name_valid(room)) {
        message(ui, m,
                *room ? "Use a room name of 1-64 letters, digits, underscores, or hyphens"
                      : "Select a room first");
        return;
    }
    if (++r->request == 0)
        r->request++;
    r->from_profile = from_profile;
    r->pending = true;
    r->message[0] = '\0';
    tlv_builder_t b;
    tlv_builder_init(&b);
    bool failed = tlv_put_u32(&b, TAG_REQUEST_ID, r->request) < 0 ||
                  tlv_put_str(&b, TAG_ROOM, room) < 0 ||
                  tlv_put_str(&b, TAG_ADMIN_ACTION, action) < 0 ||
                  tlv_put_str(&b, TAG_BASE_NICK, account ? account : "") < 0 ||
                  tlv_put_str(&b, TAG_TEXT, reason ? reason : "") < 0 ||
                  tlv_put_u8(&b, TAG_ENABLED, enabled) < 0;
    if (failed || client_send_frame(cc, CMSG_ROOM_CONTROL, b.buf, (uint32_t)b.len) < 0) {
        r->pending = false;
        message(ui, m, "Could not send room action");
    }
    tlv_builder_free(&b);
    ui->dirty = true;
}

void room_control_open(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const char *room) {
    if (!room || !*room) {
        message(ui, m, "Join or select a room first");
        return;
    }
    if (!(m->server_caps & CAP_ROOM_TOOLS)) {
        message(ui, m, "This server does not support room tools");
        return;
    }
    if (ui->room_control.pending)
        return;
    if (strcmp(ui->room_control.info.room, room)) {
        uint32_t request = ui->room_control.request;
        memset(&ui->room_control, 0, sizeof(ui->room_control));
        ui->room_control.request = request;
    }
    ui->room_control.visible = true;
    ui->show_help_modal = ui->show_poll = ui->show_buddies_modal = ui->show_emoji_modal = false;
    room_control_send(ui, m, cc, room, "info", NULL, NULL, false, false);
}

void room_control_action(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, int action) {
    room_control_state_t *r = &ui->room_control;
    if (action == UI_ROOM_OPEN) {
        const char *room = m->active_room >= 0 ? m->rooms[m->active_room].name : "";
        room_control_open(ui, m, cc, room);
        return;
    }
    if (!r->loaded || r->pending)
        return;
    if (action == UI_ROOM_REFRESH) {
        room_control_send(ui, m, cc, r->info.room, "info", NULL, NULL, false, false);
        return;
    }
    static const char *actions[] = {"invite", "kick",      "ban",     "unban", "op",
                                    "deop",   "topiclock", "decline", "info",  "join"};
    if (r->action < 0 || r->action >= 10)
        return;
    const char *command = actions[r->action];
    if (!strcmp(command, "join")) {
        cli_room_t *room = model_get_or_create_room(m, r->info.room);
        if (!room) {
            message(ui, m, "Room list is full");
            return;
        }
        if (!room->joined) {
            model_clear_room_msgs(m, room->name);
            if (client_send_join(cc, room->name) < 0) {
                message(ui, m, "Could not send join");
                return;
            }
            room->joined = true;
        }
        m->active_room = (int)(room - m->rooms);
        m->active_dm = -1;
        ui->nav_cursor = m->active_room;
        client_send_list_users(cc, room->name);
        r->visible = false;
        ui->dirty = true;
        return;
    }
    room_control_send(ui, m, cc, r->info.room, command, r->target, r->reason, !r->info.topic_locked,
                      false);
}

static void apply(cli_model_t *m, const room_info_t *info) {
    cli_room_t *r = model_get_or_create_room(m, info->room);
    if (!r)
        return;
    r->room_role = info->role;
    r->topic_locked = info->topic_locked;
    r->owner_uid = info->owner_uid;
    snprintf(r->owner_account, sizeof(r->owner_account), "%s", info->owner);
    snprintf(r->topic, sizeof(r->topic), "%s", info->topic);
    if (!(info->role & ROOM_INVITED))
        r->invite_from[0] = '\0';
}

bool room_control_frame(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const frame_t *f) {
    (void)cc;
    const uint8_t *p = f->payload;
    uint32_t length = f->hdr.length;
    room_control_state_t *r = &ui->room_control;
    if (f->hdr.type == SMSG_ERROR) {
        char scope[16] = {0};
        tlv_read_str(p, length, TAG_SCOPE, scope, sizeof(scope));
        if (strcmp(scope, "room"))
            return false;
        uint32_t id = 0;
        tlv_read_u32(p, length, TAG_REQUEST_ID, &id);
        if (id == r->request && r->pending) {
            char text[192] = {0};
            tlv_read_str(p, length, TAG_ERROR_MSG, text, sizeof(text));
            r->pending = false;
            message(ui, m, text);
            sound_play(SND_ERROR);
        }
        return true;
    }
    if (f->hdr.type < SMSG_ROOM_INFO || f->hdr.type > SMSG_ROOM_KICKED)
        return false;
    if (!m->logged_in)
        return true;
    char room[65];
    if (!room_read_name(p, length, room))
        return true;
    ui->dirty = true;
    if (f->hdr.type == SMSG_ROOM_INVITE) {
        char inviter[33] = {0}, scope[16] = {0};
        room_read_text(p, length, TAG_BASE_NICK, inviter, sizeof(inviter));
        tlv_read_str(p, length, TAG_SCOPE, scope, sizeof(scope));
        cli_room_t *target = model_get_or_create_room(m, room);
        if (target) {
            target->room_role |= ROOM_INVITED;
            snprintf(target->invite_from, sizeof(target->invite_from), "%s", inviter);
        }
        char text[384];
        snprintf(text, sizeof(text), "%s invited you to #%s. /join %s or /decline %s", inviter,
                 room, room, room);
        model_set_status(m, text, 12);
        model_push_system_msg(m, "general", text, (uint64_t)time(NULL) * 1000, MSG_SYSTEM);
        if (!strcmp(scope, "new") && !model_is_ignored(m, inviter))
            sound_play(SND_INVITE);
        return true;
    }
    if (f->hdr.type == SMSG_ROOM_KICKED) {
        char actor[33] = {0}, reason[257] = {0};
        room_read_text(p, length, TAG_NICK, actor, sizeof(actor));
        room_read_text(p, length, TAG_TEXT, reason, sizeof(reason));
        char text[384];
        snprintf(text, sizeof(text), "Removed from #%s by %s%s%s", room, actor,
                 reason[0] ? ": " : "", reason);
        cli_room_t *target = model_find_room(m, room);
        if (target) {
            target->joined = false;
            target->unread = 0;
            target->typing_nick[0] = '\0';
        }
        model_push_system_msg(m, room, text, (uint64_t)time(NULL) * 1000, MSG_SYSTEM);
        if (!strcmp(m->users_room, room)) {
            m->user_count = 0;
            m->users_room[0] = '\0';
        }
        if (m->active_room >= 0 && !strcmp(m->rooms[m->active_room].name, room))
            model_select_fallback(m);
        ui_clamp_navigation(ui, m);
        message(ui, m, text);
        sound_play(SND_ERROR);
        return true;
    }
    room_info_t info = {0};
    snprintf(info.room, sizeof(info.room), "%s", room);
    uint8_t locked = 0;
    if (!room_read_text(p, length, TAG_ROOM_OWNER, info.owner, sizeof(info.owner)) ||
        !tlv_read_str(p, length, TAG_TOPIC, info.topic, sizeof(info.topic)) ||
        !tlv_read_u8(p, length, TAG_ROOM_ROLE, &info.role) ||
        !tlv_read_u8(p, length, TAG_TOPIC_LOCK, &locked))
        return true;
    info.topic_locked = locked != 0;
    tlv_read_u32(p, length, TAG_OWNER_UID, &info.owner_uid);
    apply(m, &info);
    uint32_t id = 0;
    tlv_read_u32(p, length, TAG_REQUEST_ID, &id);
    if (!id) {
        if (r->loaded && !strcmp(r->info.room, room)) {
            if ((r->info.role & ROOM_MANAGERS) && !(info.role & ROOM_MANAGERS))
                r->info.count = 0;
            r->info.role = info.role;
            r->info.topic_locked = info.topic_locked;
            snprintf(r->message, sizeof(r->message),
                     "Room settings changed. Refresh updates the access list.");
        }
        return true;
    }
    if (id != r->request || !r->pending)
        return true;
    uint8_t more = 0;
    tlv_read_u8(p, length, TAG_MORE, &more);
    info.more = more != 0;
    tlv_reader_t reader;
    tlv_field_t field;
    tlv_reader_init(&reader, p, length);
    bool valid = true;
    while (tlv_reader_next(&reader, &field))
        if (field.tag == TAG_ENTRY) {
            if (info.count == ROOM_ACCESS_PAGE) {
                valid = false;
                break;
            }
            room_access_t *a = &info.entries[info.count++];
            if (!room_read_text(field.val, field.len, TAG_BASE_NICK, a->account,
                                sizeof(a->account)) ||
                !tlv_read_u8(field.val, field.len, TAG_ROOM_ROLE, &a->flags)) {
                valid = false;
                break;
            }
            room_read_text(field.val, field.len, TAG_NICK, a->inviter, sizeof(a->inviter));
            room_read_text(field.val, field.len, TAG_TEXT, a->reason, sizeof(a->reason));
            tlv_read_u64(field.val, field.len, TAG_TIMESTAMP, &a->invited_at);
        }
    r->pending = false;
    if (!valid) {
        message(ui, m, "Invalid room access response");
        return true;
    }
    bool first = !r->loaded;
    r->info = info;
    r->loaded = true;
    if (first && !(info.role & ROOM_MANAGERS))
        r->action = info.role & ROOM_INVITED ? 9 : 8;
    char text[192] = {0};
    tlv_read_str(p, length, TAG_TEXT, text, sizeof(text));
    if (text[0])
        message(ui, m, text);
    return true;
}

bool room_control_command(ui_state_t *ui, cli_model_t *m, client_conn_t *cc, const char *cmd) {
    const char *active = m->active_room >= 0 ? m->rooms[m->active_room].name : "";
    if (!strcmp(cmd, "room") || (!strncmp(cmd, "roominfo", 8) && (!cmd[8] || cmd[8] == ' '))) {
        const char *name = !strcmp(cmd, "room") || !cmd[8] ? active : cmd + 9;
        if (*name == '#')
            name++;
        room_control_open(ui, m, cc, name);
        return true;
    }
    char verb[16] = {0};
    size_t n = strcspn(cmd, " ");
    if (n >= sizeof(verb))
        return false;
    memcpy(verb, cmd, n);
    const char *arg = cmd + n;
    while (*arg == ' ')
        arg++;
    if (!strcmp(verb, "mode")) {
        if (strcmp(arg, "+t") && strcmp(arg, "-t"))
            message(ui, m, "Usage: /mode +t or /mode -t in the current room");
        else
            room_control_send(ui, m, cc, active, "topiclock", NULL, NULL, arg[0] == '+', false);
        return true;
    }
    const char *names[] = {"invite", "kick", "ban", "unban", "op", "deop", "decline"};
    bool known = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(verb, names[i]))
            known = true;
    if (!known)
        return false;
    if (!strcmp(verb, "decline")) {
        if (*arg == '#')
            arg++;
        room_control_send(ui, m, cc, *arg ? arg : active, verb, NULL, NULL, false, false);
        return true;
    }
    char account[33];
    size_t size = strcspn(arg, " ");
    if (!size || size > 32) {
        message(ui, m, "Provide an account name of at most 32 bytes");
        return true;
    }
    memcpy(account, arg, size);
    account[size] = '\0';
    const char *rest = arg + size;
    while (*rest == ' ')
        rest++;
    if (!strcmp(verb, "invite")) {
        const char *room = *rest ? rest : active;
        if (*room == '#')
            room++;
        room_control_send(ui, m, cc, room, verb, account, "", false, false);
    } else
        room_control_send(ui, m, cc, active, verb, account, rest, false, false);
    return true;
}
