#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "edit.h"
#include "date.h"
#include "form.h"
#include "quickadd.h"
#include "store.h"
#include "tui.h"
#include "utf8.h"

#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KIND_ENTRY 0
#define KIND_TODO  1

#define ACT_NONE   0
#define ACT_SAVE   1
#define ACT_CANCEL 2
#define ACT_NOTES  3   /* open the notes in the editor */

#define NOTES_LEN  8192

/* Option order matches REC_*, weekday - 1, priority - 1 and done */
static const char *repeat_opts[]  = { "none", "daily", "weekly", "monthly", "yearly", NULL };
static const char *weekday_opts[] = { "Monday", "Tuesday", "Wednesday", "Thursday",
                                      "Friday", "Saturday", "Sunday", NULL };
static const char *prio_opts[]    = { "high", "normal", "low", NULL };
static const char *status_opts[]  = { "open", "done", NULL };
static const char *type_opts[]    = { "calendar entry", "todo", NULL };  /* KIND_* */

static struct {
    int           open;
    int           is_new;       /* adding: Type can switch the kind */
    int           scheduling;   /* adding an entry for a todo: no Type */
    int           kind;
    int           action;       /* set by the buttons */
    sqlite3      *db;
    sqlite3_int64 id;
    sqlite3_int64 todo_id;      /* entry: kept as it was */
    char          err[128];

    char title[QA_TITLE_LEN];
    char desc[NOTES_LEN];       /* may have several lines */
    char date[32];              /* entry, REC_NONE: any quick add date */
    char time[8];
    char duration[12];
    char day[4], month[4];

    /* dropdowns: index plus a text buffer the widget writes to */
    int  repeat;   char repeat_buf[16];
    int  weekday;  char weekday_buf[16];
    int  prio;     char prio_buf[16];
    int  status;   char status_buf[16];
    int  type;     char type_buf[20];   /* new items: KIND_* */

    /* What the last save stored, for the caller to select it */
    int           saved_todo;
    sqlite3_int64 saved_id;
    Day           saved_date;
} g_edit;

