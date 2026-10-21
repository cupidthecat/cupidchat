/* CupidChat public interface. */
#pragma once
#include "proto/admin.h"
#include "proto/profile.h"
#include <stdbool.h>
#include <stdint.h>
typedef struct cupid_db cupid_db_t;
typedef void (*db_room_cb)(void *, const char *, const char *, uint64_t);
typedef void (*db_msg_cb)(void *, uint32_t, const char *, const char *, uint64_t);

cupid_db_t *db_open(const char *path);
int db_init_schema(cupid_db_t *db);
void db_seed_defaults(cupid_db_t *db);
void db_close(cupid_db_t *db);
int db_create_room(cupid_db_t *db, const char *name, uint64_t owner_account_id);
int db_set_room_topic(cupid_db_t *db, const char *name, const char *topic);
int db_delete_room(cupid_db_t *db, const char *name);
void db_persist_message(cupid_db_t *db, const char *room_name, uint32_t user_id, const char *nick,
                        const char *text, uint64_t ts_ms);
void db_persist_dm(cupid_db_t *db, uint32_t from_uid, const char *from_nick, uint32_t to_uid,
                   const char *to_nick, const char *text, uint64_t ts_ms);
void db_load_rooms(cupid_db_t *db, db_room_cb cb, void *userdata);
void db_load_room_history(cupid_db_t *db, const char *room_name, int limit, db_msg_cb cb,
                          void *userdata);
bool db_register_user(cupid_db_t *db, const char *username, const char *password);
bool db_authenticate_user(cupid_db_t *db, const char *username, const char *password);
uint64_t db_register_account(cupid_db_t *db, const char *username, const char *password);
uint64_t db_authenticate_account(cupid_db_t *db, const char *username, const char *password);
typedef void (*db_buddy_cb)(void *, uint64_t, const char *);
int db_list_buddies(cupid_db_t *db, uint64_t owner, db_buddy_cb cb, void *ctx);
int db_update_buddy(cupid_db_t *db, uint64_t owner, const char *name, bool remove);
int db_account_name(cupid_db_t *db, uint64_t account, char *name, int capacity);
uint64_t db_find_account(cupid_db_t *db, const char *name);
int db_get_profile(cupid_db_t *db, uint64_t account, const char *name, profile_info_t *info,
                   uint64_t *found_account);
int db_set_profile(cupid_db_t *db, uint64_t account, const profile_fields_t *fields);

int db_init_admin(cupid_db_t *db);
int db_get_controls(cupid_db_t *db, uint64_t account, account_controls_t *flags);
int db_control_account(cupid_db_t *db, uint64_t actor, uint64_t target, const char *action,
                       const char *reason);
int db_get_setting(cupid_db_t *db, const char *key, char *value, size_t capacity);
int db_set_setting(cupid_db_t *db, const char *key, const char *value);
int db_get_poll(cupid_db_t *db, uint64_t id, uint64_t account, poll_info_t *poll);
int db_create_poll(cupid_db_t *db, uint64_t actor, const poll_info_t *poll);
int db_vote_poll(cupid_db_t *db, uint64_t id, uint64_t account, uint32_t choice);
int db_close_poll(cupid_db_t *db);
