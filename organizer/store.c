#include "store.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Schema                                                              */
/* ------------------------------------------------------------------ */

/* Append-only: a released step is never edited, changes go into a
 * new one.  The tables keep the poc.db layout so its data imports
 * as is. */
static const char *const migrations[] = {
    /* 1: initial schema */
    "CREATE TABLE todos ("
    " id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    " title       TEXT NOT NULL,"
    " description TEXT,"
    " priority    INTEGER NOT NULL DEFAULT 2 CHECK(priority IN (1,2,3)),"
    " status      TEXT NOT NULL DEFAULT 'open' CHECK(status IN ('open','done')),"
    " created_at  TEXT DEFAULT (datetime('now')),"
    " updated_at  TEXT DEFAULT (datetime('now'))"
    ");"
    "CREATE TABLE calendar_entries ("
    " id                 INTEGER PRIMARY KEY AUTOINCREMENT,"
    " title              TEXT NOT NULL,"
    " description        TEXT,"
    " entry_date         TEXT,"
    " entry_time         TEXT CHECK(entry_time GLOB '[0-2][0-9]:[0-5][0-9]'),"
    " duration_min       INTEGER CHECK(duration_min > 0),"
    " recurrence_type    TEXT"
    "     CHECK(recurrence_type IN ('daily','weekly','monthly','yearly')),"
    " recurrence_weekday INTEGER,"
    " recurrence_day     INTEGER,"
    " recurrence_month   INTEGER,"
    " todo_id            INTEGER REFERENCES todos(id) ON DELETE SET NULL,"
    " created_at         TEXT DEFAULT (datetime('now')),"
    " CHECK ((entry_date IS NOT NULL AND recurrence_type IS NULL)"
    "     OR (entry_date IS NULL AND recurrence_type IS NOT NULL)),"
    " CHECK (recurrence_type IS NOT 'weekly'"
    "     OR COALESCE(recurrence_weekday, 0) BETWEEN 1 AND 7),"
    " CHECK (recurrence_type NOT IN ('monthly','yearly')"
    "     OR COALESCE(recurrence_day, 0) BETWEEN 1 AND 31),"
    " CHECK (recurrence_type IS NOT 'yearly'"
    "     OR COALESCE(recurrence_month, 0) BETWEEN 1 AND 12)"
    ");"
    "CREATE INDEX calendar_entries_date ON calendar_entries(entry_date);"
    "CREATE INDEX calendar_entries_todo ON calendar_entries(todo_id);",
};

