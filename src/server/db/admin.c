#include "internal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int db_init_admin(cupid_db_t *db) {
    return sqlite3_exec(
               db->handle,
               "CREATE TABLE IF NOT EXISTS account_controls("
               "user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,"
               "admin INTEGER NOT NULL DEFAULT 0,muted INTEGER NOT NULL DEFAULT 0,"
               "banned INTEGER NOT NULL DEFAULT 0,reason TEXT NOT NULL DEFAULT '');"
               "CREATE TABLE IF NOT EXISTS server_settings(key TEXT PRIMARY KEY,value TEXT NOT "
               "NULL);"
               "CREATE TABLE IF NOT EXISTS admin_actions(id INTEGER PRIMARY KEY,actor_id INTEGER "
               "NOT NULL,"
               "target_id INTEGER NOT NULL,action TEXT NOT NULL,reason TEXT NOT NULL,created_at "
               "INTEGER NOT NULL);"
               "CREATE TABLE IF NOT EXISTS polls(id INTEGER PRIMARY KEY,question TEXT NOT NULL,"
               "creator_id INTEGER NOT NULL,closed INTEGER NOT NULL DEFAULT 0);"
               "CREATE TABLE IF NOT EXISTS poll_options(poll_id INTEGER NOT NULL REFERENCES "
               "polls(id),"
               "number INTEGER NOT NULL,label TEXT NOT NULL,PRIMARY KEY(poll_id,number));"
               "CREATE TABLE IF NOT EXISTS poll_votes(poll_id INTEGER NOT NULL,account_id INTEGER "
               "NOT NULL REFERENCES users(id),"
               "choice INTEGER NOT NULL,PRIMARY KEY(poll_id,account_id),"
               "FOREIGN KEY(poll_id,choice) REFERENCES poll_options(poll_id,number));",
               NULL, NULL, NULL) == SQLITE_OK
               ? 0
               : -1;
}

int db_get_controls(cupid_db_t *db, uint64_t account, account_controls_t *flags) {
    memset(flags, 0, sizeof(*flags));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "SELECT admin,muted,banned,reason FROM account_controls WHERE user_id=?",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        flags->admin = sqlite3_column_int(q, 0) != 0;
        flags->muted = sqlite3_column_int(q, 1) != 0;
        flags->banned = sqlite3_column_int(q, 2) != 0;
        snprintf(flags->reason, sizeof(flags->reason), "%s", sqlite3_column_text(q, 3));
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int db_control_account(cupid_db_t *db, uint64_t actor, uint64_t target, const char *action,
                       const char *reason) {
    if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int result = -1;
    sqlite3_stmt *q = NULL;
    account_controls_t flags;
    if (db_get_controls(db, target, &flags) < 0)
        goto done;
    if (!strcmp(action, "grant"))
        flags.admin = true;
    else if (!strcmp(action, "revoke")) {
        if (flags.admin) {
            if (sqlite3_prepare_v2(db->handle,
                                   "SELECT COUNT(*) FROM account_controls WHERE admin=1", -1, &q,
                                   NULL) != SQLITE_OK)
                goto done;
            int rc = sqlite3_step(q);
            int count = rc == SQLITE_ROW ? sqlite3_column_int(q, 0) : 0;
            sqlite3_finalize(q);
            q = NULL;
            if (count <= 1) {
                result = -2;
                goto done;
            }
        }
        flags.admin = false;
    } else if (!strcmp(action, "mute"))
        flags.muted = true;
    else if (!strcmp(action, "unmute"))
        flags.muted = false;
    else if (!strcmp(action, "ban"))
        flags.banned = true;
    else if (!strcmp(action, "unban"))
        flags.banned = false;
    else if (strcmp(action, "kick"))
        goto done;
    if (sqlite3_prepare_v2(
            db->handle,
            "INSERT INTO account_controls(user_id,admin,muted,banned,reason) VALUES(?,?,?,?,?) "
            "ON CONFLICT(user_id) DO UPDATE SET "
            "admin=excluded.admin,muted=excluded.muted,banned=excluded.banned,reason=excluded."
            "reason",
            -1, &q, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)target);
    sqlite3_bind_int(q, 2, flags.admin);
    sqlite3_bind_int(q, 3, flags.muted);
    sqlite3_bind_int(q, 4, flags.banned);
    sqlite3_bind_text(q, 5, reason, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_DONE)
        goto done;
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO admin_actions(actor_id,target_id,action,reason,created_at) "
                           "VALUES(?,?,?,?,?)",
                           -1, &q, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)actor);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)target);
    sqlite3_bind_text(q, 3, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 4, reason, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 5, (sqlite3_int64)time(NULL));
    if (sqlite3_step(q) == SQLITE_DONE)
        result = 0;
done:
    sqlite3_finalize(q);
    if (result == 0 && sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
        return 0;
    sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return result < 0 ? result : -1;
}

