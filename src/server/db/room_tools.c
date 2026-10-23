#include "internal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int db_init_room_tools(cupid_db_t *db) {
    return sqlite3_exec(db->handle,
                        "CREATE TABLE IF NOT EXISTS room_access("
                        "room_name TEXT NOT NULL REFERENCES rooms(name) ON DELETE CASCADE,"
                        "account_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
                        "reason TEXT NOT NULL DEFAULT '',"
                        "is_operator INTEGER NOT NULL DEFAULT 0,banned INTEGER NOT NULL DEFAULT 0,"
                        "inviter_id INTEGER REFERENCES users(id) ON DELETE SET NULL,invited_at "
                        "INTEGER NOT NULL DEFAULT 0,"
                        "PRIMARY KEY(room_name,account_id),CHECK(NOT(is_operator AND banned)));"
                        "CREATE INDEX IF NOT EXISTS room_access_account ON "
                        "room_access(account_id,invited_at);",
                        NULL, NULL, NULL) == SQLITE_OK
               ? 0
               : -1;
}

int db_room_access(cupid_db_t *db, const char *room, uint64_t account, room_access_t *access) {
    memset(access, 0, sizeof(*access));
    sqlite3_stmt *q = NULL;
    const char *sql =
        "SELECT a.is_operator,a.banned,a.invited_at,COALESCE(u.username,'server'),a.reason FROM "
        "room_access "
        "a LEFT JOIN users u ON u.id=a.inviter_id WHERE a.room_name=? AND a.account_id=?";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_int(q, 0))
            access->flags |= ROOM_OPERATOR;
        if (sqlite3_column_int(q, 1))
            access->flags |= ROOM_BANNED;
        access->invited_at = (uint64_t)sqlite3_column_int64(q, 2);
        if (access->invited_at)
            access->flags |= ROOM_INVITED;
        snprintf(access->inviter, sizeof(access->inviter), "%s", sqlite3_column_text(q, 3));
        snprintf(access->reason, sizeof(access->reason), "%s", sqlite3_column_text(q, 4));
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int db_room_access_list(cupid_db_t *db, const char *room, bool manager, room_info_t *info) {
    const char *sql =
        "SELECT "
        "u.username,a.is_operator,a.banned,a.invited_at,COALESCE(i.username,'server'),a.reason "
        "FROM room_access a JOIN users u ON u.id=a.account_id LEFT JOIN users i ON "
        "i.id=a.inviter_id "
        "WHERE a.room_name=? AND (a.is_operator=1 OR (? AND (a.banned=1 OR a.invited_at>0))) "
        "ORDER BY a.is_operator DESC,a.banned DESC,u.username COLLATE NOCASE LIMIT 65";
    sqlite3_stmt *q = NULL;
    info->count = 0;
    info->more = false;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_int(q, 2, manager);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        if (info->count == ROOM_ACCESS_PAGE) {
            info->more = true;
            break;
        }
        room_access_t *a = &info->entries[info->count++];
        memset(a, 0, sizeof(*a));
        snprintf(a->account, sizeof(a->account), "%s", sqlite3_column_text(q, 0));
        if (sqlite3_column_int(q, 1))
            a->flags |= ROOM_OPERATOR;
        if (sqlite3_column_int(q, 2))
            a->flags |= ROOM_BANNED;
        a->invited_at = (uint64_t)sqlite3_column_int64(q, 3);
        if (a->invited_at)
            a->flags |= ROOM_INVITED;
        snprintf(a->inviter, sizeof(a->inviter), "%s", sqlite3_column_text(q, 4));
        snprintf(a->reason, sizeof(a->reason), "%s", sqlite3_column_text(q, 5));
        if (!manager) {
            a->flags &= ROOM_OPERATOR;
            a->invited_at = 0;
            a->inviter[0] = a->reason[0] = '\0';
        }
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int db_room_access_update(cupid_db_t *db, const char *room, uint64_t account, const char *action,
                          const char *reason) {
    const char *sql;
    if (!strcmp(action, "op"))
        sql = "INSERT INTO room_access(room_name,account_id,is_operator) VALUES(?,?,1) ON "
              "CONFLICT(room_name,account_id) DO UPDATE SET is_operator=1 WHERE banned=0";
    else if (!strcmp(action, "deop"))
        sql = "UPDATE room_access SET is_operator=0 WHERE room_name=? AND account_id=?";
    else if (!strcmp(action, "ban"))
        sql = "INSERT INTO room_access(room_name,account_id,banned,reason) VALUES(?,?,1,?) ON "
              "CONFLICT(room_name,account_id) DO UPDATE SET "
              "banned=1,is_operator=0,reason=excluded.reason,inviter_id=NULL,invited_at=0";
    else if (!strcmp(action, "unban"))
        sql = "UPDATE room_access SET banned=0,reason='' WHERE room_name=? AND account_id=?";
    else
        return -1;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    if (!strcmp(action, "ban"))
        sqlite3_bind_text(q, 3, reason, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    int changed = sqlite3_changes(db->handle);
    sqlite3_finalize(q);
    if (rc != SQLITE_DONE)
        return -1;
    return !strcmp(action, "op") && !changed ? -2 : 0;
}

int db_room_invite(cupid_db_t *db, const char *room, uint64_t target, uint64_t inviter) {
    if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int result = -1;
    sqlite3_stmt *q = NULL;
    room_access_t old;
    if (db_room_access(db, room, target, &old) < 0)
        goto done;
    if (old.flags & ROOM_BANNED) {
        result = -2;
        goto done;
    }
    uint64_t now = (uint64_t)time(NULL) * 1000;
    if (old.invited_at && now < old.invited_at + 30000) {
        result = -3;
        goto done;
    }
    if (sqlite3_prepare_v2(db->handle,
                           "SELECT COUNT(*) FROM room_access WHERE account_id=? AND invited_at>0",
                           -1, &q, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)target);
    int rc = sqlite3_step(q);
    int count = rc == SQLITE_ROW ? sqlite3_column_int(q, 0) : 0;
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_ROW)
        goto done;
    if (!old.invited_at && count >= 64) {
        result = -4;
        goto done;
    }
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO room_access(room_name,account_id,inviter_id,invited_at) "
                           "VALUES(?,?,?,?) ON CONFLICT(room_name,account_id) DO UPDATE SET "
                           "inviter_id=excluded.inviter_id,invited_at=excluded.invited_at",
                           -1, &q, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(q, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)target);
    sqlite3_bind_int64(q, 3, (sqlite3_int64)inviter);
    sqlite3_bind_int64(q, 4, (sqlite3_int64)now);
    if (sqlite3_step(q) == SQLITE_DONE)
        result = 0;
done:
    sqlite3_finalize(q);
    if (!result && sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
        return 0;
    sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return result ? result : -1;
}

int db_room_clear_invite(cupid_db_t *db, const char *room, uint64_t account) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "UPDATE room_access SET inviter_id=NULL,invited_at=0 WHERE room_name=? "
                           "AND account_id=?",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, room, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_room_invites(cupid_db_t *db, uint64_t account, db_invite_cb callback, void *ctx) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "SELECT a.room_name,COALESCE(u.username,'server') FROM room_access a "
                           "LEFT JOIN users u ON u.id=a.inviter_id WHERE a.account_id=? AND "
                           "a.invited_at>0 AND a.banned=0 ORDER BY a.invited_at DESC LIMIT 64",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW)
        callback(ctx, (const char *)sqlite3_column_text(q, 0),
                 (const char *)sqlite3_column_text(q, 1));
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_room_topic_lock(cupid_db_t *db, const char *room, bool enabled) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle, "UPDATE rooms SET topic_locked=? WHERE name=?", -1, &q,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(q, 1, enabled);
    sqlite3_bind_text(q, 2, room, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1 ? 0 : -1;
}