int
store_init(sqlite3 *db)
{
    return db_migrate(db, migrations,
                      (int)(sizeof(migrations) / sizeof(migrations[0])));
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *const rec_names[] = {
    NULL, "daily", "weekly", "monthly", "yearly"
};

static int
rec_from_name(const char *s)
{
    int i;
    for (i = 1; i <= REC_YEARLY; i++)
        if (strcmp(s, rec_names[i]) == 0) return i;
    return REC_NONE;
}

static char *
dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Grow *arr (of elemsz-byte elements) so it holds at least n + 1 */
static int
grow(void **arr, int *cap, int n, size_t elemsz)
{
    void *tmp;
    int   nc;
    if (n < *cap) return 0;
    nc  = *cap ? *cap * 2 : 16;
    tmp = realloc(*arr, (size_t)nc * elemsz);
    if (!tmp) return -1;
    *arr = tmp;
    *cap = nc;
    return 0;
}

static void
bind_text_or_null(sqlite3_stmt *s, int param, const char *v)
{
    if (v && v[0]) sqlite3_bind_text(s, param, v, -1, SQLITE_TRANSIENT);
    else           sqlite3_bind_null(s, param);
}

static void
bind_int_or_null(sqlite3_stmt *s, int param, int v)
{
    if (v > 0) sqlite3_bind_int(s, param, v);
    else       sqlite3_bind_null(s, param);
}

/* Run a statement that returns no rows; returns 0 or -1 */
static int
exec_done(sqlite3_stmt *s)
{
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Import                                                              */
/* ------------------------------------------------------------------ */

int
store_import(sqlite3 *db, const char *path)
{
    sqlite3_stmt *s;
    FILE *f;
    int   rc;

    /* ATTACH would silently create a missing file */
    if (!(f = fopen(path, "rb"))) {
        fprintf(stderr, "import: cannot open %s\n", path);
        return -1;
    }
    fclose(f);

    if (sqlite3_prepare_v2(db, "ATTACH DATABASE ?1 AS src;", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    if (exec_done(s) != 0) {
        fprintf(stderr, "import: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    rc = sqlite3_exec(db,
        "BEGIN;"
        "INSERT INTO todos"
        " (title, description, priority, status, created_at, updated_at)"
        " SELECT title, description, COALESCE(priority, 2),"
        "        COALESCE(status, 'open'), created_at, updated_at"
        " FROM src.todos ORDER BY id;"
        "INSERT INTO calendar_entries"
        " (title, description, entry_date, recurrence_type,"
        "  recurrence_weekday, recurrence_day, recurrence_month, created_at)"
        " SELECT title, description, entry_date, recurrence_type,"
        "        recurrence_weekday, recurrence_day, recurrence_month, created_at"
        " FROM src.calendar_entries ORDER BY id;"
        "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "import: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_exec(db, "DETACH DATABASE src;", NULL, NULL, NULL);
    return rc == SQLITE_OK ? 0 : -1;
}

int
store_is_empty(sqlite3 *db)
{
    sqlite3_stmt *s;
    int empty = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT NOT EXISTS (SELECT 1 FROM todos)"
            "   AND NOT EXISTS (SELECT 1 FROM calendar_entries);",
            -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        empty = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return empty;
}

/* ------------------------------------------------------------------ */
/* Todos                                                               */
/* ------------------------------------------------------------------ */

int
store_todos(sqlite3 *db, int filter, TodoRow **out)
{
    sqlite3_stmt *s;
    TodoRow *rows = NULL;
    int n = 0, cap = 0;
    char today[11];

    *out = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT t.id, t.title, t.priority, t.status = 'done',"
            "       COALESCE((SELECT MIN(e.entry_date) FROM calendar_entries e"
            "                  WHERE e.todo_id = t.id AND e.entry_date >= ?1), '')"
            "  FROM todos t"
            " WHERE ?2 = 0 OR (?2 = 1 AND t.status = 'open')"
            "              OR (?2 = 2 AND t.status = 'done')"
            " ORDER BY t.status = 'done', t.priority, t.id;",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    day_format(day_today(), today);
    sqlite3_bind_text(s, 1, today, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, filter);

    while (sqlite3_step(s) == SQLITE_ROW) {
        TodoRow *r;
        if (grow((void **)&rows, &cap, n, sizeof(*rows)) != 0) goto fail;
        r = &rows[n];
        r->id       = sqlite3_column_int64(s, 0);
        r->title    = dupstr(db_col_str(s, 1));
        r->priority = sqlite3_column_int(s, 2);
        r->done     = sqlite3_column_int(s, 3);
        snprintf(r->next_date, sizeof(r->next_date), "%s", db_col_str(s, 4));
        if (!r->title) goto fail;
        n++;
    }
    sqlite3_finalize(s);
    *out = rows;
    return n;

fail:
    sqlite3_finalize(s);
    store_todos_free(rows, n);
    return -1;
}

void
store_todos_free(TodoRow *rows, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(rows[i].title);
    free(rows);
}

int
store_get_todo(sqlite3 *db, sqlite3_int64 id, Todo *out)
{
    sqlite3_stmt *s;
    int rc = -1;

    memset(out, 0, sizeof(*out));
    if (sqlite3_prepare_v2(db,
            "SELECT title, COALESCE(description, ''), priority, status = 'done'"
            "  FROM todos WHERE id = ?1;", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        out->id          = id;
        out->title       = dupstr(db_col_str(s, 0));
        out->description = dupstr(db_col_str(s, 1));
        out->priority    = sqlite3_column_int(s, 2);
        out->done        = sqlite3_column_int(s, 3);
        rc = (out->title && out->description) ? 0 : -1;
        if (rc != 0) store_todo_free(out);
    }
    sqlite3_finalize(s);
    return rc;
}

void
store_todo_free(Todo *t)
{
    free(t->title);
    free(t->description);
    t->title = t->description = NULL;
}

sqlite3_int64
store_add_todo(sqlite3 *db, const char *title, const char *description,
               int priority)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO todos (title, description, priority)"
            " VALUES (?1, ?2, ?3);", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, title, -1, SQLITE_TRANSIENT);
    bind_text_or_null(s, 2, description);
    sqlite3_bind_int(s, 3, priority);
    if (exec_done(s) != 0) return -1;
    return sqlite3_last_insert_rowid(db);
}

int
store_update_todo(sqlite3 *db, const Todo *t)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE todos SET title = ?1, description = ?2, priority = ?3,"
            "       status = ?4, updated_at = datetime('now')"
            " WHERE id = ?5;", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, t->title, -1, SQLITE_TRANSIENT);
    bind_text_or_null(s, 2, t->description);
    sqlite3_bind_int(s, 3, t->priority);
    sqlite3_bind_text(s, 4, t->done ? "done" : "open", -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 5, t->id);
    return exec_done(s);
}

int
store_set_todo_done(sqlite3 *db, sqlite3_int64 id, int done)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE todos SET status = ?1, updated_at = datetime('now')"
            " WHERE id = ?2;", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, done ? "done" : "open", -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, id);
    return exec_done(s);
}

int
store_delete_todo(sqlite3 *db, sqlite3_int64 id)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "DELETE FROM todos WHERE id = ?1;",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, id);
    return exec_done(s);
}

