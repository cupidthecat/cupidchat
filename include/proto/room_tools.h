#pragma once
#include "proto/tlv.h"
#include <stdbool.h>

#define ROOM_OWNER 1u
#define ROOM_OPERATOR 2u
#define ROOM_ADMIN 4u
#define ROOM_INVITED 8u
#define ROOM_BANNED 16u
#define ROOM_MANAGERS (ROOM_OWNER | ROOM_OPERATOR | ROOM_ADMIN)
#define ROOM_ACCESS_PAGE 64

typedef struct {
    char account[33], inviter[33];
    uint8_t flags;
    uint64_t invited_at;
} room_access_t;

typedef struct {
    char room[65], owner[33], topic[256];
    uint32_t owner_uid;
    uint8_t role;
    bool topic_locked, more;
    room_access_t entries[ROOM_ACCESS_PAGE];
    int count;
} room_info_t;

bool room_name_valid(const char *name);
bool room_read_text(const uint8_t *p, uint32_t length, tlv_tag_t tag, char *out, size_t capacity);
bool room_read_name(const uint8_t *p, uint32_t length, char out[65]);
