#include "internal.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

int db_init_bbs(cupid_db_t *db) {
    return sqlite3_exec(
               db->handle,
               "CREATE TABLE IF NOT EXISTS bbs_threads(id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "title TEXT NOT NULL,author_id INTEGER NOT NULL REFERENCES users(id),created_at "
               "INTEGER NOT NULL,"
               "pinned INTEGER NOT NULL DEFAULT 0,locked INTEGER NOT NULL DEFAULT 0);"
               "CREATE TABLE IF NOT EXISTS bbs_posts(id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "thread_id INTEGER NOT NULL REFERENCES bbs_threads(id) ON DELETE CASCADE,"
               "author_id INTEGER NOT NULL REFERENCES users(id),body TEXT NOT NULL,created_at "
               "INTEGER NOT NULL,"
               "deleted INTEGER NOT NULL DEFAULT 0);"
               "CREATE INDEX IF NOT EXISTS bbs_posts_thread ON bbs_posts(thread_id,id);"
               "CREATE INDEX IF NOT EXISTS bbs_posts_author ON bbs_posts(author_id,created_at);"
               "CREATE TABLE IF NOT EXISTS bbs_reads(account_id INTEGER NOT NULL REFERENCES "
               "users(id) ON DELETE CASCADE,"
               "thread_id INTEGER NOT NULL REFERENCES bbs_threads(id) ON DELETE CASCADE,"
               "last_post INTEGER NOT NULL,PRIMARY KEY(account_id,thread_id));",
               NULL, NULL, NULL) == SQLITE_OK
               ? 0
               : -1;
}

static const char THREAD_COLUMNS[] =
    "SELECT t.id,t.title,u.username,t.created_at,t.pinned,t.locked,"
    "(SELECT COUNT(*) FROM bbs_posts p WHERE p.thread_id=t.id),"
    "(SELECT MAX(id) FROM bbs_posts p WHERE p.thread_id=t.id),"
    "COALESCE(r.last_post,0) FROM bbs_threads t JOIN users u ON u.id=t.author_id "
    "LEFT JOIN bbs_reads r ON r.thread_id=t.id AND r.account_id=? ";

static void thread_row(sqlite3_stmt *q, bbs_thread_t *t) {
    memset(t, 0, sizeof(*t));
    t->id = (uint64_t)sqlite3_column_int64(q, 0);
    snprintf(t->title, sizeof(t->title), "%s", sqlite3_column_text(q, 1));
    snprintf(t->author, sizeof(t->author), "%s", sqlite3_column_text(q, 2));
    t->created_at = (uint64_t)sqlite3_column_int64(q, 3);
    t->pinned = sqlite3_column_int(q, 4) != 0;
    t->locked = sqlite3_column_int(q, 5) != 0;
    t->post_count = (uint32_t)sqlite3_column_int64(q, 6);
    t->last_post = (uint64_t)sqlite3_column_int64(q, 7);
    t->unread = t->last_post > (uint64_t)sqlite3_column_int64(q, 8);
}

int db_bbs_status(cupid_db_t *db, uint64_t account, uint32_t *unread) {
    sqlite3_stmt *q = NULL;
    const char *sql = "SELECT COUNT(*) FROM bbs_threads t LEFT JOIN bbs_reads r ON "
                      "r.thread_id=t.id AND r.account_id=? WHERE COALESCE(r.last_post,0)<(SELECT "
                      "MAX(id) FROM bbs_posts p WHERE p.thread_id=t.id)";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW)
        *unread = (uint32_t)sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    return rc == SQLITE_ROW ? 0 : -1;
}

int db_bbs_list(cupid_db_t *db, uint64_t account, uint32_t offset, bbs_thread_t *rows, int *count,
                bool *more) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "%s ORDER BY t.pinned DESC,8 DESC,t.id DESC LIMIT 21 OFFSET ?",
             THREAD_COLUMNS);
    sqlite3_stmt *q = NULL;
    *count = 0;
    *more = false;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    sqlite3_bind_int64(q, 2, offset);
    int rc;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        if (*count == BBS_PAGE_SIZE) {
            *more = true;
            break;
        }
        thread_row(q, &rows[(*count)++]);
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int db_bbs_view(cupid_db_t *db, uint64_t account, uint64_t id, uint64_t before, uint64_t after,
                bbs_view_t *view) {
    memset(view, 0, sizeof(*view));
    char sql[1024];
    snprintf(sql, sizeof(sql), "%s WHERE t.id=?", THREAD_COLUMNS);
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)id);
    int rc = sqlite3_step(q);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(q);
        return rc == SQLITE_DONE ? 0 : -1;
    }
    thread_row(q, &view->thread);
    sqlite3_finalize(q);
    q = NULL;
    const char *condition = before ? " AND p.id<?" : after ? " AND p.id>?" : "";
    snprintf(sql, sizeof(sql),
             "SELECT p.id,u.username,p.body,p.created_at,p.deleted,p.author_id FROM bbs_posts p "
             "JOIN users u ON u.id=p.author_id WHERE p.thread_id=?%s ORDER BY p.id %s LIMIT 20",
             condition, after ? "ASC" : "DESC");
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)id);
    if (before || after)
        sqlite3_bind_int64(q, 2, (sqlite3_int64)(before ? before : after));
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        bbs_post_t *p = &view->posts[view->count++];
        p->id = (uint64_t)sqlite3_column_int64(q, 0);
        snprintf(p->author, sizeof(p->author), "%s", sqlite3_column_text(q, 1));
        snprintf(p->body, sizeof(p->body), "%s", sqlite3_column_text(q, 2));
        p->created_at = (uint64_t)sqlite3_column_int64(q, 3);
        p->deleted = sqlite3_column_int(q, 4) != 0;
        p->is_self = account == (uint64_t)sqlite3_column_int64(q, 5);
    }
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_DONE || !view->count)
        return -1;
    if (!after)
        for (int i = 0; i < view->count / 2; i++) {
            bbs_post_t tmp = view->posts[i];
            view->posts[i] = view->posts[view->count - 1 - i];
            view->posts[view->count - 1 - i] = tmp;
        }
    if (sqlite3_prepare_v2(db->handle, "SELECT MIN(id),MAX(id) FROM bbs_posts WHERE thread_id=?",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)id);
    rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        view->older = (uint64_t)sqlite3_column_int64(q, 0) < view->posts[0].id;
        view->newer = (uint64_t)sqlite3_column_int64(q, 1) > view->posts[view->count - 1].id;
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW ? 1 : -1;
}

