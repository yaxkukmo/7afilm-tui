/*
 * 7aorganizer-tui.c - personal organizer: calendar entries and todos (curses)
 *
 * Database: ~/.7a/organizer.db
 *
 *   7aorganizer-tui                  start the TUI
 *   7aorganizer-tui --import FILE    copy todos and calendar entries from a
 *                                    poc.db-style database (target must be
 *                                    empty)
 *
 * Keys:
 *   D / T          Dashboard / Todo view
 *   Up / Down      move in the list (j / k too), or scroll the viewer
 *   PgUp / PgDn    page;  Home / End  first / last item
 *   Tab            switch focus: list <-> viewer;  Esc back to the list
 *   Space          toggle done (Todo view)
 *   f              Todo filter: open -> done -> all
 *   n              quick add: "dentysta jutro 15:00" goes to the calendar,
 *                  text without a date or time becomes a todo (see quickadd.h)
 *   s              schedule the selected todo ("jutro 15:00") (Todo view)
 *   x              delete the selected item (asks y/n)
 *   q / Ctrl+Q     quit
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "date.h"
#include "db.h"
#include "inputline.h"
#include "quickadd.h"
#include "store.h"
#include "tui.h"
#include "utf8.h"

#define PROG "7aorganizer-tui"

#define VIEW_DASHBOARD 0
#define VIEW_TODO      1
#define VIEW_COUNT     2

#define ROW_HEADER 0   /* group title                                  */
#define ROW_EMPTY  1   /* placeholder / spacer, not selectable         */
#define ROW_OCC    2   /* g_occ[idx]                                   */
#define ROW_TODO   3   /* g_todos[idx]                                 */

#define GROUP_TODAY    0
#define GROUP_TOMORROW 1
#define GROUP_WEEK     2
#define WEEK_DAYS      7   /* the dashboard covers today + 6 days */

#define MIN_VIEWER_COLS 70 /* narrower terminals show the list only */

typedef struct {
    int         kind;    /* ROW_* */
    int         idx;     /* into g_occ or g_todos */
    int         group;   /* GROUP_* for dashboard rows */
    const char *text;    /* ROW_HEADER / ROW_EMPTY */
} Row;

static const char *const weekday_short[8] = {
    "", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};
static const char *const weekday_long[8] = {
    "", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday", "Sunday"
};
static const char *const view_names[VIEW_COUNT] = { "Dashboard", "Todo" };
static const char *const filter_names[3]        = { "all", "open", "done" };
static const char *const priority_names[4]      = { "", "high", "normal", "low" };

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static sqlite3 *g_db;
static int      g_view          = VIEW_DASHBOARD;
static int      g_viewer_focus  = 0;
static int      g_viewer_scroll = 0;
static int      g_viewer_lines  = 0;   /* lines in the last drawn viewer */
static int      g_want_quit     = 0;
static Day      g_today;

/* Model, rebuilt by load_model() */
static Occurrence *g_occ    = NULL;
static int         g_nocc   = 0;
static TodoRow    *g_todos  = NULL;
static int         g_ntodos = 0;
static int         g_todo_filter = TODO_OPEN;

static Row *g_rows     = NULL;
static int  g_nrows    = 0;
static int  g_rows_cap = 0;
static int  g_sel[VIEW_COUNT];         /* selected row, -1 = none */
static int  g_scroll[VIEW_COUNT];

/* Pending y/n question */
static int           g_confirm_open = 0;
static char          g_confirm_msg[200];
static void        (*g_confirm_action)(void);
static sqlite3_int64 g_pending_id;

/* Prompt in the bottom bar */
#define INPUT_NEW      0
#define INPUT_SCHEDULE 1
static InputLine     g_input;
static int           g_input_mode;
static sqlite3_int64 g_input_todo;   /* INPUT_SCHEDULE */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* "Wed 30.09.2026" */
static void
fmt_day(Day day, char *out, size_t outsz)
{
    int y, m, d;
    day_to_ymd(day, &y, &m, &d);
    snprintf(out, outsz, "%s %02d.%02d.%04d", weekday_short[day_weekday(day)], d, m, y);
}

