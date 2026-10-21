/* CupidChat public interface. */
#pragma once
#include "proto/types.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_ROOMS 256
#define ROOM_MEMBER_SLOTS 65535

typedef struct {
    uint32_t user_id;
    uint64_t timestamp_ms;
    char nick[MAX_NICK_LEN + 1], text[MAX_MSG_LEN + 1];
} history_entry_t;

typedef struct {
    char name[MAX_ROOM_LEN + 1], topic[256], topic_set_by[MAX_NICK_LEN + 1];
    bool active;
    uint32_t owner_uid;
    int member_fds[ROOM_MEMBER_SLOTS], member_count;
    history_entry_t *history;
    int history_cap, history_head, history_len;
} room_t;

room_t *room_create(const char *name, int history_cap);
void room_destroy(room_t *r);
bool room_add_member(room_t *r, int fd);
bool room_remove_member(room_t *r, int fd);
bool room_has_member(const room_t *r, int fd);
void room_history_push(room_t *r, uint32_t uid, const char *nick, const char *text, uint64_t ts_ms);