int db_get_setting(cupid_db_t *db, const char *key, char *value, size_t capacity) {
    sqlite3_stmt *q = NULL;
    value[0] = '\0';
    if (sqlite3_prepare_v2(db->handle, "SELECT value FROM server_settings WHERE key=?", -1, &q,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW)
        snprintf(value, capacity, "%s", sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int db_set_setting(cupid_db_t *db, const char *key, const char *value) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO server_settings(key,value) VALUES(?,?) ON CONFLICT(key) DO "
                           "UPDATE SET value=excluded.value",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(q, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_get_poll(cupid_db_t *db, uint64_t id, uint64_t account, poll_info_t *poll) {
    memset(poll, 0, sizeof(*poll));
    sqlite3_stmt *q = NULL;
    const char *sql = id ? "SELECT id,question,closed FROM polls WHERE id=?"
                         : "SELECT id,question,closed FROM polls ORDER BY id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    if (id)
        sqlite3_bind_int64(q, 1, (sqlite3_int64)id);
    int rc = sqlite3_step(q);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(q);
        return rc == SQLITE_DONE ? 0 : -1;
    }
    poll->id = (uint64_t)sqlite3_column_int64(q, 0);
    snprintf(poll->question, sizeof(poll->question), "%s", sqlite3_column_text(q, 1));
    poll->closed = sqlite3_column_int(q, 2) != 0;
    sqlite3_finalize(q);
    q = NULL;
    sql = "SELECT o.number,o.label,COUNT(v.account_id) FROM poll_options o LEFT JOIN poll_votes v "
          "ON v.poll_id=o.poll_id AND v.choice=o.number WHERE o.poll_id=? GROUP BY "
          "o.number,o.label ORDER BY o.number";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)poll->id);
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        int i = poll->option_count;
        if (i >= POLL_OPTIONS_MAX || sqlite3_column_int(q, 0) != i + 1) {
            rc = SQLITE_ERROR;
            break;
        }
        snprintf(poll->options[i], sizeof(poll->options[i]), "%s", sqlite3_column_text(q, 1));
        poll->votes[i] = (uint32_t)sqlite3_column_int64(q, 2);
        poll->option_count++;
    }
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_DONE || poll->option_count < 2)
        return -1;
    if (sqlite3_prepare_v2(db->handle,
                           "SELECT choice FROM poll_votes WHERE poll_id=? AND account_id=?", -1, &q,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)poll->id);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    rc = sqlite3_step(q);
    if (rc == SQLITE_ROW)
        poll->my_vote = (uint32_t)sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 1 : -1;
}

int db_create_poll(cupid_db_t *db, uint64_t actor, const poll_info_t *poll) {
    if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int result = -1;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT 1 FROM polls WHERE closed=0 LIMIT 1", -1, &q,
                           NULL) != SQLITE_OK)
        goto done;
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    q = NULL;
    if (rc == SQLITE_ROW) {
        result = -2;
        goto done;
    }
    if (rc != SQLITE_DONE)
        goto done;
    if (sqlite3_prepare_v2(db->handle, "INSERT INTO polls(question,creator_id) VALUES(?,?)", -1, &q,
                           NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_text(q, 1, poll->question, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)actor);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_DONE)
        goto done;
    sqlite3_int64 id = sqlite3_last_insert_rowid(db->handle);
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO poll_options(poll_id,number,label) VALUES(?,?,?)", -1, &q,
                           NULL) != SQLITE_OK)
        goto done;
    for (int i = 0; i < poll->option_count; i++) {
        sqlite3_bind_int64(q, 1, id);
        sqlite3_bind_int(q, 2, i + 1);
        sqlite3_bind_text(q, 3, poll->options[i], -1, SQLITE_STATIC);
        if (sqlite3_step(q) != SQLITE_DONE)
            goto done;
        sqlite3_reset(q);
        sqlite3_clear_bindings(q);
    }
    result = 0;
done:
    sqlite3_finalize(q);
    if (result == 0 && sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
        return 0;
    sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return result < 0 ? result : -1;
}

int db_vote_poll(cupid_db_t *db, uint64_t id, uint64_t account, uint32_t choice) {
    sqlite3_stmt *q = NULL;
    const char *sql =
        "INSERT INTO poll_votes(poll_id,account_id,choice) SELECT p.id,?,o.number FROM polls p "
        "JOIN poll_options o ON o.poll_id=p.id WHERE p.id=? AND p.closed=0 AND o.number=? ON "
        "CONFLICT(poll_id,account_id) DO UPDATE SET choice=excluded.choice";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)id);
    sqlite3_bind_int(q, 3, (int)choice);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1 ? 0 : -1;
}

int db_close_poll(cupid_db_t *db) {
    return sqlite3_exec(db->handle, "UPDATE polls SET closed=1 WHERE closed=0", NULL, NULL, NULL) ==
                   SQLITE_OK
               ? 0
               : -1;
}
