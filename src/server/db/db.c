/*
 * src/server/db/db.c -  SQLite persistence for cupid-chatd
 *
 * Schema
 *  rooms    (id, name UNIQUE, topic, created_at)
 *  messages (id, room_name, user_id, nick, text, ts_ms)
 *  dms      (id, from_uid, from_nick, to_uid, to_nick, text, ts_ms)
 *
 * WAL mode + NORMAL synchronous: fast writes, crash-safe on Linux.
 */

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <crypt.h>
#include <errno.h>

#include "server/db.h"
#include "server/log.h"

/* Internal structure */

struct cupid_db {
    sqlite3 *handle;
};

/* Schema */

static const char SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS rooms ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT    NOT NULL UNIQUE,"
    "  topic      TEXT    NOT NULL DEFAULT '',"
    "  owner_uid  INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS messages ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  room_name TEXT    NOT NULL,"
    "  user_id   INTEGER NOT NULL,"
    "  nick      TEXT    NOT NULL,"
    "  text      TEXT    NOT NULL,"
    "  ts_ms     INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_messages_room_ts"
    "  ON messages(room_name, ts_ms);"

    "CREATE TABLE IF NOT EXISTS dms ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  from_uid   INTEGER NOT NULL,"
    "  from_nick  TEXT    NOT NULL,"
    "  to_uid     INTEGER NOT NULL,"
    "  to_nick    TEXT    NOT NULL,"
    "  text       TEXT    NOT NULL,"
    "  ts_ms      INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_dms_to_uid"
    "  ON dms(to_uid, ts_ms);"

    "CREATE TABLE IF NOT EXISTS users ("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  username      TEXT    NOT NULL UNIQUE COLLATE NOCASE,"
    "  password_hash TEXT    NOT NULL,"
    "  created_at    INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_users_username"
    "  ON users(username COLLATE NOCASE);";

/* Rooms that are always present in a fresh installation. */
static const char *DEFAULT_ROOMS[] = { "general", "random", NULL };

/* Lifecycle */

cupid_db_t *db_open(const char *path) {
    cupid_db_t *db = calloc(1, sizeof(*db));
    if (!db) return NULL;

    int rc = sqlite3_open(path, &db->handle);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db_open '%s': %s", path, sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        free(db);
        return NULL;
    }

    /* WAL = non-blocking readers, safe concurrent writes */
    sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;",       NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA synchronous=NORMAL;",     NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON;",        NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA cache_size=-8000;",       NULL, NULL, NULL);
    /* Return SQLITE_BUSY after 5 s rather than immediately, so brief lock
     * contention (e.g. an external sqlite3 shell) doesn't corrupt writes. */
    sqlite3_exec(db->handle, "PRAGMA busy_timeout=5000;",      NULL, NULL, NULL);

    LOG_INFO("database opened: %s", path);
    return db;
}

void db_init_schema(cupid_db_t *db) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db->handle, SCHEMA, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db_init_schema: %s", errmsg);
        sqlite3_free(errmsg);
    } else {
        LOG_DEBUG("%s", "schema ok");
    }
    /* Migration: add owner_uid if upgrading from an older DB that lacks it.
     * ALTER TABLE fails silently when the column already exists. */
    sqlite3_exec(db->handle,
        "ALTER TABLE rooms ADD COLUMN owner_uid INTEGER NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);
}

void db_seed_defaults(cupid_db_t *db) {
    const char *sql =
        "INSERT OR IGNORE INTO rooms(name, topic, created_at)"
        " VALUES(?, '', CAST(strftime('%s','now') AS INTEGER) * 1000);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("db_seed_defaults prepare: %s", sqlite3_errmsg(db->handle));
        return;
    }

    for (int i = 0; DEFAULT_ROOMS[i] != NULL; i++) {
        sqlite3_bind_text(stmt, 1, DEFAULT_ROOMS[i], -1, SQLITE_STATIC);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE && sqlite3_changes(db->handle) > 0)
            LOG_INFO("seeded default room #%s", DEFAULT_ROOMS[i]);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    sqlite3_finalize(stmt);
}

void db_close(cupid_db_t *db) {
    if (!db) return;
    sqlite3_close(db->handle);
    free(db);
}

/* Writes */