sqlite3_int64
store_schedule_todo(sqlite3 *db, sqlite3_int64 todo_id, Day date,
                    const char *time)
{
    sqlite3_stmt *s;
    char d[11];

    if (sqlite3_prepare_v2(db,
            "INSERT INTO calendar_entries"
            " (title, description, entry_date, entry_time, todo_id)"
            " SELECT title, description, ?2, ?3, id FROM todos WHERE id = ?1;",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    day_format(date, d);
    sqlite3_bind_int64(s, 1, todo_id);
    sqlite3_bind_text(s, 2, d, -1, SQLITE_TRANSIENT);
    bind_text_or_null(s, 3, time);
    if (exec_done(s) != 0 || sqlite3_changes(db) == 0) return -1;
    return sqlite3_last_insert_rowid(db);
}

/* ------------------------------------------------------------------ */
/* Calendar entries                                                    */
/* ------------------------------------------------------------------ */

int
store_get_entry(sqlite3 *db, sqlite3_int64 id, Entry *out)
{
    sqlite3_stmt *s;
    int rc = -1;

    memset(out, 0, sizeof(*out));
    if (sqlite3_prepare_v2(db,
            "SELECT title, COALESCE(description, ''), COALESCE(entry_date, ''),"
            "       COALESCE(entry_time, ''), COALESCE(duration_min, 0),"
            "       COALESCE(recurrence_type, ''), COALESCE(recurrence_weekday, 0),"
            "       COALESCE(recurrence_day, 0), COALESCE(recurrence_month, 0),"
            "       COALESCE(todo_id, -1)"
            "  FROM calendar_entries WHERE id = ?1;", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        out->id          = id;
        out->title       = dupstr(db_col_str(s, 0));
        out->description = dupstr(db_col_str(s, 1));
        day_parse(db_col_str(s, 2), &out->date);
        snprintf(out->time, sizeof(out->time), "%s", db_col_str(s, 3));
        out->duration_min = sqlite3_column_int(s, 4);
        out->recurrence   = rec_from_name(db_col_str(s, 5));
        out->weekday      = sqlite3_column_int(s, 6);
        out->day          = sqlite3_column_int(s, 7);
        out->month        = sqlite3_column_int(s, 8);
        out->todo_id      = sqlite3_column_int64(s, 9);
        rc = (out->title && out->description) ? 0 : -1;
        if (rc != 0) store_entry_free(out);
    }
    sqlite3_finalize(s);
    return rc;
}

void
store_entry_free(Entry *e)
{
    free(e->title);
    free(e->description);
    e->title = e->description = NULL;
}

/* Parameters 1-10 in the column order of the INSERT / UPDATE below */
static void
bind_entry(sqlite3_stmt *s, const Entry *e)
{
    char d[11];
    int  r = e->recurrence;

    sqlite3_bind_text(s, 1, e->title, -1, SQLITE_TRANSIENT);
    bind_text_or_null(s, 2, e->description);
    if (r == REC_NONE) {
        day_format(e->date, d);
        sqlite3_bind_text(s, 3, d, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(s, 3);
    }
    bind_text_or_null(s, 4, e->time);
    bind_int_or_null(s, 5, e->duration_min);
    if (r >= REC_DAILY && r <= REC_YEARLY)
        sqlite3_bind_text(s, 6, rec_names[r], -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 6);
    bind_int_or_null(s, 7, r == REC_WEEKLY ? e->weekday : 0);
    bind_int_or_null(s, 8, r == REC_MONTHLY || r == REC_YEARLY ? e->day : 0);
    bind_int_or_null(s, 9, r == REC_YEARLY ? e->month : 0);
    db_bind_id(s, 10, e->todo_id);
}

sqlite3_int64
store_add_entry(sqlite3 *db, const Entry *e)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO calendar_entries"
            " (title, description, entry_date, entry_time, duration_min,"
            "  recurrence_type, recurrence_weekday, recurrence_day,"
            "  recurrence_month, todo_id)"
            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    bind_entry(s, e);
    if (exec_done(s) != 0) return -1;
    return sqlite3_last_insert_rowid(db);
}

int
store_update_entry(sqlite3 *db, const Entry *e)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE calendar_entries SET"
            " title = ?1, description = ?2, entry_date = ?3, entry_time = ?4,"
            " duration_min = ?5, recurrence_type = ?6, recurrence_weekday = ?7,"
            " recurrence_day = ?8, recurrence_month = ?9, todo_id = ?10"
            " WHERE id = ?11;", -1, &s, NULL) != SQLITE_OK)
        return -1;
    bind_entry(s, e);
    sqlite3_bind_int64(s, 11, e->id);
    return exec_done(s);
}

