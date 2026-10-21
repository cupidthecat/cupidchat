/* CupidChat public interface. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct cupid_db cupid_db_t;
typedef void (*db_room_cb)(void *, const char *, const char *, uint32_t);
typedef void (*db_msg_cb)(void *, uint32_t, const char *, const char *, uint64_t);

cupid_db_t *db_open(const char *path);
void db_init_schema(cupid_db_t *db);
void db_seed_defaults(cupid_db_t *db);
void db_close(cupid_db_t *db);
void db_persist_room(cupid_db_t *db, const char *name, const char *topic,
                     uint32_t owner_uid);
void db_delete_room(cupid_db_t *db, const char *name);
void db_persist_message(cupid_db_t *db,
                        const char *room_name,
                        uint32_t    user_id,
                        const char *nick,
                        const char *text,
                        uint64_t    ts_ms);
void db_persist_dm(cupid_db_t *db,
                   uint32_t    from_uid,
                   const char *from_nick,
                   uint32_t    to_uid,
                   const char *to_nick,
                   const char *text,
                   uint64_t    ts_ms);
void db_load_rooms(cupid_db_t *db, db_room_cb cb, void *userdata);
void db_load_room_history(cupid_db_t *db,
                          const char *room_name,
                          int         limit,
                          db_msg_cb   cb,
                          void       *userdata);
bool db_register_user(cupid_db_t *db, const char *username, const char *password);
bool db_authenticate_user(cupid_db_t *db, const char *username, const char *password);
