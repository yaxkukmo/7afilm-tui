#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

sqlite3 *
db_open(const char *progname, const char *file)
{
    const char *home = getenv("HOME");
    char app_dir[1024], db_path[1040];
    sqlite3 *db;

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/%s", app_dir, file);

    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "%s: cannot open %s\n", progname, db_path);
        exit(1);
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",  NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;",   NULL, NULL, NULL);
    return db;
}

int
db_migrate(sqlite3 *db, const char *const *steps, int nsteps)
{
    int version = db_user_version(db);
    char sql[64];

    if (version < 0)
        return -1;
    for (; version < nsteps; version++) {
        snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", version + 1);
        if (sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK)
            return -1;
        if (sqlite3_exec(db, steps[version], NULL, NULL, NULL) != SQLITE_OK ||
            sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            /* keep the step's error message, not ROLLBACK's */
            char err[256];
            snprintf(err, sizeof(err), "%s", sqlite3_errmsg(db));
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            fprintf(stderr, "schema migration %d failed: %s\n", version + 1, err);
            return -1;
        }
        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
            return -1;
    }
    return 0;
}

int
db_user_version(sqlite3 *db)
{
    sqlite3_stmt *stmt;
    int version = 0;

    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return version;
}

int
db_table_exists(sqlite3 *db, const char *name)
{
    sqlite3_stmt *stmt;
    int exists = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    return exists;
}

int
db_column_exists(sqlite3 *db, const char *table, const char *col)
{
    sqlite3_stmt *stmt;
    char sql[256];
    int exists = 0;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cn = (const char *)sqlite3_column_text(stmt, 1);
            if (cn && strcmp(cn, col) == 0) { exists = 1; break; }
        }
        sqlite3_finalize(stmt);
    }
    return exists;
}

const char *
db_col_str(sqlite3_stmt *s, int c)
{
    const unsigned char *v = sqlite3_column_text(s, c);
    return v ? (const char *)v : "";
}

void
db_bind_id(sqlite3_stmt *s, int param, sqlite3_int64 id)
{
    if (id < 0) sqlite3_bind_null(s, param);
    else        sqlite3_bind_int64(s, param, id);
}