int
store_delete_entry(sqlite3 *db, sqlite3_int64 id)
{
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "DELETE FROM calendar_entries WHERE id = ?1;",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, id);
    return exec_done(s);
}

/* ------------------------------------------------------------------ */
/* Search list                                                         */
/* ------------------------------------------------------------------ */

int
store_items(sqlite3 *db, Item **out)
{
    sqlite3_stmt *s;
    Item *items = NULL;
    int   n = 0, cap = 0;
    char  today[11];

    *out = NULL;
    /* bucket: 0 upcoming, 1 recurring, 2 open todo, 3 done todo, 4 past */
    if (sqlite3_prepare_v2(db,
            "SELECT is_todo, id, title, description, done, rec, d FROM ("
            " SELECT 0 AS is_todo, id, title,"
            "        COALESCE(description, '') AS description, 0 AS done,"
            "        COALESCE(recurrence_type, '') AS rec,"
            "        COALESCE(entry_date, '') AS d,"
            "        CASE WHEN recurrence_type IS NOT NULL THEN 1"
            "             WHEN entry_date >= ?1 THEN 0 ELSE 4 END AS bucket"
            "   FROM calendar_entries"
            " UNION ALL"
            " SELECT 1, id, title, COALESCE(description, ''),"
            "        status = 'done', '', '',"
            "        CASE WHEN status = 'done' THEN 3 ELSE 2 END"
            "   FROM todos)"
            " ORDER BY bucket,"
            "          CASE WHEN bucket = 0 THEN d END ASC,"
            "          CASE WHEN bucket = 4 THEN d END DESC,"
            "          title;",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    day_format(day_today(), today);
    sqlite3_bind_text(s, 1, today, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(s) == SQLITE_ROW) {
        Item *it;
        if (grow((void **)&items, &cap, n, sizeof(*items)) != 0) goto fail;
        it = &items[n];
        memset(it, 0, sizeof(*it));
        it->is_todo    = sqlite3_column_int(s, 0);
        it->id         = sqlite3_column_int64(s, 1);
        it->title       = dupstr(db_col_str(s, 2));
        it->description = dupstr(db_col_str(s, 3));
        it->done        = sqlite3_column_int(s, 4);
        it->recurrence  = rec_from_name(db_col_str(s, 5));
        day_parse(db_col_str(s, 6), &it->date);
        if (!it->title || !it->description) goto fail;
        n++;
    }
    sqlite3_finalize(s);
    *out = items;
    return n;

fail:
    sqlite3_finalize(s);
    store_items_free(items, n);
    return -1;
}

void
store_items_free(Item *items, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(items[i].title);
        free(items[i].description);
    }
    free(items);
}

/* ------------------------------------------------------------------ */
/* Occurrences                                                         */
/* ------------------------------------------------------------------ */

static int
recurs_on(int rec, int weekday, int day, int month, Day date)
{
    int y, m, d;

    switch (rec) {
    case REC_DAILY:
        return 1;
    case REC_WEEKLY:
        return day_weekday(date) == weekday;
    case REC_MONTHLY:
        day_to_ymd(date, &y, &m, &d);
        return d == day;
    case REC_YEARLY:
        day_to_ymd(date, &y, &m, &d);
        return d == day && m == month;
    }
    return 0;
}

static int
occ_cmp(const void *a, const void *b)
{
    const Occurrence *x = a, *y = b;
    int c;

    if (x->date != y->date) return x->date < y->date ? -1 : 1;
    if ((c = strcmp(x->time, y->time)) != 0) return c;  /* "" (all day) first */
    if ((c = strcmp(x->title, y->title)) != 0) return c;
    return (x->entry_id > y->entry_id) - (x->entry_id < y->entry_id);
}

static int
occ_add(Occurrence **occ, int *n, int *cap, sqlite3_int64 id, Day date,
        const char *title, const char *time, int duration, int recurring,
        sqlite3_int64 todo_id)
{
    Occurrence *o;
    if (grow((void **)occ, cap, *n, sizeof(**occ)) != 0) return -1;
    o = &(*occ)[*n];
    o->entry_id     = id;
    o->date         = date;
    o->title        = dupstr(title);
    snprintf(o->time, sizeof(o->time), "%s", time);
    o->duration_min = duration;
    o->recurring    = recurring;
    o->todo_id      = todo_id;
    if (!o->title) return -1;
    (*n)++;
    return 0;
}

int
store_occurrences(sqlite3 *db, Day from, Day to, Occurrence **out)
{
    sqlite3_stmt *s;
    Occurrence *occ = NULL;
    int n = 0, cap = 0;
    char f[11], t[11];

    *out = NULL;
    if (to < from) return 0;
    day_format(from, f);
    day_format(to, t);

    /* One-off entries */
    if (sqlite3_prepare_v2(db,
            "SELECT id, title, entry_date, COALESCE(entry_time, ''),"
            "       COALESCE(duration_min, 0), COALESCE(todo_id, -1)"
            "  FROM calendar_entries"
            " WHERE recurrence_type IS NULL AND entry_date BETWEEN ?1 AND ?2;",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, f, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, t, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW) {
        Day date;
        if (!day_parse(db_col_str(s, 2), &date)) continue;
        if (occ_add(&occ, &n, &cap, sqlite3_column_int64(s, 0), date,
                    db_col_str(s, 1), db_col_str(s, 3),
                    sqlite3_column_int(s, 4), 0,
                    sqlite3_column_int64(s, 5)) != 0)
            goto fail;
    }
    sqlite3_finalize(s);

    /* Recurring entries, expanded day by day */
    if (sqlite3_prepare_v2(db,
            "SELECT id, title, recurrence_type, COALESCE(recurrence_weekday, 0),"
            "       COALESCE(recurrence_day, 0), COALESCE(recurrence_month, 0),"
            "       COALESCE(entry_time, ''), COALESCE(duration_min, 0),"
            "       COALESCE(todo_id, -1)"
            "  FROM calendar_entries WHERE recurrence_type IS NOT NULL;",
            -1, &s, NULL) != SQLITE_OK)
        goto fail_nostmt;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int rec     = rec_from_name(db_col_str(s, 2));
        int weekday = sqlite3_column_int(s, 3);
        int day     = sqlite3_column_int(s, 4);
        int month   = sqlite3_column_int(s, 5);
        Day date;
        for (date = from; date <= to; date++) {
            if (!recurs_on(rec, weekday, day, month, date)) continue;
            if (occ_add(&occ, &n, &cap, sqlite3_column_int64(s, 0), date,
                        db_col_str(s, 1), db_col_str(s, 6),
                        sqlite3_column_int(s, 7), 1,
                        sqlite3_column_int64(s, 8)) != 0)
                goto fail;
        }
    }
    sqlite3_finalize(s);

    if (n > 1) qsort(occ, (size_t)n, sizeof(*occ), occ_cmp);
    *out = occ;
    return n;

fail:
    sqlite3_finalize(s);
fail_nostmt:
    store_occurrences_free(occ, n);
    return -1;
}

void
store_occurrences_free(Occurrence *occ, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(occ[i].title);
    free(occ);
}