/* Copy s into out, cut to `cols` columns with "..." when longer */
static void
clip(const char *s, int cols, char *out, size_t outsz)
{
    if (utf8_width(s) <= cols)
        snprintf(out, outsz, "%s", s);
    else
        snprintf(out, outsz, "%.*s...", (int)utf8_fit(s, cols - 3, NULL), s);
}

/* Column of the list / viewer divider, or 0 when there is no viewer */
static int
viewer_split(void)
{
    int cols = getmaxx(stdscr);
    int vw   = cols / 3;
    if (cols < MIN_VIEWER_COLS) return 0;
    if (vw < 26) vw = 26;
    if (vw > 50) vw = 50;
    return cols - vw - 1;
}

/* ------------------------------------------------------------------ */
/* Model                                                               */
/* ------------------------------------------------------------------ */

static void
add_row(int kind, int idx, int group, const char *text)
{
    if (g_nrows >= g_rows_cap) {
        int  nc  = g_rows_cap ? g_rows_cap * 2 : 32;
        Row *tmp = realloc(g_rows, (size_t)nc * sizeof(*tmp));
        if (!tmp) return;
        g_rows     = tmp;
        g_rows_cap = nc;
    }
    g_rows[g_nrows].kind  = kind;
    g_rows[g_nrows].idx   = idx;
    g_rows[g_nrows].group = group;
    g_rows[g_nrows].text  = text;
    g_nrows++;
}

static int
occ_group(Day d)
{
    if (d == g_today)     return GROUP_TODAY;
    if (d == g_today + 1) return GROUP_TOMORROW;
    return GROUP_WEEK;
}

static void
build_dashboard_rows(void)
{
    static const char *const headers[3] = { "TODAY", "TOMORROW", "THIS WEEK" };
    int g, i, any;

    for (g = GROUP_TODAY; g <= GROUP_WEEK; g++) {
        if (g > GROUP_TODAY)
            add_row(ROW_EMPTY, 0, g, "");
        add_row(ROW_HEADER, 0, g, headers[g]);
        any = 0;
        for (i = 0; i < g_nocc; i++) {
            if (occ_group(g_occ[i].date) == g) {
                add_row(ROW_OCC, i, g, NULL);
                any = 1;
            }
        }
        if (!any)
            add_row(ROW_EMPTY, 0, g, "nothing planned");
    }
}

static void
build_todo_rows(void)
{
    int i;
    for (i = 0; i < g_ntodos; i++)
        add_row(ROW_TODO, i, 0, NULL);
    if (g_ntodos == 0)
        add_row(ROW_EMPTY, 0, 0, g_todo_filter == TODO_DONE ? "nothing done yet"
                                                            : "no todos");
}

static int
selectable(int r)
{
    return r >= 0 && r < g_nrows &&
           (g_rows[r].kind == ROW_OCC || g_rows[r].kind == ROW_TODO);
}

/* Keep the selection on a selectable row, near where it was */
static void
fix_selection(void)
{
    int *sel = &g_sel[g_view];
    int  r;

    if (*sel >= g_nrows) *sel = g_nrows - 1;
    if (*sel < 0)        *sel = 0;
    if (selectable(*sel)) return;
    for (r = *sel + 1; r < g_nrows; r++)
        if (selectable(r)) { *sel = r; return; }
    for (r = *sel - 1; r >= 0; r--)
        if (selectable(r)) { *sel = r; return; }
    *sel = -1;
}

