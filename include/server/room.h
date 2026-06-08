#ifndef SERVER_ROOM_H
#define SERVER_ROOM_H

#include <stdbool.h>
#include <stdint.h>

#define ROOM_MEMBER_SLOTS 256

typedef struct {
    uint32_t user_id;
    uint64_t timestamp_ms;
    char nick[33];
    char text[4096];
} history_entry_t;

typedef struct {
    char name[65];
    char topic[256];
    char topic_set_by[33];
    uint32_t owner_uid;
    bool active;
    
    int member_fds[ROOM_MEMBER_SLOTS];
    int member_count;
    
    history_entry_t *history;
    int history_cap;
    int history_head;
    int history_len;
} room_t;

room_t *room_create(const char *name, int history_cap);
void room_destroy(room_t *r);

bool room_add_member(room_t *r, int fd);
bool room_remove_member(room_t *r, int fd);
bool room_has_member(const room_t *r, int fd);

void room_history_push(room_t *r, uint32_t uid, const char *nick,
                       const char *text, uint64_t ts_ms);

#endif