void db_persist_room(cupid_db_t *db, const char *name, const char *topic,
                     uint32_t owner_uid) {
    /* INSERT new row or UPDATE topic/owner on conflict so that calls from
     * handle_set_topic actually persist the latest topic to disk. */
    const char *sql =
        "INSERT INTO rooms(name, topic, owner_uid, created_at)"
        " VALUES(?, ?, ?, CAST(strftime('%s','now') AS INTEGER) * 1000)"
        " ON CONFLICT(name) DO UPDATE SET"
        "   topic     = excluded.topic,"
        "   owner_uid = excluded.owner_uid;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text (stmt, 1, name,             -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 2, topic ? topic : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)owner_uid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_delete_room(cupid_db_t *db, const char *name) {
    /* Wrap both DELETEs in a transaction so a crash between them cannot
     * leave orphaned message rows for a room that no longer exists. */
    sqlite3_exec(db->handle, "BEGIN;", NULL, NULL, NULL);

    const char *del_msgs = "DELETE FROM messages WHERE room_name = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, del_msgs, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    const char *del_room = "DELETE FROM rooms WHERE name = ?;";
    if (sqlite3_prepare_v2(db->handle, del_room, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_exec(db->handle, "COMMIT;", NULL, NULL, NULL);
    LOG_INFO("deleted room #%s from db", name);
}

void db_persist_message(cupid_db_t *db,
                        const char *room_name,
                        uint32_t    user_id,
                        const char *nick,
                        const char *text,
                        uint64_t    ts_ms) {
    const char *sql =
        "INSERT INTO messages(room_name, user_id, nick, text, ts_ms)"
        " VALUES(?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text (stmt, 1, room_name,       -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)user_id);
    sqlite3_bind_text (stmt, 3, nick,            -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 4, text,            -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (int64_t)ts_ms);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_persist_dm(cupid_db_t *db,
                   uint32_t    from_uid,
                   const char *from_nick,
                   uint32_t    to_uid,
                   const char *to_nick,
                   const char *text,
                   uint64_t    ts_ms) {
    const char *sql =
        "INSERT INTO dms(from_uid, from_nick, to_uid, to_nick, text, ts_ms)"
        " VALUES(?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, (int64_t)from_uid);
    sqlite3_bind_text (stmt, 2, from_nick, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)to_uid);
    sqlite3_bind_text (stmt, 4, to_nick,   -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 5, text,      -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (int64_t)ts_ms);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* Reads */

void db_load_rooms(cupid_db_t *db, db_room_cb cb, void *userdata) {
    const char *sql =
        "SELECT name, topic, owner_uid FROM rooms ORDER BY id ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("db_load_rooms: %s", sqlite3_errmsg(db->handle));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name      = (const char *)sqlite3_column_text(stmt, 0);
        const char *topic     = (const char *)sqlite3_column_text(stmt, 1);
        uint32_t    owner_uid = (uint32_t)sqlite3_column_int64(stmt, 2);
        if (name) cb(userdata, name, topic ? topic : "", owner_uid);
    }
    sqlite3_finalize(stmt);
}

void db_load_room_history(cupid_db_t *db,
                          const char *room_name,
                          int         limit,
                          db_msg_cb   cb,
                          void       *userdata) {
    /*
     * Sub-select: take the most-recent `limit` rows DESC, then re-order them
     * ASC so the callback receives them oldest-first (natural chat order).
 */
    const char *sql =
        "SELECT user_id, nick, text, ts_ms FROM ("
        "  SELECT user_id, nick, text, ts_ms"
        "  FROM   messages"
        "  WHERE  room_name = ?"
        "  ORDER  BY ts_ms DESC"
        "  LIMIT  ?"
        ") ORDER BY ts_ms ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("db_load_room_history: %s", sqlite3_errmsg(db->handle));
        return;
    }
    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint32_t    uid  = (uint32_t)sqlite3_column_int64(stmt, 0);
        const char *nick = (const char *)sqlite3_column_text(stmt, 1);
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        uint64_t    ts   = (uint64_t)sqlite3_column_int64(stmt, 3);
        if (nick && text) cb(userdata, uid, nick, text, ts);
    }
    sqlite3_finalize(stmt);
}

/* User auth */

static void make_crypt_salt(char *buf, size_t buflen) {
    /* $6$ = SHA-512 glibc crypt.  Fill 8 random bytes from /dev/urandom
     * to produce a salt of the form "$6$XXXXXXXX$". */
    static const char chars[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789./";
    unsigned char rnd[8] = {0};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t off = 0;
        while (off < sizeof(rnd)) {
            ssize_t n = read(fd, rnd + off, sizeof(rnd) - off);
            if (n > 0) {
                off += (size_t)n;
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        if (off < sizeof(rnd)) {
            /* fallback-mix if urandom read was short/interrupted */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            unsigned v = (unsigned)(ts.tv_nsec ^ (unsigned long)ts.tv_sec ^ (unsigned)getpid());
            for (size_t i = off; i < sizeof(rnd); i++) {
                v = v * 1103515245u + 12345u;
                rnd[i] = (unsigned char)(v >> 16);
            }
        }
        close(fd);
    } else {
        /* fallback: mix pid + nanosecond time (weaker, logged as warning) */
        LOG_WARN("%s", "make_crypt_salt: /dev/urandom unavailable, using weak fallback");
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned v = (unsigned)(ts.tv_nsec ^ (unsigned long)ts.tv_sec ^ (unsigned)getpid());
        for (int i = 0; i < 8; i++) { v = v * 1103515245u + 12345u; rnd[i] = (unsigned char)(v >> 16); }
    }
    buf[0] = '$'; buf[1] = '6'; buf[2] = '$';
    for (int i = 0; i < 8 && (size_t)(3 + i) < buflen - 2; i++)
        buf[3 + i] = chars[rnd[i] & 63u];
    buf[11] = '$';
    buf[12] = '\0';
}

bool db_register_user(cupid_db_t *db, const char *username, const char *password) {
    char salt[16];
    make_crypt_salt(salt, sizeof(salt));

    struct crypt_data cd;
    memset(&cd, 0, sizeof(cd));
    char *hash = crypt_r(password, salt, &cd);
    if (!hash) return false;

    const char *sql =
        "INSERT INTO users(username, password_hash, created_at)"
        " VALUES(?, ?, CAST(strftime('%s','now') AS INTEGER) * 1000);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("db_register_user prepare: %s", sqlite3_errmsg(db->handle));
        return false;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash,     -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        LOG_INFO("registered new user: %s", username);
        return true;
    } else if (rc == SQLITE_CONSTRAINT) {
        return false;   /* UNIQUE violation -  account already exists */
    } else {
        LOG_ERROR("db_register_user step (%d): %s", rc, sqlite3_errmsg(db->handle));
        return false;
    }
}

bool db_authenticate_user(cupid_db_t *db, const char *username, const char *password) {
    const char *sql =
        "SELECT password_hash FROM users WHERE username = ? COLLATE NOCASE;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(stmt, 0);
        if (stored) {
            struct crypt_data cd;
            memset(&cd, 0, sizeof(cd));
            char *hash = crypt_r(password, stored, &cd);
            if (hash && strcmp(hash, stored) == 0)
                ok = true;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}