static void
load_model(void)
{
    int n;

    store_occurrences_free(g_occ, g_nocc);
    store_todos_free(g_todos, g_ntodos);
    g_occ   = NULL; g_nocc   = 0;
    g_todos = NULL; g_ntodos = 0;
    g_nrows = 0;
    g_today = day_today();

    if (g_view == VIEW_DASHBOARD) {
        n = store_occurrences(g_db, g_today, g_today + WEEK_DAYS - 1, &g_occ);
        if (n < 0)
            snprintf(g_status, sizeof(g_status), "Database error: %s",
                     sqlite3_errmsg(g_db));
        g_nocc = n < 0 ? 0 : n;
        build_dashboard_rows();
    } else {
        n = store_todos(g_db, g_todo_filter, &g_todos);
        if (n < 0)
            snprintf(g_status, sizeof(g_status), "Database error: %s",
                     sqlite3_errmsg(g_db));
        g_ntodos = n < 0 ? 0 : n;
        build_todo_rows();
    }
    fix_selection();
}

/* Move the selection by `steps` selectable rows (negative = up) */
static void
move_sel(int steps)
{
    int *sel = &g_sel[g_view];
    int  dir = steps < 0 ? -1 : 1;
    int  r;

    if (*sel < 0) return;
    for (; steps != 0; steps -= dir) {
        for (r = *sel + dir; r >= 0 && r < g_nrows && !selectable(r); r += dir)
            ;
        if (!selectable(r)) break;
        *sel = r;
    }
    g_viewer_scroll = 0;
}

static const Occurrence *
selected_occ(void)
{
    int r = g_sel[g_view];
    return (selectable(r) && g_rows[r].kind == ROW_OCC) ? &g_occ[g_rows[r].idx] : NULL;
}

