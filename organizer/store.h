#ifndef STORE_H
#define STORE_H

/*
 * store.h - organizer data: todos (undated ideas / notes) and calendar
 * entries (one-off on a date, or recurring).  A todo can be scheduled,
 * which creates a one-off entry linked back to it through todo_id.
 *
 * Strings in the structs below are malloc'd; free them with the
 * matching *_free function.
 */

#include <sqlite3.h>

#include "date.h"

#define REC_NONE    0
#define REC_DAILY   1
#define REC_WEEKLY  2   /* on `weekday` (1 = Monday .. 7 = Sunday)      */
#define REC_MONTHLY 3   /* on `day`; skipped in months without that day */
#define REC_YEARLY  4   /* on `day`.`month`; 29.02 only in leap years    */

#define TODO_ALL    0
#define TODO_OPEN   1
#define TODO_DONE   2

typedef struct {
    sqlite3_int64 id;
    char *title;
    char *description;      /* "" when none */
    int   priority;         /* 1 = high, 2 = normal, 3 = low */
    int   done;
} Todo;

typedef struct {
    sqlite3_int64 id;
    char *title;
    int   priority;
    int   done;
    char  next_date[11];    /* earliest scheduled date from today on, or "" */
} TodoRow;

typedef struct {
    sqlite3_int64 id;
    char *title;
    char *description;      /* "" when none */
    int   recurrence;       /* REC_* */
    Day   date;             /* REC_NONE only */
    int   weekday;          /* REC_WEEKLY */
    int   day, month;       /* REC_MONTHLY (day) / REC_YEARLY (day, month) */
    char  time[6];          /* "HH:MM", "" = all day */
    int   duration_min;     /* 0 = none */
    sqlite3_int64 todo_id;  /* -1 = not from a todo */
} Entry;

/* One entry on one date - recurring entries expanded */
typedef struct {
    sqlite3_int64 entry_id;
    Day   date;
    char *title;
    char  time[6];
    int   duration_min;
    int   recurring;
    sqlite3_int64 todo_id;
} Occurrence;

/* Create or upgrade the schema.  Returns 0, -1 on error (see db_migrate) */
int  store_init(sqlite3 *db);

/* Copy todos and calendar entries from a database with the poc.db
 * schema (no times, no todo links).  Returns 0, -1 on error. */
int  store_import(sqlite3 *db, const char *path);
/* 1 when there are no todos and no calendar entries */
int  store_is_empty(sqlite3 *db);

/* Todos */
int           store_todos(sqlite3 *db, int filter, TodoRow **out);  /* count or -1 */
void          store_todos_free(TodoRow *rows, int n);
int           store_get_todo(sqlite3 *db, sqlite3_int64 id, Todo *out);
void          store_todo_free(Todo *t);
sqlite3_int64 store_add_todo(sqlite3 *db, const char *title,
                             const char *description, int priority);
int           store_update_todo(sqlite3 *db, const Todo *t);
int           store_set_todo_done(sqlite3 *db, sqlite3_int64 id, int done);
int           store_delete_todo(sqlite3 *db, sqlite3_int64 id);

/* Put a todo on the calendar: a one-off entry with the todo's title and
 * description.  time may be NULL or "" for all day.  Returns the entry id. */
sqlite3_int64 store_schedule_todo(sqlite3 *db, sqlite3_int64 todo_id,
                                  Day date, const char *time);

/* Calendar entries */
int           store_get_entry(sqlite3 *db, sqlite3_int64 id, Entry *out);
void          store_entry_free(Entry *e);
sqlite3_int64 store_add_entry(sqlite3 *db, const Entry *e);
int           store_update_entry(sqlite3 *db, const Entry *e);
int           store_delete_entry(sqlite3 *db, sqlite3_int64 id);

/* All occurrences in [from, to], sorted by date, then all-day entries
 * first, then time, then title.  Returns the count or -1. */
int           store_occurrences(sqlite3 *db, Day from, Day to, Occurrence **out);
void          store_occurrences_free(Occurrence *occ, int n);

#endif /* STORE_H */
