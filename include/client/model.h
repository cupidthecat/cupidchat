/* CupidChat public interface. */
#pragma once
#include "proto/profile.h"
#include "proto/types.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define CLI_NICK_MAX (MAX_NICK_LEN + 1)
#define CLI_ROOM_MAX (MAX_ROOM_LEN + 1)
#define CLI_MSG_MAX (MAX_MSG_LEN + 1)
#define CLI_MAX_ROOMS 256
#define CLI_MAX_USERS 1024
#define CLI_HISTORY_CAP 500

typedef enum { CLI_DISCONNECTED, CLI_CONNECTING, CLI_SIGNIN, CLI_ONLINE } cli_status_t;

typedef enum { MSG_CHAT, MSG_ACTION, MSG_JOIN, MSG_PART, MSG_QUIT, MSG_SYSTEM } cli_msg_type_t;

typedef struct {
    uint32_t user_id;
    uint64_t timestamp_ms;
    char nick[CLI_NICK_MAX], base_nick[CLI_NICK_MAX], text[CLI_MSG_MAX];
    bool is_self, is_dm, is_highlight, is_history;
    cli_msg_type_t type;
} cli_msg_t;

typedef struct {
    char name[CLI_ROOM_MAX], topic[257], topic_set_by[CLI_NICK_MAX];
    bool active, joined;
    uint32_t owner_uid;
    cli_msg_t *msgs;
    int msgs_cap, msgs_head, msgs_len, unread, scroll_offset;
    int view_width;
    char typing_nick[CLI_NICK_MAX];
    time_t typing_until;
} cli_room_t;

typedef struct {
    uint32_t peer_uid;
    char peer_nick[CLI_NICK_MAX];
    cli_msg_t *msgs;
    int msgs_cap, msgs_head, msgs_len, unread, scroll_offset;
    int view_width;
} cli_dm_t;

typedef struct {
    uint32_t user_id;
    char nick[CLI_NICK_MAX];
} cli_user_t;

typedef struct {
    char account[CLI_NICK_MAX], nick[CLI_NICK_MAX], away_msg[256];
    uint32_t user_id;
    uint8_t presence;
} cli_buddy_t;

typedef struct {
    cli_status_t status;
    uint32_t session_id, user_id;
    uint32_t server_caps;
    bool logged_in;
    cli_buddy_t buddies[MAX_BUDDIES];
    int buddy_count;
    char nick[CLI_NICK_MAX], base_nick[CLI_NICK_MAX], server_name[128], motd[2048];
    bool is_away;
    char away_msg[256];
    cli_room_t rooms[CLI_MAX_ROOMS];
    int room_count, active_room;
    cli_dm_t dms[64];
    int dm_count, active_dm;
    cli_user_t users[CLI_MAX_USERS];
    int user_count;
    char users_room[CLI_ROOM_MAX], pending_query[CLI_NICK_MAX];
    char last_dm_nick[CLI_NICK_MAX];
    uint32_t last_dm_uid;
    char status_msg[256];
    time_t status_msg_until;
    char ignore_list[32][CLI_NICK_MAX];
    int ignore_count;
} cli_model_t;

void model_init(cli_model_t *m);
void model_free(cli_model_t *m);
cli_room_t *model_find_room(cli_model_t *m, const char *name);
cli_room_t *model_get_or_create_room(cli_model_t *m, const char *name);
void model_push_room_msg(cli_model_t *m, const char *room, uint32_t uid, const char *nick,
                         const char *base_nick, const char *text, uint64_t ts_ms, bool self,
                         const char *my_nick);
void model_push_room_msg_typed(cli_model_t *m, const char *room, uint32_t uid, const char *nick,
                               const char *base_nick, const char *text, uint64_t ts_ms, bool self,
                               cli_msg_type_t type, const char *my_nick);
void model_push_system_msg(cli_model_t *m, const char *room, const char *text, uint64_t ts_ms,
                           cli_msg_type_t type);
void model_clear_room_msgs(cli_model_t *m, const char *room);
bool model_is_ignored(const cli_model_t *m, const char *nick);
cli_dm_t *model_get_or_create_dm(cli_model_t *m, uint32_t uid, const char *nick);
void model_push_dm_msg(cli_model_t *m, uint32_t uid, const char *nick, const char *text,
                       uint64_t ts_ms, bool self);
void model_set_status(cli_model_t *m, const char *msg, int secs);
void model_update_users(cli_model_t *m, const char *room, cli_user_t *users, int count);
void model_select_fallback(cli_model_t *m);
bool model_close_dm(cli_model_t *m, int index);
bool model_remove_room(cli_model_t *m, const char *name);