static const TodoRow *
selected_todo(void)
{
    int r = g_sel[g_view];
    return (selectable(r) && g_rows[r].kind == ROW_TODO) ? &g_todos[g_rows[r].idx] : NULL;
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void
switch_view(int view)
{
    g_view          = view;
    g_viewer_focus  = 0;
    g_viewer_scroll = 0;
    load_model();
}

static void
do_delete_entry(void)
{
    if (store_delete_entry(g_db, g_pending_id) == 0)
        snprintf(g_status, sizeof(g_status), "Deleted.");
    else
        snprintf(g_status, sizeof(g_status), "Delete failed: %s", sqlite3_errmsg(g_db));
}

static void
do_delete_todo(void)
{
    if (store_delete_todo(g_db, g_pending_id) == 0)
        snprintf(g_status, sizeof(g_status), "Deleted.");
    else
        snprintf(g_status, sizeof(g_status), "Delete failed: %s", sqlite3_errmsg(g_db));
}

static void
ask(const char *question, void (*action)(void), sqlite3_int64 id)
{
    snprintf(g_confirm_msg, sizeof(g_confirm_msg), "%s", question);
    g_confirm_action = action;
    g_pending_id     = id;
    g_confirm_open   = 1;
}

static void
ask_delete(void)
{
    const Occurrence *o = selected_occ();
    const TodoRow    *t = selected_todo();
    char title[120], q[200];

    if (o) {
        clip(o->title, 30, title, sizeof(title));
        snprintf(q, sizeof(q), o->recurring ? "Delete \"%s\" and all its repeats?"
                                            : "Delete \"%s\"?", title);
        ask(q, do_delete_entry, o->entry_id);
    } else if (t) {
        clip(t->title, 30, title, sizeof(title));
        snprintf(q, sizeof(q), "Delete todo \"%s\"?", title);
        ask(q, do_delete_todo, t->id);
    }
}

static void
start_new(void)
{
    g_input_mode = INPUT_NEW;
    inputline_open(&g_input, "New: ",
                   "e.g. dentysta jutro 15:00   or   pomysł !high");
}

static void
start_schedule(void)
{
    const TodoRow *t = selected_todo();

    if (!t) {
        snprintf(g_status, sizeof(g_status), "Select a todo to schedule (Todo view).");
        return;
    }
    g_input_mode = INPUT_SCHEDULE;
    g_input_todo = t->id;
    inputline_open(&g_input, "Schedule on: ", "e.g. jutro 15:00, pt, 30.09");
}

static void
submit_new(void)
{
    QuickAdd qa;
    char     err[100], when[40];

    if (!quickadd_parse(g_input.text, day_today(), &qa, err, sizeof(err))) {
        snprintf(g_status, sizeof(g_status), "%s", err);
        return;                                 /* keep the prompt open */
    }
    if (qa.has_date) {
        Entry e;
        memset(&e, 0, sizeof(e));
        e.title      = qa.title;
        e.recurrence = REC_NONE;
        e.date       = qa.date;
        e.todo_id    = -1;
        memcpy(e.time, qa.time, sizeof(e.time));
        if (store_add_entry(g_db, &e) < 0) {
            snprintf(g_status, sizeof(g_status), "Add failed: %s", sqlite3_errmsg(g_db));
            return;
        }
        fmt_day(qa.date, when, sizeof(when));
        snprintf(g_status, sizeof(g_status), "Added to the calendar: %s%s%s%s",
                 when, qa.time[0] ? " " : "", qa.time,
                 qa.priority ? " (priority is for todos only)" : "");
    } else {
        if (store_add_todo(g_db, qa.title, NULL, qa.priority ? qa.priority : 2) < 0) {
            snprintf(g_status, sizeof(g_status), "Add failed: %s", sqlite3_errmsg(g_db));
            return;
        }
        snprintf(g_status, sizeof(g_status), "Added a todo.");
    }
    inputline_close(&g_input);
    load_model();
}

static void
submit_schedule(void)
{
    Day  date;
    char time[6], err[100], when[40];

    if (!quickadd_parse_when(g_input.text, day_today(), &date, time, err, sizeof(err))) {
        snprintf(g_status, sizeof(g_status), "%s", err);
        return;
    }
    if (store_schedule_todo(g_db, g_input_todo, date, time) < 0) {
        snprintf(g_status, sizeof(g_status), "Schedule failed: %s", sqlite3_errmsg(g_db));
        return;
    }
    fmt_day(date, when, sizeof(when));
    snprintf(g_status, sizeof(g_status), "Scheduled on %s%s%s",
             when, time[0] ? " " : "", time);
    inputline_close(&g_input);
    load_model();
}

static void
toggle_done(void)
{
    const TodoRow *t = selected_todo();

    if (!t) {
        snprintf(g_status, sizeof(g_status), "Space marks todos done (Todo view).");
        return;
    }
    if (store_set_todo_done(g_db, t->id, !t->done) != 0) {
        snprintf(g_status, sizeof(g_status), "Update failed: %s", sqlite3_errmsg(g_db));
        return;
    }
    snprintf(g_status, sizeof(g_status), t->done ? "Reopened." : "Done.");
    load_model();
}

/* ------------------------------------------------------------------ */
/* Drawing: list                                                       */
/* ------------------------------------------------------------------ */

static void
draw_list_row(int y, int x, int w, const Row *row, int selected)
{
    char buf[600];
    attr_t attr = 0;

    switch (row->kind) {
    case ROW_HEADER:
        if (row->group == GROUP_WEEK) {
            snprintf(buf, sizeof(buf), " %s", row->text);
        } else {
            char d[20];
            fmt_day(g_today + (row->group == GROUP_TOMORROW), d, sizeof(d));
            snprintf(buf, sizeof(buf), " %s  %s", row->text, d);
        }
        attr = A_BOLD;
        break;

    case ROW_EMPTY:
        snprintf(buf, sizeof(buf), "   %s", row->text);
        attr = A_DIM;
        break;

    case ROW_OCC: {
        const Occurrence *o = &g_occ[row->idx];
        if (row->group == GROUP_WEEK) {
            int yy, m, d;
            day_to_ymd(o->date, &yy, &m, &d);
            snprintf(buf, sizeof(buf), "   %s %02d.%02d  %-5s  %s",
                     weekday_short[day_weekday(o->date)], d, m, o->time, o->title);
        } else {
            snprintf(buf, sizeof(buf), "   %-5s  %s", o->time, o->title);
        }
        break;
    }

    case ROW_TODO: {
        const TodoRow *t = &g_todos[row->idx];
        int dw = 0;
        snprintf(buf, sizeof(buf), "   [%c] %c %s", t->done ? 'x' : ' ',
                 t->priority == 1 ? '!' : ' ', t->title);
        if (t->done || t->priority == 3)
            attr = A_DIM;
        if (t->next_date[0]) {
            /* scheduled: date at the right edge */
            Day nd;
            char ds[12];
            if (day_parse(t->next_date, &nd)) {
                int yy, m, d;
                day_to_ymd(nd, &yy, &m, &d);
                snprintf(ds, sizeof(ds), " %02d.%02d ", d, m);
                dw = (int)strlen(ds);
                if (dw < w) {
                    attron(COLOR_PAIR(CP_BOX) | (selected ? A_REVERSE : 0));
                    tui_put_text(y, x + w - dw, dw, ds);
                    attroff(COLOR_PAIR(CP_BOX) | A_REVERSE);
                } else {
                    dw = 0;
                }
            }
        }
        w -= dw;
        break;
    }
    }

    if (selected) attr = (attr & ~A_DIM) | A_REVERSE | (g_viewer_focus ? 0 : A_BOLD);
    attron(COLOR_PAIR(CP_BOX) | attr);
    tui_put_text(y, x, w, buf);
    attroff(COLOR_PAIR(CP_BOX) | attr);
}

static void
draw_list(int top, int x, int w, int h)
{
    int *sel    = &g_sel[g_view];
    int *scroll = &g_scroll[g_view];
    int  max_scroll = g_nrows - h;
    int  i;

    if (*sel >= 0) {
        if (*sel < *scroll)      *scroll = *sel;
        if (*sel >= *scroll + h) *scroll = *sel - h + 1;
        /* keep the group title above the first item of a group visible */
        if (*sel == *scroll && *sel > 0 && g_rows[*sel - 1].kind == ROW_HEADER)
            (*scroll)--;
    }
    if (max_scroll < 0)       max_scroll = 0;
    if (*scroll > max_scroll) *scroll = max_scroll;
    if (*scroll < 0)          *scroll = 0;

    for (i = 0; i < h && *scroll + i < g_nrows; i++)
        draw_list_row(top + i, x, w, &g_rows[*scroll + i], *scroll + i == *sel);

    /* scroll hints on the left border */
    attron(COLOR_PAIR(CP_BOX_LINE));
    if (*scroll > 0)             mvaddch(top, x - 1, ACS_UARROW);
    if (*scroll + h < g_nrows)   mvaddch(top + h - 1, x - 1, ACS_DARROW);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

/* ------------------------------------------------------------------ */
/* Drawing: viewer                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int top, x, w, h;
    int line;           /* next content line, before scrolling */
} Pane;

/* Wrapped text; "" gives an empty line */
static void
pane_text(Pane *p, attr_t attr, const char *s)
{
    do {
        size_t next, len = utf8_wrap(s, p->w, &next);
        int    vi = p->line - g_viewer_scroll;
        if (vi >= 0 && vi < p->h) {
            attron(COLOR_PAIR(CP_BOX) | attr);
            mvaddnstr(p->top + vi, p->x, s, (int)len);
            attroff(COLOR_PAIR(CP_BOX) | attr);
        }
        p->line++;
        s += next;
    } while (*s);
}

static void
pane_field(Pane *p, const char *label, const char *value)
{
    char buf[300];
    snprintf(buf, sizeof(buf), "%-10s %s", label, value);
    pane_text(p, 0, buf);
}

static void
pane_rule(Pane *p)
{
    int vi = p->line - g_viewer_scroll;
    if (vi >= 0 && vi < p->h) {
        attron(COLOR_PAIR(CP_BOX_LINE));
        mvhline(p->top + vi, p->x, g_hl, p->w);
        attroff(COLOR_PAIR(CP_BOX_LINE));
    }
    p->line++;
}

static void
pane_description(Pane *p, const char *desc)
{
    pane_text(p, 0, "");
    if (desc[0]) pane_text(p, 0, desc);
    else         pane_text(p, A_DIM, "(no description)");
}

static void
draw_viewer_occ(Pane *p, const Occurrence *o)
{
    Entry e;
    char  buf[100];

    if (store_get_entry(g_db, o->entry_id, &e) != 0) {
        pane_text(p, A_DIM, "(entry not found)");
        return;
    }
    pane_text(p, A_BOLD, e.title);
    pane_rule(p);

    fmt_day(o->date, buf, sizeof(buf));
    pane_field(p, "Date:", buf);

    if (e.time[0] && e.duration_min)
        snprintf(buf, sizeof(buf), "%s (%d min)", e.time, e.duration_min);
    else
        snprintf(buf, sizeof(buf), "%s", e.time[0] ? e.time : "all day");
    pane_field(p, "Time:", buf);

    switch (e.recurrence) {
    case REC_DAILY:
        pane_field(p, "Repeats:", "every day");
        break;
    case REC_WEEKLY:
        snprintf(buf, sizeof(buf), "every %s",
                 e.weekday >= 1 && e.weekday <= 7 ? weekday_long[e.weekday] : "?");
        pane_field(p, "Repeats:", buf);
        break;
    case REC_MONTHLY:
        snprintf(buf, sizeof(buf), "monthly on day %d", e.day);
        pane_field(p, "Repeats:", buf);
        break;
    case REC_YEARLY:
        snprintf(buf, sizeof(buf), "yearly on %02d.%02d", e.day, e.month);
        pane_field(p, "Repeats:", buf);
        break;
    }
    if (e.todo_id >= 0)
        pane_field(p, "From:", "a todo");

    pane_description(p, e.description);
    store_entry_free(&e);
}

static void
draw_viewer_todo(Pane *p, const TodoRow *row)
{
    Todo t;
    char buf[100];
    Day  d;

    if (store_get_todo(g_db, row->id, &t) != 0) {
        pane_text(p, A_DIM, "(todo not found)");
        return;
    }
    pane_text(p, A_BOLD, t.title);
    pane_rule(p);
    pane_field(p, "Priority:", t.priority >= 1 && t.priority <= 3
                               ? priority_names[t.priority] : "?");
    pane_field(p, "Status:", t.done ? "done" : "open");
    if (row->next_date[0] && day_parse(row->next_date, &d)) {
        fmt_day(d, buf, sizeof(buf));
        pane_field(p, "Scheduled:", buf);
    }
    pane_description(p, t.description);
    store_todo_free(&t);
}

static void
draw_viewer(int top, int x, int w, int h)
{
    Pane p = { top, x, w, h, 0 };
    const Occurrence *o = selected_occ();
    const TodoRow    *t = selected_todo();

    if (o)      draw_viewer_occ(&p, o);
    else if (t) draw_viewer_todo(&p, t);
    else        pane_text(&p, A_DIM, "Nothing selected.");
    g_viewer_lines = p.line;

    attron(COLOR_PAIR(CP_BOX_LINE));
    if (g_viewer_scroll > 0)               mvaddch(top, x + w, ACS_UARROW);
    if (g_viewer_scroll + h < p.line)      mvaddch(top + h - 1, x + w, ACS_DARROW);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

/* ------------------------------------------------------------------ */
/* Drawing: frame                                                      */
/* ------------------------------------------------------------------ */

static void
draw_title(int col, const char *title, int focused)
{
    attron(COLOR_PAIR(CP_BOX_LINE) | A_BOLD | (focused ? A_REVERSE : 0));
    mvprintw(0, col, " %s ", title);
    attroff(COLOR_PAIR(CP_BOX_LINE) | A_BOLD | A_REVERSE);
}

static void
draw_all(void)
{
    int rows  = getmaxy(stdscr);
    int cols  = getmaxx(stdscr);
    int split = viewer_split();
    int body_h = rows - 4;
    int right  = split ? split : cols - 1;   /* list's right border */
    char title[64];
    int r;

    erase();
    if (rows < 8 || cols < 30) {
        mvprintw(0, 0, "Terminal too small");
        refresh();
        return;
    }

    /* Background and frame */
    attron(COLOR_PAIR(CP_BOX));
    for (r = 1; r < rows - 1; r++)
        mvhline(r, 1, ' ', cols - 2);
    attroff(COLOR_PAIR(CP_BOX));

    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(0, 0, g_ul);
    mvhline(0, 1, g_hl, cols - 2);
    mvaddch(0, cols - 1, g_ur);
    mvvline(1, 0, g_vl, rows - 2);
    mvvline(1, cols - 1, g_vl, rows - 2);
    mvaddch(rows - 3, 0, g_lt);
    mvhline(rows - 3, 1, g_hl, cols - 2);
    mvaddch(rows - 3, cols - 1, g_rt);
    mvaddch(rows - 1, 0, g_ll);
    mvhline(rows - 1, 1, g_hl, cols - 2);
    mvaddch(rows - 1, cols - 1, g_lr);
    if (split) {
        mvaddch(0, split, g_tt);
        mvvline(1, split, g_vl, body_h);
        mvaddch(rows - 3, split, g_bt);
    }
    attroff(COLOR_PAIR(CP_BOX_LINE));

    /* Panes */
    if (g_view == VIEW_TODO)
        snprintf(title, sizeof(title), "Todo: %s", filter_names[g_todo_filter]);
    else
        snprintf(title, sizeof(title), "%s", view_names[g_view]);
    draw_title(2, title, !g_viewer_focus);
    draw_list(1, 1, right - 1, body_h);
    if (split) {
        draw_title(split + 2, "Viewer", g_viewer_focus);
        draw_viewer(1, split + 2, cols - split - 4, body_h);
    }

    /* Status or key help */
    if (g_status[0]) {
        attron(COLOR_PAIR(CP_BOX) | A_BOLD);
        tui_put_text(rows - 2, 2, cols - 4, g_status);
        attroff(COLOR_PAIR(CP_BOX) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CP_BOX) | A_DIM);
        tui_put_text(rows - 2, 2, cols - 4, g_view == VIEW_TODO
            ? "n new  s schedule  Space done  f filter  x delete  D T views  Tab viewer  q quit"
            : "n new  x delete  D T views  Tab viewer  q quit");
        attroff(COLOR_PAIR(CP_BOX) | A_DIM);
    }

    /* Last, so the cursor stays where the prompt put it */
    if (g_confirm_open) {
        draw_confirm_box(g_confirm_msg);
    } else if (g_input.open) {
        if (g_status[0]) {                      /* e.g. a parse error */
            attron(COLOR_PAIR(CP_BOX_LINE) | A_BOLD);
            mvprintw(rows - 3, 2, " %.*s ", (int)utf8_fit(g_status, cols - 6, NULL),
                     g_status);
            attroff(COLOR_PAIR(CP_BOX_LINE) | A_BOLD);
        }
        inputline_draw(&g_input, rows - 2, 2, cols - 4);
    } else {
        curs_set(0);
    }
    refresh();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void
handle_key(int ch)
{
    int page = getmaxy(stdscr) - 5;
    if (page < 1) page = 1;

    if (g_confirm_open) {
        if (ch == 'y' || ch == 'Y') {
            g_confirm_open = 0;
            g_confirm_action();
            load_model();
        } else if (ch == 'n' || ch == 'N' || ch == 27) {
            g_confirm_open = 0;
        }
        return;
    }

    if (g_input.open) {
        g_status[0] = '\0';
        switch (inputline_key(&g_input, ch)) {
        case IL_SUBMIT:
            if (g_input_mode == INPUT_NEW) submit_new();
            else                           submit_schedule();
            break;
        case IL_CANCEL:
            g_status[0] = '\0';
            break;
        }
        return;
    }

    g_status[0] = '\0';
    switch (ch) {
    case 'n':
        start_new();
        return;
    case 's':
        start_schedule();
        return;
    case 'q':
    case 'q' & 0x1f:
        g_want_quit = 1;
        return;
    case 'D': case 'd':
        switch_view(VIEW_DASHBOARD);
        return;
    case 'T': case 't':
        switch_view(VIEW_TODO);
        return;
    case '\t':
        if (viewer_split()) g_viewer_focus = !g_viewer_focus;
        return;
    case 27: /* ESC */
        g_viewer_focus = 0;
        return;
    case 'x':
        ask_delete();
        return;
    case ' ':
        toggle_done();
        return;
    case 'f':
        if (g_view == VIEW_TODO) {
            g_todo_filter = (g_todo_filter + 1) % 3; /* open -> done -> all */
            load_model();
        }
        return;
    }

    if (g_viewer_focus) {
        int max = g_viewer_lines - (getmaxy(stdscr) - 4);
        if (max < 0) max = 0;
        if (ch == KEY_UP || ch == 'k')   g_viewer_scroll--;
        if (ch == KEY_DOWN || ch == 'j') g_viewer_scroll++;
        if (ch == KEY_PPAGE)             g_viewer_scroll -= page;
        if (ch == KEY_NPAGE)             g_viewer_scroll += page;
        if (ch == KEY_HOME)              g_viewer_scroll = 0;
        if (ch == KEY_END)               g_viewer_scroll = max;
        if (g_viewer_scroll > max) g_viewer_scroll = max;
        if (g_viewer_scroll < 0)   g_viewer_scroll = 0;
        return;
    }

    switch (ch) {
    case KEY_UP:    case 'k': move_sel(-1);        break;
    case KEY_DOWN:  case 'j': move_sel(+1);        break;
    case KEY_PPAGE:           move_sel(-page);     break;
    case KEY_NPAGE:           move_sel(+page);     break;
    case KEY_HOME:            move_sel(-g_nrows);  break;
    case KEY_END:             move_sel(+g_nrows);  break;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static int
import_file(const char *path)
{
    if (!store_is_empty(g_db)) {
        fprintf(stderr, PROG ": ~/.7a/organizer.db is not empty; "
                "import only into an empty database\n");
        return 1;
    }
    if (store_import(g_db, path) != 0)
        return 1;
    printf("Imported %s\n", path);
    return 0;
}

int
main(int argc, char **argv)
{
    const char *import = NULL;
    int ch;

    if (argc == 3 && strcmp(argv[1], "--import") == 0) {
        import = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: " PROG " [--import FILE]\n");
        return 2;
    }

    g_db = db_open(PROG, "organizer.db");
    if (store_init(g_db) != 0) {
        sqlite3_close(g_db);
        return 1;
    }
    if (import) {
        int rc = import_file(import);
        sqlite3_close(g_db);
        return rc;
    }

#ifdef __OpenBSD__
    if (pledge("stdio rpath wpath cpath flock tty", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    tui_init();
    timeout(250);   /* wake up for resizes and the date changing */
    load_model();
    draw_all();

    while (!g_want_quit) {
        ch = tui_getkey();
        if (ch != ERR) {
            handle_key(ch);
            draw_all();
        } else if (g_resize) {
            g_resize = 0;
            endwin();
            refresh();
            clear();
            draw_all();
        } else if (day_today() != g_today) {
            load_model();
            draw_all();
        }
    }

    endwin();
    store_occurrences_free(g_occ, g_nocc);
    store_todos_free(g_todos, g_ntodos);
    free(g_rows);
    sqlite3_close(g_db);
    return 0;
}