/* One-line editing: newlines and tabs become spaces */
static void
flatten(char *dst, size_t dstsz, const char *src)
{
    char *p;
    snprintf(dst, dstsz, "%s", src);
    for (p = dst; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
}

/* Notes keep their lines; tabs become spaces, CRs and trailing blank
 * lines go.  Returns 0 when the text had to be cut. */
static int
set_notes(const char *src)
{
    char  *d = g_edit.desc;
    size_t n = 0;

    for (; *src && n < sizeof(g_edit.desc) - 1; src++) {
        if (*src == '\r') continue;
        d[n++] = *src == '\t' ? ' ' : *src;
    }
    /* cut in the middle of a UTF-8 character: drop its first bytes too */
    if (((unsigned char)*src & 0xC0) == 0x80) {
        while (n > 0 && ((unsigned char)d[n - 1] & 0xC0) == 0x80)
            n--;
        if (n > 0 && ((unsigned char)d[n - 1] & 0x80))
            n--;
    }
    while (n > 0 && (d[n - 1] == '\n' || d[n - 1] == ' '))
        n--;
    d[n] = '\0';
    while (*src == '\n' || *src == '\r' || *src == ' ' || *src == '\t')
        src++;
    return *src == '\0';
}

static void
set_error(const char *msg)
{
    snprintf(g_edit.err, sizeof(g_edit.err), "%s", msg);
}

/* Date, weekday, day and month fields all from one day */
static void
fill_when(Day base)
{
    int y, m, d;
    day_to_ymd(base, &y, &m, &d);
    snprintf(g_edit.date, sizeof(g_edit.date), "%02d.%02d.%04d", d, m, y);
    snprintf(g_edit.day, sizeof(g_edit.day), "%d", d);
    snprintf(g_edit.month, sizeof(g_edit.month), "%d", m);
    g_edit.weekday = day_weekday(base) - 1;
}

int
edit_new(sqlite3 *db, int todo, Day date)
{
    memset(&g_edit, 0, sizeof(g_edit));
    g_edit.is_new  = 1;
    g_edit.kind    = todo ? KIND_TODO : KIND_ENTRY;
    g_edit.type    = g_edit.kind;
    g_edit.db      = db;
    g_edit.id      = -1;
    g_edit.todo_id = -1;
    g_edit.prio    = 1;                         /* normal */
    g_edit.saved_date = date;                   /* kept for recurring entries */
    fill_when(date);
    g_edit.open = 1;
    g_focus     = 0;
    return 0;
}

int
edit_schedule_todo(sqlite3 *db, sqlite3_int64 todo_id, Day date)
{
    Todo t;

    if (store_get_todo(db, todo_id, &t) != 0) return -1;
    edit_new(db, 0, date);
    g_edit.scheduling = 1;
    g_edit.todo_id    = todo_id;
    flatten(g_edit.title, sizeof(g_edit.title), t.title);
    set_notes(t.description);
    store_todo_free(&t);
    return 0;
}

void
edit_saved_item(int *is_todo, sqlite3_int64 *id, Day *date)
{
    *is_todo = g_edit.saved_todo;
    *id      = g_edit.saved_id;
    *date    = g_edit.saved_date;
}

int
edit_open_entry(sqlite3 *db, sqlite3_int64 id)
{
    Entry e;

    if (store_get_entry(db, id, &e) != 0) return -1;
    memset(&g_edit, 0, sizeof(g_edit));
    g_edit.kind    = KIND_ENTRY;
    g_edit.db      = db;
    g_edit.id      = id;
    g_edit.todo_id = e.todo_id;
    flatten(g_edit.title, sizeof(g_edit.title), e.title);
    set_notes(e.description);
    snprintf(g_edit.time, sizeof(g_edit.time), "%s", e.time);
    if (e.duration_min > 0)
        snprintf(g_edit.duration, sizeof(g_edit.duration), "%d", e.duration_min);
    g_edit.repeat = e.recurrence;

    /* Fill every "when" field, so switching Repeat starts from sensible values */
    fill_when(e.recurrence == REC_NONE ? e.date : day_today());
    if (e.recurrence == REC_WEEKLY && e.weekday >= 1 && e.weekday <= 7)
        g_edit.weekday = e.weekday - 1;
    if (e.recurrence == REC_MONTHLY || e.recurrence == REC_YEARLY)
        snprintf(g_edit.day, sizeof(g_edit.day), "%d", e.day);
    if (e.recurrence == REC_YEARLY)
        snprintf(g_edit.month, sizeof(g_edit.month), "%d", e.month);

    store_entry_free(&e);
    g_edit.open = 1;
    g_focus     = 0;
    return 0;
}

int
edit_open_todo(sqlite3 *db, sqlite3_int64 id)
{
    Todo t;

    if (store_get_todo(db, id, &t) != 0) return -1;
    memset(&g_edit, 0, sizeof(g_edit));
    g_edit.kind = KIND_TODO;
    g_edit.db   = db;
    g_edit.id   = id;
    flatten(g_edit.title, sizeof(g_edit.title), t.title);
    set_notes(t.description);
    g_edit.prio   = (t.priority >= 1 && t.priority <= 3) ? t.priority - 1 : 1;
    g_edit.status = t.done ? 1 : 0;
    store_todo_free(&t);
    g_edit.open = 1;
    g_focus     = 0;
    return 0;
}

int
edit_is_open(void)
{
    return g_edit.open;
}

/* ------------------------------------------------------------------ */
/* Saving                                                              */
/* ------------------------------------------------------------------ */

static int
save_entry(void)
{
    Entry e;
    char  err[100];
    long  n;

    memset(&e, 0, sizeof(e));
    e.id          = g_edit.id;
    e.title       = g_edit.title;
    e.description = g_edit.desc;
    e.recurrence  = g_edit.repeat;
    e.todo_id     = g_edit.todo_id;

    if (g_edit.time[0]) {
        if (quickadd_parse_time(g_edit.time, e.time) != 1) {
            set_error("Time: use 15:00 or 3pm, or leave it empty");
            return -1;
        }
    }

    switch (e.recurrence) {
    case REC_NONE: {
        char t[6];
        if (!quickadd_parse_when(g_edit.date, day_today(), &e.date, t, err, sizeof(err))) {
            snprintf(g_edit.err, sizeof(g_edit.err), "Date: %s", err);
            return -1;
        }
        if (t[0] && !e.time[0])                 /* "jutro 15:00" in the date field */
            memcpy(e.time, t, sizeof(e.time));
        break;
    }
    case REC_WEEKLY:
        e.weekday = g_edit.weekday + 1;
        break;
    case REC_MONTHLY:
        e.day = atoi(g_edit.day);
        if (e.day < 1 || e.day > 31) {
            set_error("Day must be 1-31");
            return -1;
        }
        break;
    case REC_YEARLY:
        e.day   = atoi(g_edit.day);
        e.month = atoi(g_edit.month);
        if (e.month < 1 || e.month > 12) {
            set_error("Month must be 1-12");
            return -1;
        }
        if (e.day < 1 || e.day > days_in_month(2000, e.month)) {   /* 2000: 29.02 ok */
            set_error("No such day in that month");
            return -1;
        }
        break;
    }

    n = strtol(g_edit.duration, NULL, 10);
    if (n < 0 || n > 7 * 24 * 60) {
        set_error("Duration: up to 10080 minutes");
        return -1;
    }
    e.duration_min = (int)n;

    if (g_edit.is_new) {
        e.id = store_add_entry(g_edit.db, &e);
        if (e.id < 0) {
            snprintf(g_edit.err, sizeof(g_edit.err), "Save failed: %s",
                     sqlite3_errmsg(g_edit.db));
            return -1;
        }
    } else if (store_update_entry(g_edit.db, &e) != 0) {
        snprintf(g_edit.err, sizeof(g_edit.err), "Save failed: %s",
                 sqlite3_errmsg(g_edit.db));
        return -1;
    }
    g_edit.saved_todo = 0;
    g_edit.saved_id   = e.id;
    if (e.recurrence == REC_NONE)
        g_edit.saved_date = e.date;
    return 0;
}

static int
save_todo(void)
{
    Todo t;

    t.id          = g_edit.id;
    t.title       = g_edit.title;
    t.description = g_edit.desc;
    t.priority    = g_edit.prio + 1;
    t.done        = g_edit.status;
    if (g_edit.is_new) {
        t.id = store_add_todo(g_edit.db, t.title, t.description, t.priority);
        if (t.id < 0) {
            snprintf(g_edit.err, sizeof(g_edit.err), "Save failed: %s",
                     sqlite3_errmsg(g_edit.db));
            return -1;
        }
    } else if (store_update_todo(g_edit.db, &t) != 0) {
        snprintf(g_edit.err, sizeof(g_edit.err), "Save failed: %s",
                 sqlite3_errmsg(g_edit.db));
        return -1;
    }
    g_edit.saved_todo = 1;
    g_edit.saved_id   = t.id;
    return 0;
}

static int
try_save(void)
{
    const char *p = g_edit.title;

    while (*p == ' ') p++;
    if (!*p) {
        set_error("Title is empty");
        return EDIT_HANDLED;
    }
    if (g_edit.is_new)
        g_edit.kind = g_edit.type;
    if ((g_edit.kind == KIND_ENTRY ? save_entry() : save_todo()) != 0)
        return EDIT_HANDLED;
    g_edit.open = 0;
    return EDIT_SAVED;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Notes in an external editor                                         */
/* ------------------------------------------------------------------ */

/* $VISUAL or $EDITOR (may have arguments), or NULL for nvim / vi */
static const char *
editor_command(void)
{
    const char *ed = getenv("VISUAL");
    if (!ed || !ed[0]) ed = getenv("EDITOR");
    return ed && ed[0] ? ed : NULL;
}

/* Returns the editor's exit status, -1 when it could not be started */
static int
run_editor(const char *path)
{
    const char *ed = editor_command();
    pid_t pid;
    int   status;

    if ((pid = fork()) < 0)
        return -1;
    if (pid == 0) {
        if (ed) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s \"$1\"", ed);
            execl("/bin/sh", "sh", "-c", cmd, "sh", path, (char *)NULL);
        } else {
            execlp("nvim", "nvim", path, (char *)NULL);
            execlp("vi", "vi", path, (char *)NULL);
        }
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Write the notes to a temporary file, let the editor change it and
 * read it back.  Curses is suspended meanwhile. */
static void
edit_notes(void)
{
    const char *tmp = getenv("TMPDIR");
    char  path[512], *text;
    FILE *f;
    long  size;
    int   fd, rc;

    snprintf(path, sizeof(path), "%s/7aorganizer-XXXXXX",
             tmp && tmp[0] ? tmp : "/tmp");
    if ((fd = mkstemp(path)) < 0) {
        set_error("Cannot create a temporary file for the editor");
        return;
    }
    if ((f = fdopen(fd, "w")) == NULL) {
        close(fd);
        unlink(path);
        set_error("Cannot write the temporary file");
        return;
    }
    fprintf(f, g_edit.desc[0] ? "%s\n" : "%s", g_edit.desc);
    if (fclose(f) != 0) {
        unlink(path);
        set_error("Cannot write the temporary file");
        return;
    }

    def_prog_mode();
    endwin();
    rc = run_editor(path);
    reset_prog_mode();
    clearok(curscr, TRUE);
    refresh();

    if (rc != 0) {
        unlink(path);
        set_error(rc == 127 ? "No editor found: set $EDITOR"
                            : "The editor failed; notes not changed");
        return;
    }
    f = fopen(path, "r");
    unlink(path);
    if (!f) {
        set_error("Cannot read the notes back");
        return;
    }
    /* read a bit more than fits, so set_notes() sees the text is cut */
    text = malloc(NOTES_LEN + 4);
    size = text ? (long)fread(text, 1, NOTES_LEN + 3, f) : -1;
    fclose(f);
    if (size < 0) {
        set_error("Out of memory");
        return;
    }
    text[size] = '\0';
    if (!set_notes(text))
        set_error("Notes are too long and were cut");
    free(text);
}

static void cb_save(void *arg)   { (void)arg; g_edit.action = ACT_SAVE; }
static void cb_cancel(void *arg) { (void)arg; g_edit.action = ACT_CANCEL; }
static void cb_notes(void *arg)  { (void)arg; g_edit.action = ACT_NOTES; }

/* Notes with several lines can only be edited in the editor: the field
 * shows the first line and how many more there are */
static void
draw_notes_field(int row, int col, int width)
{
    char   first[256], more[32];
    const char *nl = strchr(g_edit.desc, '\n');
    int    idx, focused, lines = 1, mw;
    size_t len = (size_t)(nl - g_edit.desc);

    for (; nl; nl = strchr(nl + 1, '\n'))
        lines++;
    snprintf(first, sizeof(first), "%.*s", (int)len, g_edit.desc);
    snprintf(more, sizeof(more), " (+%d lines)", lines - 1);
    mw = utf8_width(more);
    if (mw > width) mw = width;

    idx     = field_reg(FT_BUTTON, NULL, 0, 0, cb_notes, NULL, row, col, width);
    focused = (idx >= 0 && idx == g_focus);
    if (focused)
        attron(COLOR_PAIR(CP_INPUT) | A_BOLD | (g_basic_colors ? A_REVERSE : 0));
    else
        attron(COLOR_PAIR(CP_INPUT));
    tui_put_text(row, col, width - mw, first);
    attron(A_DIM);
    tui_put_text(row, col + width - mw, mw, more);
    attroff(COLOR_PAIR(CP_INPUT) | A_BOLD | A_REVERSE | A_DIM);
}

static void
label(int row, int col, const char *text)
{
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, col, "%s", text);
    attroff(COLOR_PAIR(CP_BOX));
}

static void
hint(int row, int col, int cols, const char *text)
{
    if (cols < 4) return;
    attron(COLOR_PAIR(CP_BOX) | A_DIM);
    tui_put_text(row, col, cols, text);
    attroff(COLOR_PAIR(CP_BOX) | A_DIM);
}

void
edit_draw(void)
{
    int rows = getmaxy(stdscr);
    int cols = getmaxx(stdscr);
    int w    = cols - 4 < 72 ? cols - 4 : 72;
    int h;
    int top, left, lc, fc, fw, r, i;

    fields_reset();
    if (g_edit.is_new)
        g_edit.kind = g_edit.type;
    /* borders, spacing, buttons and message: 7 rows; plus the fields */
    h = 7 + (g_edit.kind == KIND_ENTRY ? 5 : 4) +
        (g_edit.is_new && !g_edit.scheduling && g_edit.kind == KIND_ENTRY);
    if (!g_edit.open) return;
    if (w < 40 || rows < h + 2) {
        set_error("Terminal too small for the form");
        return;
    }
    top  = (rows - h) / 2;
    left = (cols - w) / 2;
    lc   = left + 2;             /* labels */
    fc   = left + 13;            /* fields */
    fw   = w - 15;

    attron(COLOR_PAIR(CP_BOX));
    for (i = 1; i < h - 1; i++)
        mvhline(top + i, left + 1, ' ', w - 2);
    attroff(COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX_LINE));
    draw_popup_frame(top, left, h, w);
    attron(A_BOLD);
    mvprintw(top, left + 2, " %s ",
             g_edit.scheduling ? "Schedule todo"
             : g_edit.is_new ? (g_edit.kind == KIND_TODO ? "New todo" : "New calendar entry")
             : g_edit.kind == KIND_TODO ? "Edit todo"
             : g_edit.repeat == REC_NONE ? "Edit entry"
             : "Edit entry (all repeats)");
    attroff(COLOR_PAIR(CP_BOX_LINE) | A_BOLD);

    r = top + 2;
    label(r, lc, "Title:");
    draw_textfield(r, fc, fw, g_edit.title, sizeof(g_edit.title), FT_TEXT, 0);
    r++;

    if (g_edit.is_new && !g_edit.scheduling) {
        label(r, lc, "Type:");
        draw_dropdown(r, fc, 18, type_opts, &g_edit.type,
                      g_edit.type_buf, sizeof(g_edit.type_buf), NULL);
        r++;
    }

    if (g_edit.kind == KIND_ENTRY) {
        label(r, lc, "Repeat:");
        draw_dropdown(r, fc, 12, repeat_opts, &g_edit.repeat,
                      g_edit.repeat_buf, sizeof(g_edit.repeat_buf), NULL);
        r++;

        switch (g_edit.repeat) {
        case REC_NONE:
            label(r, lc, "Date:");
            draw_textfield(r, fc, 14, g_edit.date, sizeof(g_edit.date), FT_TEXT, 0);
            hint(r, fc + 16, fw - 16, "30.09, tomorrow, fri, +3d");
            break;
        case REC_DAILY:
            hint(r, fc, fw, "every day");
            break;
        case REC_WEEKLY:
            label(r, lc, "Weekday:");
            draw_dropdown(r, fc, 14, weekday_opts, &g_edit.weekday,
                          g_edit.weekday_buf, sizeof(g_edit.weekday_buf), NULL);
            break;
        case REC_MONTHLY:
            label(r, lc, "Day:");
            draw_textfield(r, fc, 4, g_edit.day, sizeof(g_edit.day), FT_DIGITS, 0);
            hint(r, fc + 6, fw - 6, "of every month (skipped when missing)");
            break;
        case REC_YEARLY:
            label(r, lc, "Day:");
            draw_textfield(r, fc, 4, g_edit.day, sizeof(g_edit.day), FT_DIGITS, 0);
            label(r, fc + 6, "Month:");
            draw_textfield(r, fc + 13, 4, g_edit.month, sizeof(g_edit.month), FT_DIGITS, 0);
            break;
        }
        r++;

        label(r, lc, "Time:");
        draw_textfield(r, fc, 7, g_edit.time, sizeof(g_edit.time), FT_TEXT, 0);
        label(r, fc + 9, "Duration:");
        draw_textfield(r, fc + 19, 6, g_edit.duration, sizeof(g_edit.duration), FT_DIGITS, 0);
        label(r, fc + 26, "min");
        r++;
    } else {
        label(r, lc, "Priority:");
        draw_dropdown(r, fc, 10, prio_opts, &g_edit.prio,
                      g_edit.prio_buf, sizeof(g_edit.prio_buf), NULL);
        r++;
        if (!g_edit.is_new) {
            label(r, lc, "Status:");
            draw_dropdown(r, fc, 10, status_opts, &g_edit.status,
                          g_edit.status_buf, sizeof(g_edit.status_buf), NULL);
            r++;
        }
    }

    label(r, lc, "Notes:");
    if (strchr(g_edit.desc, '\n'))
        draw_notes_field(r, fc, fw);
    else
        draw_textfield(r, fc, fw, g_edit.desc, sizeof(g_edit.desc), FT_TEXT, 0);
    r++;
    {
        char h[80];
        snprintf(h, sizeof(h), "Ctrl+E: notes in %s", editor_command() ? editor_command() : "nvim");
        hint(r, fc, fw, h);
    }
    r++;

    draw_button(r, left + w / 2 - 10, "Save", cb_save, NULL);
    draw_button(r, left + w / 2 + 2, "Cancel", cb_cancel, NULL);
    r++;

    if (g_edit.err[0]) {
        attron(COLOR_PAIR(CP_BOX) | A_BOLD);
        tui_put_text(r, lc, w - 4, g_edit.err);
        attroff(COLOR_PAIR(CP_BOX) | A_BOLD);
    } else {
        hint(r, lc, w - 4, "Tab / arrows move   Enter or Ctrl+S saves   Esc cancels");
    }

    if (g_focus >= g_nfields) g_focus = g_nfields - 1;
    draw_dropdown_popup();
    form_place_cursor();
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

int
edit_key(int ch)
{
    int ft;

    if (!g_edit.open) return EDIT_IGNORED;
    if (form_popup_key(ch)) return EDIT_HANDLED;

    g_edit.err[0] = '\0';
    if (ch == 27) {                                     /* Esc */
        g_edit.open = 0;
        return EDIT_CANCELLED;
    }
    if (ch == ('s' & 0x1f))                             /* Ctrl+S */
        return try_save();
    if (ch == ('e' & 0x1f)) {                           /* Ctrl+E */
        edit_notes();
        return EDIT_HANDLED;
    }

    ft = (g_focus >= 0 && g_focus < g_nfields) ? g_fields[g_focus].type : -1;
    if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && (ft == FT_TEXT || ft == FT_DIGITS))
        return try_save();

    g_edit.action = ACT_NONE;
    form_field_key(ch);
    if (g_edit.action == ACT_SAVE)
        return try_save();
    if (g_edit.action == ACT_NOTES) {
        edit_notes();
        return EDIT_HANDLED;
    }
    if (g_edit.action == ACT_CANCEL) {
        g_edit.open = 0;
        return EDIT_CANCELLED;
    }
    return EDIT_HANDLED;
}
