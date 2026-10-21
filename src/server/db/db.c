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

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "proto/types.h"
#include "server/db.h"
#include "server/log.h"

/* Internal structure */

struct cupid_db {
    sqlite3 *handle;
};

/* Schema */

static const char SCHEMA[] = "CREATE TABLE IF NOT EXISTS rooms ("
                             "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
                             "  name       TEXT    NOT NULL UNIQUE,"
                             "  topic      TEXT    NOT NULL DEFAULT '',"
                             "  owner_uid  INTEGER NOT NULL DEFAULT 0,"
                             "  owner_account_id INTEGER REFERENCES users(id),"
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
                             "  ON users(username COLLATE NOCASE);"
                             "CREATE TABLE IF NOT EXISTS buddies ("
                             " owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                             " buddy_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                             " PRIMARY KEY(owner_id, buddy_id), CHECK(owner_id != buddy_id)"
                             ");";

/* Rooms that are always present in a fresh installation. */
static const char *DEFAULT_ROOMS[] = {"general", "random", NULL};

/* Lifecycle */

cupid_db_t *db_open(const char *path) {
    cupid_db_t *db = calloc(1, sizeof(*db));
    if (!db)
        return NULL;

    int rc = sqlite3_open(path, &db->handle);
    if (rc != SQLITE_OK) {
        LOG_ERROR("db_open '%s': %s", path, sqlite3_errmsg(db->handle));
        sqlite3_close(db->handle);
        free(db);
        return NULL;
    }

    /* WAL = non-blocking readers, safe concurrent writes */
    sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    sqlite3_exec(db->handle, "PRAGMA cache_size=-8000;", NULL, NULL, NULL);
    /* Return SQLITE_BUSY after 5 s rather than immediately, so brief lock
     * contention (e.g. an external sqlite3 shell) doesn't corrupt writes. */
    sqlite3_exec(db->handle, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    LOG_INFO("database opened: %s", path);
    return db;
}

static int ensure_room_column(cupid_db_t *db, const char *name, const char *alter) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, "PRAGMA table_info(rooms)", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    bool found = false;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *column = (const char *)sqlite3_column_text(stmt, 1);
        if (column && !strcmp(column, name))
            found = true;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return -1;
    return found || sqlite3_exec(db->handle, alter, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int db_init_schema(cupid_db_t *db) {
    if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_exec(db->handle, SCHEMA, NULL, NULL, NULL) != SQLITE_OK ||
        ensure_room_column(db, "owner_uid",
                           "ALTER TABLE rooms ADD COLUMN owner_uid INTEGER NOT NULL DEFAULT 0") <
            0 ||
        ensure_room_column(
            db, "owner_account_id",
            "ALTER TABLE rooms ADD COLUMN owner_account_id INTEGER REFERENCES users(id)") < 0 ||
        sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_ERROR("db_init_schema: %s", sqlite3_errmsg(db->handle));
        sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

void db_seed_defaults(cupid_db_t *db) {
    const char *sql = "INSERT OR IGNORE INTO rooms(name, topic, created_at)"
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
    if (!db)
        return;
    sqlite3_close(db->handle);
    free(db);
}

/* Writes */

int db_create_room(cupid_db_t *db, const char *name, uint64_t owner_account_id) {
    /* Legacy owner_uid values stay available for an operator to review.
     * New rows use zero there; connection IDs never grant persistent ownership. */
    const char *sql = "INSERT INTO rooms(name, topic, owner_uid, owner_account_id, created_at)"
                      " VALUES(?, '', 0, ?, CAST(strftime('%s','now') AS INTEGER) * 1000);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (owner_account_id)
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)owner_account_id);
    else
        sqlite3_bind_null(stmt, 2);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        LOG_ERROR("db_create_room: %s", sqlite3_errmsg(db->handle));
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_set_room_topic(cupid_db_t *db, const char *name, const char *topic) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, "UPDATE rooms SET topic = ? WHERE name = ?", -1, &stmt,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, topic, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1 ? 0 : -1;
}

int db_delete_room(cupid_db_t *db, const char *name) {
    if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    const char *queries[] = {"DELETE FROM messages WHERE room_name = ?",
                             "DELETE FROM rooms WHERE name = ?"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        int rc = sqlite3_prepare_v2(db->handle, queries[i], -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            rc = sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            LOG_ERROR("db_delete_room: %s", sqlite3_errmsg(db->handle));
            sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
    }
    if (sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}

void db_persist_message(cupid_db_t *db, const char *room_name, uint32_t user_id, const char *nick,
                        const char *text, uint64_t ts_ms) {
    const char *sql = "INSERT INTO messages(room_name, user_id, nick, text, ts_ms)"
                      " VALUES(?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, room_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (int64_t)user_id);
    sqlite3_bind_text(stmt, 3, nick, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, text, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (int64_t)ts_ms);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_persist_dm(cupid_db_t *db, uint32_t from_uid, const char *from_nick, uint32_t to_uid,
                   const char *to_nick, const char *text, uint64_t ts_ms) {
    const char *sql = "INSERT INTO dms(from_uid, from_nick, to_uid, to_nick, text, ts_ms)"
                      " VALUES(?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, (int64_t)from_uid);
    sqlite3_bind_text(stmt, 2, from_nick, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, (int64_t)to_uid);
    sqlite3_bind_text(stmt, 4, to_nick, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, text, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (int64_t)ts_ms);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/* Reads */

void db_load_rooms(cupid_db_t *db, db_room_cb cb, void *userdata) {
    const char *sql = "SELECT name, topic, owner_account_id FROM rooms ORDER BY id ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("db_load_rooms: %s", sqlite3_errmsg(db->handle));
        return;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *topic = (const char *)sqlite3_column_text(stmt, 1);
        uint64_t owner_account_id = (uint64_t)sqlite3_column_int64(stmt, 2);
        if (name)
            cb(userdata, name, topic ? topic : "", owner_account_id);
    }
    sqlite3_finalize(stmt);
}

void db_load_room_history(cupid_db_t *db, const char *room_name, int limit, db_msg_cb cb,
                          void *userdata) {
    /*
     * Sub-select: take the most-recent `limit` rows DESC, then re-order them
     * ASC so the callback receives them oldest-first (natural chat order).
     */
    const char *sql = "SELECT user_id, nick, text, ts_ms FROM ("
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
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint32_t uid = (uint32_t)sqlite3_column_int64(stmt, 0);
        const char *nick = (const char *)sqlite3_column_text(stmt, 1);
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        uint64_t ts = (uint64_t)sqlite3_column_int64(stmt, 3);
        if (nick && text)
            cb(userdata, uid, nick, text, ts);
    }
    sqlite3_finalize(stmt);
}

/* User auth */

static void make_crypt_salt(char *buf, size_t buflen) {
    /* $6$ = SHA-512 glibc crypt.  Fill 8 random bytes from /dev/urandom
     * to produce a salt of the form "$6$XXXXXXXX$". */
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz"
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
            if (n == 0)
                break;
            if (errno == EINTR)
                continue;
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
        for (int i = 0; i < 8; i++) {
            v = v * 1103515245u + 12345u;
            rnd[i] = (unsigned char)(v >> 16);
        }
    }
    buf[0] = '$';
    buf[1] = '6';
    buf[2] = '$';
    for (int i = 0; i < 8 && (size_t)(3 + i) < buflen - 2; i++)
        buf[3 + i] = chars[rnd[i] & 63u];
    buf[11] = '$';
    buf[12] = '\0';
}

uint64_t db_register_account(cupid_db_t *db, const char *username, const char *password) {
    char salt[16];
    make_crypt_salt(salt, sizeof(salt));

    struct crypt_data cd;
    memset(&cd, 0, sizeof(cd));
    char *hash = crypt_r(password, salt, &cd);
    if (!hash)
        return 0;

    const char *sql = "INSERT INTO users(username, password_hash, created_at)"
                      " VALUES(?, ?, CAST(strftime('%s','now') AS INTEGER) * 1000);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_ERROR("db_register_user prepare: %s", sqlite3_errmsg(db->handle));
        return 0;
    }
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        LOG_INFO("registered new user: %s", username);
        return (uint64_t)sqlite3_last_insert_rowid(db->handle);
    } else if (rc == SQLITE_CONSTRAINT) {
        return 0; /* UNIQUE violation -  account already exists */
    } else {
        LOG_ERROR("db_register_user step (%d): %s", rc, sqlite3_errmsg(db->handle));
        return 0;
    }
}

