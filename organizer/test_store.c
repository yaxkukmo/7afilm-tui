/*
 * test_store.c - checks for date.c and store.c on an in-memory database.
 * Run with `make check` (imports organizer/testdata/poc.db).
 */

#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "date.h"
#include "quickadd.h"
#include "store.h"

static int g_fail = 0;

#define CHECK(cond) do {                                             \
        if (!(cond)) {                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",             \
                    __FILE__, __LINE__, #cond);                      \
            g_fail++;                                                \
        }                                                            \
    } while (0)

static Day
D(const char *s)
{
    Day d = 0;
    if (!day_parse(s, &d))
        fprintf(stderr, "bad test date %s\n", s);
    return d;
}

static int
count_rows(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

/* Occurrences in [from, to] rendered as "MM-DD HH:MM title; ..." */
static void
agenda(sqlite3 *db, const char *from, const char *to, char *out, size_t outsz)
{
    Occurrence *occ;
    int n, i;
    size_t len = 0;

    out[0] = '\0';
    n = store_occurrences(db, D(from), D(to), &occ);
    for (i = 0; i < n && len < outsz; i++) {
        char d[11];
        day_format(occ[i].date, d);
        len += (size_t)snprintf(out + len, outsz - len, "%s%s %s%s%s",
                                i ? "; " : "", d + 5, occ[i].time,
                                occ[i].time[0] ? " " : "", occ[i].title);
    }
    store_occurrences_free(occ, n);
}

static void
test_dates(void)
{
    Day d;
    int y, m, dd;

    CHECK(day_from_ymd(1970, 1, 1) == 0);
    CHECK(day_weekday(D("2026-09-28")) == 1);      /* Monday */
    CHECK(day_weekday(D("2026-10-04")) == 7);      /* Sunday */
    CHECK(day_weekday(D("1969-12-31")) == 3);      /* Wednesday */
    CHECK(days_in_month(2028, 2) == 29);
    CHECK(days_in_month(2100, 2) == 28);
    CHECK(days_in_month(2000, 2) == 29);
    CHECK(!day_parse("2027-02-29", &d));
    CHECK(!day_parse("2026-9-28", &d));
    CHECK(!day_parse("2026-09-28x", &d));

    /* Round trip over a wide range */
    for (d = D("1900-01-01"); d <= D("2200-12-31"); d++) {
        char s[11];
        Day back;
        day_to_ymd(d, &y, &m, &dd);
        day_format(d, s);
        if (!day_parse(s, &back) || back != d) {
            CHECK(!"date round trip");
            break;
        }
    }
}

static void
test_schema(sqlite3 *db)
{
    CHECK(store_init(db) == 0);
    CHECK(store_init(db) == 0);                    /* idempotent */
    CHECK(count_rows(db, "PRAGMA user_version;") == 1);

    /* Both a date and a recurrence, or neither */
    CHECK(sqlite3_exec(db,
        "INSERT INTO calendar_entries(title, entry_date, recurrence_type)"
        " VALUES('x', '2026-01-01', 'daily');", NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "INSERT INTO calendar_entries(title) VALUES('x');",
        NULL, NULL, NULL) != SQLITE_OK);
    /* Recurrence fields must be present */
    CHECK(sqlite3_exec(db,
        "INSERT INTO calendar_entries(title, recurrence_type)"
        " VALUES('x', 'weekly');", NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "INSERT INTO calendar_entries(title, recurrence_type, recurrence_day)"
        " VALUES('x', 'yearly', 3);", NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "INSERT INTO calendar_entries(title, entry_date, entry_time)"
        " VALUES('x', '2026-01-01', '9:00');", NULL, NULL, NULL) != SQLITE_OK);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM calendar_entries;") == 0);
}

static void
test_import(sqlite3 *db, const char *poc)
{
    char buf[512];

    CHECK(store_import(db, "/nonexistent/poc.db") != 0);
    CHECK(store_import(db, poc) == 0);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM todos;") == 4);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM calendar_entries;") == 5);

    /* 2026-09-28 is a Monday; Standup is weekly on Monday (1) */
    agenda(db, "2026-09-28", "2026-10-04", buf, sizeof(buf));
    CHECK(strcmp(buf, "09-28 Standup; 09-30 Team meeting") == 0);
    agenda(db, "2026-10-05", "2026-10-05", buf, sizeof(buf));
    CHECK(strcmp(buf, "10-05 Doctor appointment; 10-05 Standup") == 0);
    agenda(db, "2027-03-15", "2027-03-15", buf, sizeof(buf));
    CHECK(strcmp(buf, "03-15 Birthday - Anna; 03-15 Standup") == 0);
    agenda(db, "2026-11-22", "2026-11-22", buf, sizeof(buf));
    CHECK(strcmp(buf, "11-22 Birthday - Piotr") == 0);
}

