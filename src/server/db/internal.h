#pragma once
#include "server/db.h"
#include <sqlite3.h>

typedef enum {
    DB_STMT_MESSAGE,
    DB_STMT_DM,
    DB_STMT_CONTROLS,
    DB_STMT_ROOM_ACCESS,
    DB_STMT_PROFILE_ID,
    DB_STMT_PROFILE_NAME,
    DB_STMT_BUDDIES,
    DB_STMT_COUNT
} db_statement_id_t;

struct cupid_db {
    sqlite3 *handle;
    sqlite3_stmt *statements[DB_STMT_COUNT];
};

/* Cache query plans, not results: permissions are read again on every request. */
static inline sqlite3_stmt *db_statement(cupid_db_t *db, db_statement_id_t id, const char *sql) {
    sqlite3_stmt **slot = &db->statements[id];
    if (!*slot &&
        sqlite3_prepare_v3(db->handle, sql, -1, SQLITE_PREPARE_PERSISTENT, slot, NULL) != SQLITE_OK)
        return NULL;
    return *slot;
}

static inline void db_statement_release(sqlite3_stmt *stmt) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
}