uint64_t db_authenticate_account(cupid_db_t *db, const char *username, const char *password) {
    const char *sql = "SELECT password_hash, id FROM users WHERE username = ? COLLATE NOCASE;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    uint64_t account_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(stmt, 0);
        if (stored) {
            struct crypt_data cd;
            memset(&cd, 0, sizeof(cd));
            char *hash = crypt_r(password, stored, &cd);
            if (hash && strcmp(hash, stored) == 0)
                account_id = (uint64_t)sqlite3_column_int64(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
    return account_id;
}

bool db_register_user(cupid_db_t *db, const char *username, const char *password) {
    return db_register_account(db, username, password) != 0;
}

bool db_authenticate_user(cupid_db_t *db, const char *username, const char *password) {
    return db_authenticate_account(db, username, password) != 0;
}

int db_account_name(cupid_db_t *db, uint64_t account, char *name, int capacity) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT username FROM users WHERE id=?", -1, &stmt, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)account);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        snprintf(name, (size_t)capacity, "%s", sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

int db_list_buddies(cupid_db_t *db, uint64_t owner, db_buddy_cb cb, void *ctx) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT u.id,u.username FROM buddies b JOIN users u ON u.id=b.buddy_id "
                      "WHERE b.owner_id=? ORDER BY u.username COLLATE NOCASE";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)owner);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        cb(ctx, (uint64_t)sqlite3_column_int64(stmt, 0),
           (const char *)sqlite3_column_text(stmt, 1));
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_update_buddy(cupid_db_t *db, uint64_t owner, const char *name, bool remove) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT id FROM users WHERE username=? COLLATE NOCASE", -1,
                           &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    uint64_t buddy = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    if (!buddy)
        return rc == SQLITE_DONE ? -2 : -1;
    if (owner == buddy)
        return -4;
    if (!remove) {
        const char *sql =
            "SELECT COUNT(*),COALESCE(MAX(buddy_id=?),0) FROM buddies WHERE owner_id=?";
        if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)buddy);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)owner);
        rc = sqlite3_step(stmt);
        int count = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : MAX_BUDDIES;
        int exists = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 1) : 0;
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW)
            return -1;
        if (exists)
            return 0;
        if (count >= MAX_BUDDIES)
            return -3;
    }
    const char *sql = remove ? "DELETE FROM buddies WHERE owner_id=? AND buddy_id=?"
                             : "INSERT INTO buddies(owner_id,buddy_id) VALUES(?,?)";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)owner);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)buddy);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}