static void
test_recurrence(sqlite3 *db)
{
    Entry e;
    Occurrence *occ;
    int n;
    sqlite3_int64 id;
    char buf[512];

    sqlite3_exec(db, "DELETE FROM calendar_entries;", NULL, NULL, NULL);

    /* Monthly on the 31st: skipped in 30-day months and February */
    memset(&e, 0, sizeof(e));
    e.title = "Rent"; e.recurrence = REC_MONTHLY; e.day = 31; e.todo_id = -1;
    CHECK(store_add_entry(db, &e) > 0);
    n = store_occurrences(db, D("2027-01-01"), D("2027-12-31"), &occ);
    CHECK(n == 7);
    store_occurrences_free(occ, n);

    /* Yearly on 29.02: leap years only */
    e.title = "Leap"; e.recurrence = REC_YEARLY; e.day = 29; e.month = 2;
    id = store_add_entry(db, &e);
    CHECK(id > 0);
    n = store_occurrences(db, D("2027-02-01"), D("2027-03-31"), &occ);
    CHECK(n == 1);                                 /* only Rent 31.03 */
    store_occurrences_free(occ, n);
    agenda(db, "2028-02-29", "2028-02-29", buf, sizeof(buf));
    CHECK(strcmp(buf, "02-29 Leap") == 0);
    CHECK(store_delete_entry(db, id) == 0);

    /* Ordering: all-day first, then by time */
    memset(&e, 0, sizeof(e));
    e.todo_id = -1;
    e.date = D("2027-05-10");
    e.title = "Late";   strcpy(e.time, "18:30"); store_add_entry(db, &e);
    e.title = "Early";  strcpy(e.time, "07:05"); store_add_entry(db, &e);
    e.title = "All day"; e.time[0] = '\0';       store_add_entry(db, &e);
    e.title = "Daily"; e.recurrence = REC_DAILY; strcpy(e.time, "12:00");
    store_add_entry(db, &e);
    agenda(db, "2027-05-10", "2027-05-10", buf, sizeof(buf));
    CHECK(strcmp(buf, "05-10 All day; 05-10 07:05 Early; "
                      "05-10 12:00 Daily; 05-10 18:30 Late") == 0);

    /* Round trip through get / update */
    {
        Entry g;
        id = store_add_entry(db, &(Entry){ .title = "Gym", .description = "legs",
                                           .recurrence = REC_WEEKLY, .weekday = 3,
                                           .time = "19:00", .duration_min = 90,
                                           .todo_id = -1 });
        CHECK(store_get_entry(db, id, &g) == 0);
        CHECK(strcmp(g.title, "Gym") == 0 && strcmp(g.description, "legs") == 0);
        CHECK(g.recurrence == REC_WEEKLY && g.weekday == 3);
        CHECK(strcmp(g.time, "19:00") == 0 && g.duration_min == 90);
        CHECK(g.todo_id == -1);
        store_entry_free(&g);

        /* Turn it into a one-off entry */
        CHECK(store_update_entry(db, &(Entry){ .id = id, .title = "Gym",
                                               .recurrence = REC_NONE,
                                               .date = D("2027-06-02"),
                                               .todo_id = -1 }) == 0);
        CHECK(store_get_entry(db, id, &g) == 0);
        CHECK(g.recurrence == REC_NONE && g.date == D("2027-06-02"));
        CHECK(g.time[0] == '\0' && g.duration_min == 0 && g.weekday == 0);
        CHECK(g.description[0] == '\0');
        store_entry_free(&g);
    }
}

static void
test_todos(sqlite3 *db)
{
    TodoRow *rows;
    Todo t;
    int n, i;
    sqlite3_int64 a, b, eid;
    Day tomorrow = day_today() + 1;
    char tomorrow_s[11];

    sqlite3_exec(db, "DELETE FROM calendar_entries; DELETE FROM todos;",
                 NULL, NULL, NULL);
    day_format(tomorrow, tomorrow_s);

    a = store_add_todo(db, "Napisać raport", "Q3, dział sprzedaży", 3);
    b = store_add_todo(db, "Zadzwonić", NULL, 1);
    CHECK(a > 0 && b > 0);
    CHECK(store_add_todo(db, "Bad", NULL, 7) < 0);   /* priority 1..3 */

    /* Open first, by priority */
    n = store_todos(db, TODO_ALL, &rows);
    CHECK(n == 2 && rows[0].id == b && rows[1].id == a);
    CHECK(rows[1].next_date[0] == '\0');
    store_todos_free(rows, n);

    CHECK(store_get_todo(db, a, &t) == 0);
    CHECK(strcmp(t.title, "Napisać raport") == 0);
    CHECK(strcmp(t.description, "Q3, dział sprzedaży") == 0);
    CHECK(t.priority == 3 && !t.done);
    store_todo_free(&t);

    /* Scheduling copies title/description and links the entry back */
    eid = store_schedule_todo(db, a, tomorrow, "09:30");
    CHECK(eid > 0);
    CHECK(store_schedule_todo(db, 9999, tomorrow, NULL) < 0);
    {
        Entry e;
        CHECK(store_get_entry(db, eid, &e) == 0);
        CHECK(e.todo_id == a && e.date == tomorrow);
        CHECK(strcmp(e.title, "Napisać raport") == 0);
        CHECK(strcmp(e.time, "09:30") == 0);
        store_entry_free(&e);
    }
    n = store_todos(db, TODO_ALL, &rows);
    for (i = 0; i < n; i++)
        if (rows[i].id == a)
            CHECK(strcmp(rows[i].next_date, tomorrow_s) == 0);
    store_todos_free(rows, n);

    /* Filters */
    CHECK(store_set_todo_done(db, b, 1) == 0);
    n = store_todos(db, TODO_OPEN, &rows);
    CHECK(n == 1 && rows[0].id == a);
    store_todos_free(rows, n);
    n = store_todos(db, TODO_DONE, &rows);
    CHECK(n == 1 && rows[0].id == b && rows[0].done);
    store_todos_free(rows, n);

    /* Deleting the todo keeps the entry, without the link */
    CHECK(store_delete_todo(db, a) == 0);
    {
        Entry e;
        CHECK(store_get_entry(db, eid, &e) == 0);
        CHECK(e.todo_id == -1);
        store_entry_free(&e);
    }
}