int db_bbs_read(cupid_db_t *db, uint64_t account, uint64_t thread, uint64_t last) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO bbs_reads(account_id,thread_id,last_post) VALUES(?,?,?) ON "
                           "CONFLICT(account_id,thread_id) DO UPDATE SET "
                           "last_post=MAX(last_post,excluded.last_post)",
                           -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)thread);
    sqlite3_bind_int64(q, 3, (sqlite3_int64)last);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_bbs_post(cupid_db_t *db, uint64_t account, uint64_t *thread, const char *title,
                const char *body, uint64_t *post) {
    if (sqlite3_exec(db->handle, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int result = -1;
    sqlite3_stmt *q = NULL;
    sqlite3_int64 now = (sqlite3_int64)time(NULL) * 1000;
    if (sqlite3_prepare_v2(db->handle, "SELECT MAX(created_at) FROM bbs_posts WHERE author_id=?",
                           -1, &q, NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)account);
    int rc = sqlite3_step(q);
    bool recent = rc == SQLITE_ROW && sqlite3_column_type(q, 0) != SQLITE_NULL &&
                  now - sqlite3_column_int64(q, 0) < 5000;
    sqlite3_finalize(q);
    q = NULL;
    if (recent) {
        result = -4;
        goto done;
    }
    if (rc != SQLITE_ROW)
        goto done;
    if (*thread) {
        if (sqlite3_prepare_v2(db->handle, "SELECT locked FROM bbs_threads WHERE id=?", -1, &q,
                               NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(q, 1, (sqlite3_int64)*thread);
        rc = sqlite3_step(q);
        bool locked = rc == SQLITE_ROW && sqlite3_column_int(q, 0) != 0;
        sqlite3_finalize(q);
        q = NULL;
        if (rc != SQLITE_ROW || locked) {
            result = -2;
            goto done;
        }
    } else {
        if (sqlite3_prepare_v2(db->handle,
                               "INSERT INTO bbs_threads(title,author_id,created_at) VALUES(?,?,?)",
                               -1, &q, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_text(q, 1, title, -1, SQLITE_STATIC);
        sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
        sqlite3_bind_int64(q, 3, now);
        rc = sqlite3_step(q);
        sqlite3_finalize(q);
        q = NULL;
        if (rc != SQLITE_DONE)
            goto done;
        *thread = (uint64_t)sqlite3_last_insert_rowid(db->handle);
    }
    if (sqlite3_prepare_v2(
            db->handle,
            "INSERT INTO bbs_posts(thread_id,author_id,body,created_at) VALUES(?,?,?,?)", -1, &q,
            NULL) != SQLITE_OK)
        goto done;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)*thread);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    sqlite3_bind_text(q, 3, body, -1, SQLITE_STATIC);
    sqlite3_bind_int64(q, 4, now);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_DONE)
        goto done;
    *post = (uint64_t)sqlite3_last_insert_rowid(db->handle);
    if (db_bbs_read(db, account, *thread, *post) == 0)
        result = 0;
done:
    sqlite3_finalize(q);
    if (result == 0 && sqlite3_exec(db->handle, "COMMIT", NULL, NULL, NULL) == SQLITE_OK)
        return 0;
    sqlite3_exec(db->handle, "ROLLBACK", NULL, NULL, NULL);
    return result < 0 ? result : -1;
}

int db_bbs_delete(cupid_db_t *db, uint64_t account, bool admin, uint64_t post, uint64_t *thread) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "SELECT thread_id FROM bbs_posts WHERE id=? AND (author_id=? OR ?)", -1,
                           &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)post);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    sqlite3_bind_int(q, 3, admin);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW)
        *thread = (uint64_t)sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    q = NULL;
    if (rc != SQLITE_ROW)
        return -2;
    if (sqlite3_prepare_v2(
            db->handle, "UPDATE bbs_posts SET body='',deleted=1 WHERE id=? AND (author_id=? OR ?)",
            -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)post);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)account);
    sqlite3_bind_int(q, 3, admin);
    rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1 ? 0 : -1;
}

int db_bbs_flags(cupid_db_t *db, uint64_t thread, const char *setting, bool value) {
    sqlite3_stmt *q = NULL;
    const char *sql = !strcmp(setting, "pin") ? "UPDATE bbs_threads SET pinned=? WHERE id=?"
                                              : "UPDATE bbs_threads SET locked=? WHERE id=?";
    if (sqlite3_prepare_v2(db->handle, sql, -1, &q, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(q, 1, value);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)thread);
    int rc = sqlite3_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE && sqlite3_changes(db->handle) == 1 ? 0 : -1;
}
