#ifndef CLIENT_MODEL_H
#define CLIENT_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "proto/types.h"

typedef enum {
    CLI_OFFLINE = 0,
    CLI_CONNECTING,
    CLI_ONLINE,
    CLI_DISCONNECTED
} cli_status_t;

typedef enum {
    MSG_CHAT = 0,
    MSG_ACTION,
    MSG_SYSTEM,
    MSG_JOIN,
    MSG_PART,
    MSG_QUIT
} cli_msg_type_t;

#define CLI_NICK_MAX 33
#define CLI_MAX_USERS 256
#define CLI_MAX_ROOMS 64
#define CLI_ROOM_MAX 65
#define CLI_HISTORY_CAP 100
#define CLI_MSG_MAX 4096

typedef struct {
    uint32_t user_id;
    char nick[33];
    char base_nick[33];
    char text[4096];
    uint64_t timestamp_ms;
    cli_msg_type_t type;
    bool is_highlight;
    bool is_self;
    bool is_dm;
} cli_msg_t;

typedef struct {
    char name[65];
    char topic[256];
    char topic_set_by[CLI_NICK_MAX];
    uint32_t owner_uid;
    bool active;
    bool joined;
    
    cli_msg_t *msgs;
    int msgs_cap;
    int msgs_head;
    int msgs_len;
    
    int unread;
    int scroll_offset;
    char typing_nick[33];
    time_t typing_until;
} cli_room_t;

typedef struct {
    uint32_t peer_uid;
    char peer_nick[33];
    
    cli_msg_t *msgs;
    int msgs_cap;
    int msgs_head;
    int msgs_len;
    
    int unread;
    int scroll_offset;
    char typing_nick[33];
    time_t typing_until;
} cli_dm_t;

typedef struct {
    uint32_t user_id;
    char nick[33];
    char away_msg[64];
    bool is_away;
} cli_user_t;

typedef struct {
    char server_name[64];
    char motd[2048];
    uint32_t session_id;
    uint32_t user_id;
    char nick[CLI_NICK_MAX];
    char base_nick[CLI_NICK_MAX];
    bool is_away;
    char away_msg[256];
    char ignore_list[32][CLI_NICK_MAX];
    int ignore_count;
    cli_status_t status;
    
    char status_msg[128];
    time_t status_msg_until;
    
    cli_room_t rooms[64];
    int room_count;
    
    cli_dm_t dms[64];
    int dm_count;
    
    cli_user_t users[256];
    int user_count;
    
    int active_room;
    int active_dm;
} cli_model_t;

void model_init(cli_model_t *m);
void model_free(cli_model_t *m);

void model_set_status(cli_model_t *m, const char *msg, int secs);

cli_room_t *model_get_or_create_room(cli_model_t *m, const char *name);
cli_room_t *model_find_room(cli_model_t *m, const char *name);
void model_clear_room_msgs(cli_model_t *m, const char *name);

void model_push_room_msg(cli_model_t *m, const char *room, uint32_t uid, const char *nick, const char *base_nick, const char *text, uint64_t ts_ms, bool is_self, const char *my_nick);
void model_push_room_msg_typed(cli_model_t *m, const char *room, uint32_t uid, const char *nick, const char *base_nick, const char *text, uint64_t ts_ms, bool is_self, cli_msg_type_t type, const char *my_nick);
void model_push_system_msg(cli_model_t *m, const char *room_name, const char *text, uint64_t ts_ms, cli_msg_type_t type);

cli_dm_t *model_get_or_create_dm(cli_model_t *m, uint32_t uid, const char *nick);
void model_push_dm_msg(cli_model_t *m, uint32_t uid, const char *nick, const char *text, uint64_t ts_ms, bool is_self);

bool model_is_ignored(const cli_model_t *m, const char *nick);
void model_update_users(cli_model_t *m, const char *room, cli_user_t *users, int count);

#endif