/* Parse s with today = Monday 2026-09-28; "title|YYYY-MM-DD|HH:MM|prio"
 * (empty fields when not set), or "ERR:message" */
static void
qa(const char *s, const char *want)
{
    QuickAdd q;
    char err[100], got[400], d[11] = "";

    if (!quickadd_parse(s, D("2026-09-28"), &q, err, sizeof(err))) {
        snprintf(got, sizeof(got), "ERR:%s", err);
    } else {
        if (q.has_date) day_format(q.date, d);
        snprintf(got, sizeof(got), "%s|%s|%s|%d", q.title, d, q.time, q.priority);
    }
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "quickadd \"%s\": got \"%s\", want \"%s\"\n", s, got, want);
        g_fail++;
    }
}

static void
test_quickadd(int utf8_locale)
{
    Day  d;
    char t[6], err[100];

    qa("dentist tomorrow 3pm",         "dentist|2026-09-29|15:00|0");
    qa("Dentysta w piątek o 15:00",    "Dentysta|2026-10-02|15:00|0");
    qa("Kupić mleko !high",            "Kupić mleko|||1");
    qa("idea !low  z  odstępami",      "idea z odstępami|||3");
    qa("Spotkanie 30.09 9:30",         "Spotkanie|2026-09-30|09:30|0");
    qa("urodziny 15.03",               "urodziny|2027-03-15||0");
    qa("rocznica 28.09.",              "rocznica|2026-09-28||0");
    qa("przestępny 29.02",             "przestępny|2028-02-29||0");
    qa("wyjazd 1.10.2027",             "wyjazd|2027-10-01||0");
    qa("raport +3d",                   "raport|2026-10-01||0");
    qa("urlop +2w",                    "urlop|2026-10-12||0");
    qa("call monday",                  "call|2026-10-05||0");   /* next, not today */
    qa("sprawdzić dziś 12:00am",       "sprawdzić|2026-09-28|00:00|0");
    qa("lunch 13:15",                  "lunch|2026-09-28|13:15|0");
    qa("3 jabłka",                     "3 jabłka|||0");
    qa("wyjście o 18",                 "wyjście o 18|||0");     /* 18 is not a time */
    qa("x 31.02",                      "ERR:No such date: 31.02");
    qa("x 2026-02-30",                 "ERR:No such date: 2026-02-30");
    qa("x 25:00",                      "ERR:No such time: 25:00");
    qa("x 13pm",                       "ERR:No such time: 13pm");
    qa("x jutro pojutrze",             "ERR:More than one date: pojutrze");
    qa("jutro 15:00",                  "ERR:Missing title");
    if (utf8_locale)
        qa("Kino w PIĄTEK",            "Kino|2026-10-02||0");

    CHECK(quickadd_parse_when("jutro 15:00", D("2026-09-28"), &d, t, err, sizeof(err)));
    CHECK(d == D("2026-09-29") && strcmp(t, "15:00") == 0);
    CHECK(quickadd_parse_when("30.09", D("2026-09-28"), &d, t, err, sizeof(err)));
    CHECK(d == D("2026-09-30") && t[0] == '\0');
    CHECK(!quickadd_parse_when("jutro kino", D("2026-09-28"), &d, t, err, sizeof(err)));
    CHECK(!quickadd_parse_when("", D("2026-09-28"), &d, t, err, sizeof(err)));
}

int
main(int argc, char **argv)
{
    sqlite3 *db;
    int utf8_locale;

    if (argc != 2) {
        fprintf(stderr, "usage: %s poc.db\n", argv[0]);
        return 2;
    }
    /* Polish case folding in towlower() needs a UTF-8 locale */
    utf8_locale = setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "C.utf8") ||
                  setlocale(LC_CTYPE, "en_US.UTF-8");
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return 2;
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);

    test_dates();
    test_schema(db);
    test_import(db, argv[1]);
    test_recurrence(db);
    test_todos(db);
    test_quickadd(utf8_locale);

    sqlite3_close(db);
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
