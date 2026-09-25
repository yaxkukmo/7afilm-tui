/*
 * 7afilm-tui.c - terminal UI for 7afilm (curses)
 *
 * Same database (~/.7a/film.db) as the desktop version.
 *
 * Tab bar (top):  [ Timer ]  [ Database ]  [ Calc ]  [ Config ]
 *   Left/Right or 1/2/3/4 to switch tabs
 *
 * Navigation within a tab:
 *   Tab / Shift+Tab  - next / previous field
 *   Up / Down        - spinner +/-1 (HH:MM:SS) or list navigation
 *   Enter / Space    - activate button or load selected preset
 *   d                - delete selected item (list focused)
 *   q / Q            - quit
 */

#define _DEFAULT_SOURCE

#include <curses.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <sqlite3.h>

#include "dynlist.h"
#include "timer.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define PRESET_NAME_LEN    64
#define VISIBLE_PRESETS     4
#define VISIBLE_CFG_LIST    6
#define MAX_FIELDS        160

#define FT_TEXT    0   /* free text                                    */
#define FT_DIGITS  1   /* digits only                                  */
#define FT_SPINNER 2   /* digits + up/down arrows (HH/MM/SS)          */
#define FT_BUTTON  3   /* Enter/Space = activate                       */
#define FT_LIST    4   /* list (presets or config)                     */
#define FT_TABS    5   /* tab bar - Left/Right switches tab            */
#define FT_DROPDOWN 6  /* dropdown - opens popup                       */
#define MAX_DROPDOWNS 8
#define MAX_SEARCH_MATCHES 512

#define CP_BUTTON   1  /* white on black - button background          */
#define CP_BOX      2  /* box interior fill: fg=default, bg=#033535   */
#define CP_BG       3  /* app background: fg=default, bg=#404040      */
#define CP_BOX_LINE   4  /* box border chars: fg=amber, bg=transparent   */
#define CP_BOX_BORDER 5  /* block border: fg=#033535, bg=#404040         */
#define CP_INPUT      6  /* input field: fg=amber, bg=#0d4848          */
#define CP_PROGRESS   7  /* progress bar: fg=black, bg=green           */
#define CP_ALARM       8  /* alarm tick: fg=black, bg=red               */
#define COLOR_BOX_BG   8  /* custom color #033535                      */
#define COLOR_APP_BG   9  /* custom color #404040                      */
#define COLOR_BORDER  10  /* custom color amber safelight ~#E68000     */
#define COLOR_INPUT_BG 11 /* custom color #0d4848 - lighter teal       */
#define COLOR_PROG_GREEN 12 /* custom green for progress bar           */

#define LABEL_W    14  /* label column width ("Temperature:" is widest) */
#define INDENT      2  /* section indent                               */

#define TAB_TIMER     0
#define TAB_DATABASE  1
#define TAB_CALC      2
#define TAB_CONFIG    3
#define TAB_HELP      4
#define TAB_QUIT      5
#define TAB_COUNT     6

/* ------------------------------------------------------------------ */
/* Seed option arrays (used to populate user_lists on first run)      */
/* ------------------------------------------------------------------ */

static const char *film_options[] = {
    "Kodak Tri-X 400", "Kodak T-Max 100", "Kodak T-Max 400",
    "Ilford HP5 Plus", "Ilford FP4 Plus", "Ilford Delta 100",
    "Ilford Delta 400", "Ilford Delta 3200", "Fomapan 100",
    "Fomapan 200", "Fomapan 400", NULL
};
static const char *iso_options[] = {
    "25", "50", "100", "125", "160", "200", "250", "320",
    "400", "500", "640", "800", "1000", "1250", "1600",
    "2000", "2500", "3200", NULL
};
static const char *dev_options[] = {
    "Kodak D-76", "Kodak HC-110", "Kodak XTOL",
    "Ilford ID-11", "Ilford Ilfosol 3", "Ilford Microphen",
    "Rodinal / R09", "Caffenol-C", NULL
};
static const char *dil_options[] = {
    "Stock", "1:1", "1:2", "1:3", "1:4", "1:9",
    "1:25", "1:31", "1:50", "1:100", NULL
};

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    sqlite3_int64 id;
    char name[PRESET_NAME_LEN];
} PresetRow;

typedef struct {
    int    type;
    char  *buf;
    size_t bufsz;
    int    maxval;
    void (*action)(void *);
    void  *arg;
    int    row, col, width;
} Field;

typedef struct {
    const char **options;
    int         *index;
    char        *buf;
    size_t       bufsz;
    void       (*on_confirm)(int); /* called with new index on confirm; may be NULL */
} DropdownMeta;

/* ------------------------------------------------------------------ */
/* Box-drawing characters (ACS or ASCII fallback)                     */
/* ------------------------------------------------------------------ */

static chtype g_ul, g_ur, g_ll, g_lr; /* corners                     */
static chtype g_hl, g_vl;             /* horizontal / vertical line  */
static chtype g_lt, g_rt, g_tt;       /* T-junctions                 */

static void
init_box_chars(void)
{
    /* The OpenBSD wscons console (TERM=vt220 / wsvt25) supports DEC
     * Special Graphics, so ACS works there too.  ncurses falls back to
     * ASCII by itself when the terminal has no acsc capability.  The
     * NO_ACS env var still forces plain ASCII.                        */
    if (getenv("NO_ACS") != NULL) {
        g_ul = g_ur = g_ll = g_lr = g_lt = g_rt = g_tt = '+';
        g_hl = '-';
        g_vl = '|';
    } else {
        g_ul = ACS_ULCORNER; g_ur = ACS_URCORNER;
        g_ll = ACS_LLCORNER; g_lr = ACS_LRCORNER;
        g_hl = ACS_HLINE;    g_vl = ACS_VLINE;
        g_lt = ACS_LTEE;     g_rt = ACS_RTEE;
        g_tt = ACS_TTEE;
    }
}

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static sqlite3   *g_db;
static PresetRow *g_presets      = NULL;
static int        g_preset_count  = 0;
static int        g_preset_cap    = 0;
static int        g_preset_scroll = 0;
static int        g_preset_sel    = 0;
static char       g_preset_name[PRESET_NAME_LEN] = "";
static sqlite3_int64 g_preset_loaded_id = -1;
char              g_status[128]    = "";
static char       g_temp_buf[8]   = "20";  /* runtime temperature — not stored in DB */

static Field      g_fields[MAX_FIELDS];
static int        g_nfields = 0;
static int        g_focus   = 0;

static int        g_tab     = TAB_TIMER;
static int        g_want_quit = 0;

/* Tab button positions — used to draw the gap in the top box bottom border */
static int        g_tab_btn_col[TAB_COUNT];
static int        g_tab_btn_width[TAB_COUNT];

static int g_film_idx     = 0;
static int g_iso_nom_idx  = 8;   /* 400 */
static int g_iso_used_idx = 8;   /* 400 */
static int g_dev_idx      = 0;
static DropdownMeta g_dd_store[MAX_DROPDOWNS];
static int          g_dd_count = 0;

/* Edit popup (config list item rename) state */
static int      g_editpopup_open = 0;
static DynList *g_editpopup_dl   = NULL;
static int      g_editpopup_idx  = 0;
static char     g_editpopup_buf[MAX_OPTION_LEN]  = "";
static char     g_editpopup_orig[MAX_OPTION_LEN] = "";

/* Popup (open dropdown) state */
static int           g_popup_open   = 0;
static DropdownMeta *g_popup_dm     = NULL;
static int           g_popup_frow   = 0;
static int           g_popup_fcol   = 0;
static int           g_popup_fwidth = 0;
static int           g_popup_sel    = 0;
static int           g_popup_scroll = 0;

/* Search preset popup state */
static int  g_searchpopup_open              = 0;
static char g_searchpopup_buf[PRESET_NAME_LEN] = "";
static int  g_searchpopup_sel               = 0;
static int  g_searchpopup_scroll            = 0;
static int  g_searchpopup_matches[MAX_SEARCH_MATCHES];
static int  g_searchpopup_nmatches          = 0;
static int  g_searchpopup_show_new          = 0;

/* Dynamic lists */
static DynList g_films;
static DynList g_devs;
static DynList g_isos;
static DynList g_dils;
static int     g_dil_idx = 0;

/* Config tab defaults */
static char cfg_def_dev_hh[8]  = "00";
static char cfg_def_dev_mm[8]  = "09";
static char cfg_def_dev_ss[8]  = "30";
static char cfg_def_stop_hh[8] = "00";
static char cfg_def_stop_mm[8] = "01";
static char cfg_def_stop_ss[8] = "00";
static char cfg_def_fix_hh[8]  = "00";
static char cfg_def_fix_mm[8]  = "05";
static char cfg_def_fix_ss[8]  = "00";
static char cfg_def_temp[8]    = "20";

/* Calc tab state */
static char calc_mm_buf[8]        = "09";
static char calc_ss_buf[8]        = "30";
static char calc_base_temp[8]     = "20";
static char calc_actual_temp[8]   = "20";
static char calc_dilution[24]     = "1:100";
static char calc_volume[8]        = "500";

static volatile sig_atomic_t g_resize = 0;
static int        g_content_scroll[TAB_COUNT]; /* per-tab scroll offset (zero-init) */

/* Workflow phase for timer tab:
 * 0=ready(dev)  1=dev running  2=ready(stop)  3=stop running
 * 4=ready(fix)  5=fix running  6=all done
 */
int g_workflow_phase = 0;

/* Forward declarations */
static void sync_dropdown_indices(void);
static const char *ColStr(sqlite3_stmt *s, int c);
static void BuildPresetName(char *out, size_t outsz, const char *film,
                            const char *iso_nom, const char *iso_used,
                            const char *developer, const char *dilution);

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

static int
TableExists(const char *name)
{
    sqlite3_stmt *stmt;
    int exists = 0;
    if (sqlite3_prepare_v2(g_db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
    }
    return exists;
}

static int
ColumnExists(const char *table, const char *col)
{
    sqlite3_stmt *stmt;
    char sql[256];
    int exists = 0;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *cn = (const char *)sqlite3_column_text(stmt, 1);
            if (cn && strcmp(cn, col) == 0) { exists = 1; break; }
        }
        sqlite3_finalize(stmt);
    }
    return exists;
}

static void
MigrateFromOldSchema(void)
{
    sqlite3_exec(g_db,
        "INSERT OR IGNORE INTO options(category, value)"
        " SELECT DISTINCT 'dilution', dev_dilution FROM presets"
        " WHERE dev_dilution != '' AND dev_dilution IS NOT NULL;",
        NULL, NULL, NULL);

    sqlite3_exec(g_db,
        "CREATE TABLE presets_new ("
        " id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name         TEXT NOT NULL COLLATE NOCASE UNIQUE,"
        " film_id      INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " iso_id       INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " iso_used_id  INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " developer_id INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " dilution_id  INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " dev_time     INTEGER NOT NULL DEFAULT 0,"
        " dev_every    INTEGER NOT NULL DEFAULT 0,"
        " dev_for      INTEGER NOT NULL DEFAULT 10,"
        " stop_time    INTEGER NOT NULL DEFAULT 0,"
        " fix_time     INTEGER NOT NULL DEFAULT 0,"
        " fix_every    INTEGER NOT NULL DEFAULT 0,"
        " fix_for      INTEGER NOT NULL DEFAULT 10"
        ");", NULL, NULL, NULL);

    sqlite3_exec(g_db,
        "INSERT OR IGNORE INTO presets_new"
        " (name, film_id, iso_id, iso_used_id, developer_id, dilution_id,"
        "  dev_time, dev_every, dev_for, stop_time, fix_time, fix_every, fix_for)"
        " SELECT p.name,"
        "  (SELECT id FROM options WHERE category='film' AND value=p.dev_film),"
        "  (SELECT id FROM options WHERE category='iso' AND value=p.dev_iso),"
        "  (SELECT id FROM options WHERE category='iso' AND value=p.dev_iso_used),"
        "  (SELECT id FROM options WHERE category='developer' AND value=p.dev_developer),"
        "  (SELECT id FROM options WHERE category='dilution' AND value=p.dev_dilution),"
        "  CAST(COALESCE(p.dev_hh,'0') AS INTEGER)*3600"
        "   + CAST(COALESCE(p.dev_mm,'0') AS INTEGER)*60"
        "   + CAST(COALESCE(p.dev_ss,'0') AS INTEGER),"
        "  CAST(COALESCE(p.dev_every,'0') AS INTEGER),"
        "  CAST(COALESCE(p.dev_for,'10') AS INTEGER),"
        "  CAST(COALESCE(p.stop_hh,'0') AS INTEGER)*3600"
        "   + CAST(COALESCE(p.stop_mm,'0') AS INTEGER)*60"
        "   + CAST(COALESCE(p.stop_ss,'0') AS INTEGER),"
        "  CAST(COALESCE(p.fix_hh,'0') AS INTEGER)*3600"
        "   + CAST(COALESCE(p.fix_mm,'0') AS INTEGER)*60"
        "   + CAST(COALESCE(p.fix_ss,'0') AS INTEGER),"
        "  CAST(COALESCE(p.fix_every,'0') AS INTEGER),"
        "  CAST(COALESCE(p.fix_for,'10') AS INTEGER)"
        " FROM presets p;", NULL, NULL, NULL);

    sqlite3_exec(g_db, "DROP TABLE presets;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "ALTER TABLE presets_new RENAME TO presets;", NULL, NULL, NULL);
}

static sqlite3_int64
UpsertOption(const char *category, const char *value)
{
    sqlite3_stmt *stmt;
    sqlite3_int64 id = -1;

    if (!value || !value[0]) return -1;

    if (sqlite3_prepare_v2(g_db,
            "INSERT OR IGNORE INTO options(category, value) VALUES(?1, ?2);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, category, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(g_db,
            "SELECT id FROM options WHERE category=?1 AND value=?2;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, category, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return id;
}

static void
BindOptId(sqlite3_stmt *stmt, int param, sqlite3_int64 id)
{
    if (id < 0) sqlite3_bind_null(stmt, param);
    else        sqlite3_bind_int64(stmt, param, id);
}

static void
OpenDatabase(void)
{
    const char *home = getenv("HOME");
    char app_dir[1024], db_path[1040];

    snprintf(app_dir, sizeof(app_dir), "%s/.7a", home ? home : ".");
    mkdir(app_dir, 0700);
    snprintf(db_path, sizeof(db_path), "%s/film.db", app_dir);

    if (sqlite3_open(db_path, &g_db) != SQLITE_OK) {
        endwin();
        fprintf(stderr, "7afilm-tui: cannot open %s\n", db_path);
        exit(1);
    }
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",  NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;",   NULL, NULL, NULL);

    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS options ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " category TEXT NOT NULL,"
        " value TEXT NOT NULL,"
        " UNIQUE(category, value)"
        ");", NULL, NULL, NULL);

    if (TableExists("user_lists")) {
        sqlite3_exec(g_db,
            "INSERT OR IGNORE INTO options(category, value)"
            " SELECT category, item FROM user_lists;",
            NULL, NULL, NULL);
        sqlite3_exec(g_db, "DROP TABLE user_lists;", NULL, NULL, NULL);
    }

    if (ColumnExists("presets", "dev_hh"))
        MigrateFromOldSchema();

    sqlite3_exec(g_db,
        "CREATE TABLE IF NOT EXISTS presets ("
        " id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name         TEXT NOT NULL COLLATE NOCASE UNIQUE,"
        " film_id      INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " iso_id       INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " iso_used_id  INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " developer_id INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " dilution_id  INTEGER REFERENCES options(id) ON DELETE SET NULL,"
        " dev_time     INTEGER NOT NULL DEFAULT 0,"
        " dev_every    INTEGER NOT NULL DEFAULT 0,"
        " dev_for      INTEGER NOT NULL DEFAULT 10,"
        " stop_time    INTEGER NOT NULL DEFAULT 0,"
        " fix_time     INTEGER NOT NULL DEFAULT 0,"
        " fix_every    INTEGER NOT NULL DEFAULT 0,"
        " fix_for      INTEGER NOT NULL DEFAULT 10"
        ");", NULL, NULL, NULL);
}

static void
SaveOption(const char *category, const char *item)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db,
            "INSERT OR IGNORE INTO options (category, value) VALUES (?1, ?2);",
            -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, item,     -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void
RenameOption(const char *category, const char *old_val, const char *new_val)
{
    sqlite3_stmt *stmt;
    /* FK references handle preset cascade via ON DELETE SET NULL;
       just rename the option row itself. */
    if (sqlite3_prepare_v2(g_db,
            "UPDATE options SET value=?1 WHERE category=?2 AND value=?3;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, new_val,  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, category, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, old_val,  -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    (void)category; /* used above */
}

static void
DeleteOption(const char *category, const char *item)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db,
            "DELETE FROM options WHERE category=?1 AND value=?2;",
            -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, item,     -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void
SeedOptionsIfEmpty(void)
{
    sqlite3_stmt *stmt;
    int count = 0;
    int i;

    if (sqlite3_prepare_v2(g_db,
            "SELECT COUNT(*) FROM options;",
            -1, &stmt, NULL) != SQLITE_OK) return;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (count > 0) return;

    for (i = 0; film_options[i]; i++)
        SaveOption("film", film_options[i]);
    for (i = 0; iso_options[i]; i++)
        SaveOption("iso", iso_options[i]);
    for (i = 0; dev_options[i]; i++)
        SaveOption("developer", dev_options[i]);
    for (i = 0; dil_options[i]; i++)
        SaveOption("dilution", dil_options[i]);
}

static void
LoadOptions(void)
{
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db,
            "SELECT category, value FROM options ORDER BY id;",
            -1, &stmt, NULL) != SQLITE_OK) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cat  = (const char *)sqlite3_column_text(stmt, 0);
        const char *item = (const char *)sqlite3_column_text(stmt, 1);
        if (!cat || !item) continue;
        if (strcmp(cat, "film") == 0)
            dynlist_add(&g_films, item);
        else if (strcmp(cat, "iso") == 0)
            dynlist_add(&g_isos, item);
        else if (strcmp(cat, "developer") == 0)
            dynlist_add(&g_devs, item);
        else if (strcmp(cat, "dilution") == 0)
            dynlist_add(&g_dils, item);
    }
    sqlite3_finalize(stmt);
}


static void
LoadPresetList(void)
{
    sqlite3_stmt *stmt;
    g_preset_count = 0;
    if (sqlite3_prepare_v2(g_db,
            "SELECT id, name FROM presets ORDER BY name COLLATE NOCASE;",
            -1, &stmt, NULL) != SQLITE_OK) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (g_preset_count >= g_preset_cap) {
            int nc = g_preset_cap ? g_preset_cap * 2 : 16;
            PresetRow *tmp = realloc(g_presets, (size_t)nc * sizeof(PresetRow));
            if (!tmp) break;
            g_presets = tmp;
            g_preset_cap = nc;
        }
        g_presets[g_preset_count].id = sqlite3_column_int64(stmt, 0);
        snprintf(g_presets[g_preset_count].name, PRESET_NAME_LEN,
                 "%s", name ? (const char *)name : "");
        g_preset_count++;
    }
    sqlite3_finalize(stmt);
    if (g_preset_sel >= g_preset_count) g_preset_sel = g_preset_count - 1;
    if (g_preset_sel < 0) g_preset_sel = 0;
}

static const char *
ColStr(sqlite3_stmt *s, int c)
{
    const unsigned char *v = sqlite3_column_text(s, c);
    return v ? (const char *)v : "";
}

static void
LoadPresetIntoTimers(sqlite3_int64 id)
{
    sqlite3_stmt *stmt;
    CountdownTimer *dev  = &g_timers[0];
    CountdownTimer *stop = &g_timers[1];
    CountdownTimer *fix  = &g_timers[2];
    int dev_time, stop_time, fix_time;

    if (sqlite3_prepare_v2(g_db,
            "SELECT f.value, i.value, iu.value, d.value, dil.value,"
            "       p.dev_time, p.dev_every, p.dev_for,"
            "       p.stop_time,"
            "       p.fix_time, p.fix_every, p.fix_for"
            " FROM presets p"
            " LEFT JOIN options f   ON f.id   = p.film_id"
            " LEFT JOIN options i   ON i.id   = p.iso_id"
            " LEFT JOIN options iu  ON iu.id  = p.iso_used_id"
            " LEFT JOIN options d   ON d.id   = p.developer_id"
            " LEFT JOIN options dil ON dil.id = p.dilution_id"
            " WHERE p.id=?1;", -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        snprintf(dev->film_buf,      sizeof(dev->film_buf),      "%s", ColStr(stmt, 0));
        snprintf(dev->iso_buf,       sizeof(dev->iso_buf),       "%s", ColStr(stmt, 1));
        snprintf(dev->iso_used_buf,  sizeof(dev->iso_used_buf),  "%s", ColStr(stmt, 2));
        snprintf(dev->dev_name_buf,  sizeof(dev->dev_name_buf),  "%s", ColStr(stmt, 3));
        snprintf(dev->dilution_buf,  sizeof(dev->dilution_buf),  "%s", ColStr(stmt, 4));

        dev_time  = sqlite3_column_int(stmt, 5);
        snprintf(dev->alarm_buf,     sizeof(dev->alarm_buf),     "%d", sqlite3_column_int(stmt, 6));
        snprintf(dev->alarm_dur_buf, sizeof(dev->alarm_dur_buf), "%d", sqlite3_column_int(stmt, 7));
        SetTimerTime(dev, dev_time);

        stop_time = sqlite3_column_int(stmt, 8);
        SetTimerTime(stop, stop_time);

        fix_time  = sqlite3_column_int(stmt, 9);
        snprintf(fix->alarm_buf,     sizeof(fix->alarm_buf),     "%d", sqlite3_column_int(stmt, 10));
        snprintf(fix->alarm_dur_buf, sizeof(fix->alarm_dur_buf), "%d", sqlite3_column_int(stmt, 11));
        SetTimerTime(fix, fix_time);
    }
    sync_dropdown_indices();
    sqlite3_finalize(stmt);
}

static int
hms_to_secs(const char *hh, const char *mm, const char *ss)
{
    return (int)strtol(hh, NULL, 10) * 3600
         + (int)strtol(mm, NULL, 10) * 60
         + (int)strtol(ss, NULL, 10);
}

static void
SavePreset(void)
{
    sqlite3_stmt *stmt;
    CountdownTimer *dev  = &g_timers[0];
    CountdownTimer *stop = &g_timers[1];
    CountdownTimer *fix  = &g_timers[2];
    char name[PRESET_NAME_LEN];
    sqlite3_int64 film_id, iso_id, iso_used_id, dev_id, dil_id;
    int dev_time, stop_time, fix_time;

    if (AnyTimerRunning()) {
        snprintf(g_status, sizeof(g_status), "Stop all timers before saving.");
        return;
    }

    /* Auto-generate preset name from recipe */
    BuildPresetName(name, sizeof(name),
        dev->film_buf, dev->iso_buf, dev->iso_used_buf,
        dev->dev_name_buf, dev->dilution_buf);
    if (name[0] == '\0') {
        snprintf(g_status, sizeof(g_status), "Set film, developer and dilution first.");
        return;
    }

    film_id    = UpsertOption("film",      dev->film_buf);
    iso_id     = UpsertOption("iso",       dev->iso_buf);
    iso_used_id= UpsertOption("iso",       dev->iso_used_buf);
    dev_id     = UpsertOption("developer", dev->dev_name_buf);
    dil_id     = UpsertOption("dilution",  dev->dilution_buf);

    dev_time  = hms_to_secs(dev->hh_buf,  dev->mm_buf,  dev->ss_buf);
    stop_time = hms_to_secs(stop->hh_buf, stop->mm_buf, stop->ss_buf);
    fix_time  = hms_to_secs(fix->hh_buf,  fix->mm_buf,  fix->ss_buf);

    /* Skip save if an identical preset already exists */
    if (sqlite3_prepare_v2(g_db,
            "SELECT 1 FROM presets"
            " WHERE name=?1 COLLATE NOCASE"
            "   AND film_id IS ?2 AND iso_id IS ?3"
            "   AND iso_used_id IS ?4 AND developer_id IS ?5 AND dilution_id IS ?6"
            "   AND dev_time=?7 AND dev_every=?8 AND dev_for=?9"
            "   AND stop_time=?10"
            "   AND fix_time=?11 AND fix_every=?12 AND fix_for=?13",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt,  1, name, -1, SQLITE_TRANSIENT);
        BindOptId(stmt,  2, film_id);
        BindOptId(stmt,  3, iso_id);
        BindOptId(stmt,  4, iso_used_id);
        BindOptId(stmt,  5, dev_id);
        BindOptId(stmt,  6, dil_id);
        sqlite3_bind_int(stmt,  7, dev_time);
        sqlite3_bind_int(stmt,  8, (int)strtol(dev->alarm_buf,     NULL, 10));
        sqlite3_bind_int(stmt,  9, (int)strtol(dev->alarm_dur_buf, NULL, 10));
        sqlite3_bind_int(stmt, 10, stop_time);
        sqlite3_bind_int(stmt, 11, fix_time);
        sqlite3_bind_int(stmt, 12, (int)strtol(fix->alarm_buf,     NULL, 10));
        sqlite3_bind_int(stmt, 13, (int)strtol(fix->alarm_dur_buf, NULL, 10));
        int identical = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        if (identical) {
            snprintf(g_status, sizeof(g_status), "No changes: %s", name);
            return;
        }
    }

    if (sqlite3_prepare_v2(g_db,
            "INSERT OR REPLACE INTO presets"
            " (id, name, film_id, iso_id, iso_used_id, developer_id, dilution_id,"
            "  dev_time, dev_every, dev_for,"
            "  stop_time,"
            "  fix_time, fix_every, fix_for)"
            " VALUES("
            "  (SELECT id FROM presets WHERE name=?1 COLLATE NOCASE),"
            "  ?1,?2,?3,?4,?5,?6,"
            "  ?7,?8,?9,"
            "  ?10,"
            "  ?11,?12,?13);",
            -1, &stmt, NULL) != SQLITE_OK) return;

    sqlite3_bind_text(stmt,  1, name, -1, SQLITE_TRANSIENT);
    BindOptId(stmt,  2, film_id);
    BindOptId(stmt,  3, iso_id);
    BindOptId(stmt,  4, iso_used_id);
    BindOptId(stmt,  5, dev_id);
    BindOptId(stmt,  6, dil_id);
    sqlite3_bind_int(stmt,  7, dev_time);
    sqlite3_bind_int(stmt,  8, (int)strtol(dev->alarm_buf,     NULL, 10));
    sqlite3_bind_int(stmt,  9, (int)strtol(dev->alarm_dur_buf, NULL, 10));
    sqlite3_bind_int(stmt, 10, stop_time);
    sqlite3_bind_int(stmt, 11, fix_time);
    sqlite3_bind_int(stmt, 12, (int)strtol(fix->alarm_buf,     NULL, 10));
    sqlite3_bind_int(stmt, 13, (int)strtol(fix->alarm_dur_buf, NULL, 10));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Keep g_preset_name in sync for display */
    snprintf(g_preset_name, sizeof(g_preset_name), "%s", name);
    LoadPresetList();
    g_preset_scroll = 0;
    snprintf(g_status, sizeof(g_status), "Saved: %s", name);
}

static void
DeleteSelectedPreset(void)
{
    sqlite3_stmt *stmt;
    if (g_preset_sel < 0 || g_preset_sel >= g_preset_count) return;
    if (sqlite3_prepare_v2(g_db,
            "DELETE FROM presets WHERE id=?1;", -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, g_presets[g_preset_sel].id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (g_preset_loaded_id == g_presets[g_preset_sel].id)
        g_preset_loaded_id = -1;
    LoadPresetList();
    snprintf(g_status, sizeof(g_status), "Preset deleted.");
}

/* ------------------------------------------------------------------ */
/* Field registry                                                      */
/* ------------------------------------------------------------------ */

static void fields_reset(void) { g_nfields = 0; g_dd_count = 0; }

static int
field_reg(int type, char *buf, size_t bufsz, int maxval,
          void (*action)(void *), void *arg,
          int row, int col, int width)
{
    Field *f;
    if (g_nfields >= MAX_FIELDS) return -1;
    f         = &g_fields[g_nfields];
    f->type   = type;
    f->buf    = buf;
    f->bufsz  = bufsz;
    f->maxval = maxval;
    f->action = action;
    f->arg    = arg;
    f->row    = row;
    f->col    = col;
    f->width  = width;
    return g_nfields++;
}

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                  */
/* ------------------------------------------------------------------ */


static void
draw_box_bottom(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_ll);
    mvhline(row, col + 1, g_hl, width - 2);
    mvaddch(row, col + width - 1, g_lr);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

static void
draw_box_top_plain(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_ul);
    mvhline(row, col + 1, g_hl, width - 2);
    mvaddch(row, col + width - 1, g_ur);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

static void
draw_h_separator(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_lt);
    mvhline(row, col + 1, g_hl, width - 2);
    mvaddch(row, col + width - 1, g_rt);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

static void
draw_box_sides(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));
    attron(COLOR_PAIR(CP_BOX));
    hline(' ', width - 2);
    attroff(COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col + width - 1, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

/* Popup frame: border drawn with the current attributes, plus a drop
 * shadow (one column right, one row below) recolored in place.       */
static void
draw_popup_frame(int top, int left, int height, int width)
{
    int rows = getmaxy(stdscr);
    int cols = getmaxx(stdscr);
    int r;

    mvaddch(top, left, g_ul);
    mvhline(top, left + 1, g_hl, width - 2);
    mvaddch(top, left + width - 1, g_ur);
    mvvline(top + 1, left, g_vl, height - 2);
    mvvline(top + 1, left + width - 1, g_vl, height - 2);
    mvaddch(top + height - 1, left, g_ll);
    mvhline(top + height - 1, left + 1, g_hl, width - 2);
    mvaddch(top + height - 1, left + width - 1, g_lr);

    if (!has_colors()) return;
    if (left + width < cols)
        for (r = top + 1; r <= top + height && r < rows; r++)
            mvchgat(r, left + width, 1, A_NORMAL, CP_BUTTON, NULL);
    if (top + height < rows && left + 1 < cols)
        mvchgat(top + height, left + 1,
                (left + width < cols ? width : cols - left - 1),
                A_NORMAL, CP_BUTTON, NULL);
}

/* Section title row inside the flat main box */
static void
draw_section_title(int row, int col, int width, const char *title)
{
    draw_box_sides(row, col, width);
    attron(A_BOLD | COLOR_PAIR(CP_BOX));
    mvprintw(row, col + INDENT, "  %s", title);
    attroff(A_BOLD | COLOR_PAIR(CP_BOX));
}

static int
draw_textfield(int row, int col, int width,
               char *buf, size_t bufsz, int type, int maxval)
{
    int idx     = field_reg(type, buf, bufsz, maxval, NULL, NULL, row, col, width);
    int focused = (idx >= 0 && idx == g_focus);
    int len     = (int)strlen(buf);
    int i;

    if (focused)
        attron(COLOR_PAIR(CP_INPUT) | A_BOLD);
    else
        attron(COLOR_PAIR(CP_INPUT));
    move(row, col);
    for (i = 0; i < width; i++)
        addch(i < len ? (unsigned char)buf[i] : ' ');
    if (focused)
        attroff(COLOR_PAIR(CP_INPUT) | A_BOLD);
    else
        attroff(COLOR_PAIR(CP_INPUT));
    return idx;
}

static int
draw_button(int row, int col, const char *label,
            void (*action)(void *), void *arg)
{
    int width   = (int)strlen(label) + 2;
    int idx     = field_reg(FT_BUTTON, NULL, 0, 0, action, arg, row, col, width);
    int focused = (idx >= 0 && idx == g_focus);

    if (focused)
        attron(COLOR_PAIR(CP_BUTTON) | A_REVERSE | A_BOLD);
    else
        attron(COLOR_PAIR(CP_BUTTON));
    mvprintw(row, col, " %s ", label);
    if (focused)
        attroff(COLOR_PAIR(CP_BUTTON) | A_REVERSE | A_BOLD);
    else
        attroff(COLOR_PAIR(CP_BUTTON));
    return idx;
}

static void
draw_label(int row, const char *text)
{
    int tw  = (int)strlen(text);
    int col = INDENT + LABEL_W - tw;
    if (col < INDENT) col = INDENT;
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, col, "%s", text);
    attroff(COLOR_PAIR(CP_BOX));
}

static int field_col(void) { return INDENT + LABEL_W + 1; }

static void
draw_hms_fields(int row, int fc, char *hh, char *mm, char *ss)
{
    draw_textfield(row, fc,      5, hh, 8, FT_SPINNER, 999);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 5, ":");
    attroff(COLOR_PAIR(CP_BOX));
    draw_textfield(row, fc + 6,  4, mm, 8, FT_SPINNER, 59);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 10, ":");
    attroff(COLOR_PAIR(CP_BOX));
    draw_textfield(row, fc + 11, 4, ss, 8, FT_SPINNER, 59);
}

static int
draw_dropdown(int row, int col, int width,
              const char **options, int *index, char *buf, size_t bufsz,
              void (*on_confirm)(int))
{
    DropdownMeta *dm;
    int n, idx, focused, len, i;
    const char *val;

    for (n = 0; options[n]; n++);

    if (g_dd_count >= MAX_DROPDOWNS) return -1;
    dm             = &g_dd_store[g_dd_count++];
    dm->options    = options;
    dm->index      = index;
    dm->buf        = buf;
    dm->bufsz      = bufsz;
    dm->on_confirm = on_confirm;

    idx     = field_reg(FT_DROPDOWN, buf, bufsz, n - 1, NULL, dm, row, col, width);
    focused = (idx >= 0 && idx == g_focus);
    val     = (*index >= 0 && *index < n) ? options[*index] : "";
    len     = (int)strlen(val);

    if (focused)
        attron(COLOR_PAIR(CP_INPUT) | A_BOLD);
    else
        attron(COLOR_PAIR(CP_INPUT));
    move(row, col);
    for (i = 0; i < width - 2; i++)
        addch(i < len ? (unsigned char)val[i] : ' ');
    addch(' ');
    addch(ACS_DARROW);
    if (focused)
        attroff(COLOR_PAIR(CP_INPUT) | A_BOLD);
    else
        attroff(COLOR_PAIR(CP_INPUT));
    return idx;
}

static void
open_dropdown_popup(Field *f)
{
    DropdownMeta *dm = (DropdownMeta *)f->arg;
    int n, rows, max_vis;
    if (!dm) return;
    g_popup_open   = 1;
    g_popup_dm     = dm;
    g_popup_frow   = f->row;
    g_popup_fcol   = f->col;
    g_popup_fwidth = f->width;
    g_popup_sel    = *dm->index;
    g_popup_scroll = 0;
    for (n = 0; dm->options[n]; n++);
    rows    = getmaxy(stdscr);
    max_vis = rows - f->row - 3;
    if (max_vis < 3)      max_vis = 3;
    if (max_vis > n)      max_vis = n;
    g_popup_scroll = g_popup_sel - max_vis / 2;
    if (g_popup_scroll < 0)           g_popup_scroll = 0;
    if (g_popup_scroll > n - max_vis) g_popup_scroll = n - max_vis;
    if (g_popup_scroll < 0)           g_popup_scroll = 0;
}

/* ------------------------------------------------------------------ */
/* Button callbacks                                                    */
/* ------------------------------------------------------------------ */

static void cb_save_preset(void *arg) { (void)arg; SavePreset(); }


static void
cb_workflow_stop(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < TIMER_COUNT; i++)
        CountdownDoStop(&g_timers[i]);
    g_workflow_phase = 0;
    snprintf(g_status, sizeof(g_status), "Stopped. Ready to restart.");
}

static void
cb_workflow_action(void *arg)
{
    (void)arg;
    switch (g_workflow_phase) {
    case 0:
        CountdownDoStart(&g_timers[0]);
        if (g_timers[0].running) g_workflow_phase = 1;
        break;
    case 2:
        CountdownDoStart(&g_timers[1]);
        if (g_timers[1].running) g_workflow_phase = 3;
        break;
    case 4:
        CountdownDoStart(&g_timers[2]);
        if (g_timers[2].running) g_workflow_phase = 5;
        break;
    case 6:
        CountdownDoReset(&g_timers[0]);
        CountdownDoReset(&g_timers[1]);
        CountdownDoReset(&g_timers[2]);
        g_workflow_phase = 0;
        break;
    }
}
static void
cb_quit(void *arg)
{
    (void)arg;
    if (AnyTimerRunning()) {
        snprintf(g_status, sizeof(g_status), "Stop all timers before quitting.");
        return;
    }
    g_want_quit = 1;
}


static void
cb_apply_defaults(void *arg)
{
    (void)arg;
    snprintf(g_timers[0].hh_buf, sizeof(g_timers[0].hh_buf), "%s", cfg_def_dev_hh);
    snprintf(g_timers[0].mm_buf, sizeof(g_timers[0].mm_buf), "%s", cfg_def_dev_mm);
    snprintf(g_timers[0].ss_buf, sizeof(g_timers[0].ss_buf), "%s", cfg_def_dev_ss);
    snprintf(g_temp_buf, sizeof(g_temp_buf), "%s", cfg_def_temp);
    snprintf(g_timers[1].hh_buf, sizeof(g_timers[1].hh_buf), "%s", cfg_def_stop_hh);
    snprintf(g_timers[1].mm_buf, sizeof(g_timers[1].mm_buf), "%s", cfg_def_stop_mm);
    snprintf(g_timers[1].ss_buf, sizeof(g_timers[1].ss_buf), "%s", cfg_def_stop_ss);
    snprintf(g_timers[2].hh_buf, sizeof(g_timers[2].hh_buf), "%s", cfg_def_fix_hh);
    snprintf(g_timers[2].mm_buf, sizeof(g_timers[2].mm_buf), "%s", cfg_def_fix_mm);
    snprintf(g_timers[2].ss_buf, sizeof(g_timers[2].ss_buf), "%s", cfg_def_fix_ss);
    snprintf(g_status, sizeof(g_status), "Defaults applied to timers.");
}

static void
rebuild_search_matches(void)
{
    int i;
    char query[PRESET_NAME_LEN];
    int  qlen = (int)strlen(g_searchpopup_buf);

    for (i = 0; i < qlen && i < (int)sizeof(query) - 1; i++) {
        unsigned char c = (unsigned char)g_searchpopup_buf[i];
        query[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    query[qlen] = '\0';

    g_searchpopup_nmatches = 0;
    for (i = 0; i < g_preset_count && g_searchpopup_nmatches < MAX_SEARCH_MATCHES; i++) {
        if (qlen == 0) {
            g_searchpopup_matches[g_searchpopup_nmatches++] = i;
        } else {
            const char *name = g_presets[i].name;
            int nlen = (int)strlen(name);
            int j, k;
            for (j = 0; j <= nlen - qlen; j++) {
                int match = 1;
                for (k = 0; k < qlen; k++) {
                    unsigned char c = (unsigned char)name[j + k];
                    char lc = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
                    if (lc != query[k]) { match = 0; break; }
                }
                if (match) { g_searchpopup_matches[g_searchpopup_nmatches++] = i; break; }
            }
        }
    }

    {
        int total = g_searchpopup_nmatches + (g_searchpopup_show_new ? 1 : 0);
        if (g_searchpopup_sel >= total)
            g_searchpopup_sel = total > 0 ? total - 1 : 0;
        if (g_searchpopup_sel < 0)
            g_searchpopup_sel = 0;
    }
}

static void
cb_open_search_popup_impl(int show_new)
{
    if (!show_new && g_preset_count == 0) {
        snprintf(g_status, sizeof(g_status), "No presets saved.");
        return;
    }
    g_searchpopup_show_new = show_new;
    g_searchpopup_buf[0]   = '\0';
    g_searchpopup_sel      = 0;
    g_searchpopup_scroll   = 0;
    rebuild_search_matches();
    g_searchpopup_open     = 1;
}

static void cb_open_search_popup(void *a)    { (void)a; cb_open_search_popup_impl(0); }
static void cb_open_search_popup_db(void *a) { (void)a; cb_open_search_popup_impl(1); }

/* ------------------------------------------------------------------ */
/* Dropdown index sync                                                 */
/* ------------------------------------------------------------------ */

static int
find_option_idx(const char **opts, const char *val)
{
    int i;
    if (!val || !val[0]) return 0;
    for (i = 0; opts[i]; i++)
        if (strcmp(opts[i], val) == 0) return i;
    return 0;
}

static void
sync_dropdown_indices(void)
{
    CountdownTimer *dev = &g_timers[0];
    g_film_idx     = find_option_idx(g_films.ptrs, dev->film_buf);
    g_iso_nom_idx  = find_option_idx(g_isos.ptrs,  dev->iso_buf);
    g_iso_used_idx = find_option_idx(g_isos.ptrs,  dev->iso_used_buf);
    g_dev_idx      = find_option_idx(g_devs.ptrs,  dev->dev_name_buf);
    g_dil_idx      = find_option_idx(g_dils.ptrs,  dev->dilution_buf);
}

static void
sync_bufs_from_indices(void)
{
    CountdownTimer *dev = &g_timers[0];
    if (g_films.count > 0)
        snprintf(dev->film_buf,     sizeof(dev->film_buf),     "%s", g_films.ptrs[g_film_idx]);
    if (g_isos.count > 0) {
        snprintf(dev->iso_buf,      sizeof(dev->iso_buf),      "%s", g_isos.ptrs[g_iso_nom_idx]);
        snprintf(dev->iso_used_buf, sizeof(dev->iso_used_buf), "%s", g_isos.ptrs[g_iso_used_idx]);
    }
    if (g_devs.count > 0)
        snprintf(dev->dev_name_buf, sizeof(dev->dev_name_buf), "%s", g_devs.ptrs[g_dev_idx]);
    if (g_dils.count > 0)
        snprintf(dev->dilution_buf, sizeof(dev->dilution_buf), "%s", g_dils.ptrs[g_dil_idx]);
}

static void
BuildPresetName(char *out, size_t outsz,
                const char *film, const char *iso_nom, const char *iso_used,
                const char *developer, const char *dilution)
{
    if (!iso_nom  || !iso_nom[0])  iso_nom  = "?";
    if (!iso_used || !iso_used[0]) iso_used = iso_nom;
    if (strcmp(iso_nom, iso_used) == 0)
        snprintf(out, outsz, "%s %s %s %s", film, iso_nom, developer, dilution);
    else
        snprintf(out, outsz, "%s [%s @ %s] %s %s", film, iso_nom, iso_used, developer, dilution);
}

static void
AutoUpdatePresetName(void)
{
    CountdownTimer *dev = &g_timers[0];
    BuildPresetName(g_preset_name, sizeof(g_preset_name),
                    dev->film_buf, dev->iso_buf, dev->iso_used_buf,
                    dev->dev_name_buf, dev->dilution_buf);
}

/* ------------------------------------------------------------------ */
/* Top button box                                                      */
/* ------------------------------------------------------------------ */

static const char *tab_names[TAB_COUNT] = { "F1:Timer", "F2:Db", "F3:Calc", "F4:Cfg", "F11:Help", "F12:Quit" };

/* Visual-only redraw of the tab buttons row (no field registration) */
static void
draw_tab_chrome(int row, int cols)
{
    int t, col = INDENT;

    /* Left border + interior fill */
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, 0, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));
    attron(COLOR_PAIR(CP_BOX));
    mvhline(row, 1, ' ', cols - 2);
    attroff(COLOR_PAIR(CP_BOX));

    /* Tab buttons */
    for (t = 0; t < TAB_COUNT; t++) {
        int active = (t == g_tab);
        int w      = (int)strlen(tab_names[t]) + 2;

        g_tab_btn_col[t]   = col;
        g_tab_btn_width[t] = w;

        if (active) {
            attron(COLOR_PAIR(CP_BOX_LINE));
            mvaddch(row, col,         g_vl);
            mvaddch(row, col + w - 1, g_vl);
            attroff(COLOR_PAIR(CP_BOX_LINE));
            attron(A_BOLD | COLOR_PAIR(CP_BOX));
            mvprintw(row, col + 1, "%s", tab_names[t]);
            attroff(A_BOLD | COLOR_PAIR(CP_BOX));
            attron(COLOR_PAIR(CP_BOX_LINE));
            mvaddch(row - 1, col,         g_tt);
            mvaddch(row - 1, col + w - 1, g_tt);
            attroff(COLOR_PAIR(CP_BOX_LINE));
        } else {
            attron(COLOR_PAIR(CP_BOX));
            mvprintw(row, col, " %s ", tab_names[t]);
            attroff(COLOR_PAIR(CP_BOX));
        }

        col += (int)strlen(tab_names[t]) + 3;
    }

    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, cols - 1, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

static void
draw_tab_box_buttons(int row, int cols)
{
    /* Register FT_TABS as field 0 — must come first */
    field_reg(FT_TABS, NULL, 0, 0, NULL, NULL, row, INDENT, cols - INDENT - 1);
    draw_tab_chrome(row, cols);
}

static void
draw_tab_box_bottom(int row, int cols)
{
    int i;
    int gap_s = g_tab_btn_col[g_tab];               /* inclusive gap start */
    int gap_e = gap_s + g_tab_btn_width[g_tab] - 1; /* inclusive gap end   */

    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, 0, g_lt);

    for (i = 1; i < cols - 1; i++) {
        if (i == gap_s) {
            mvaddch(row, i, g_lr);
        } else if (i > gap_s && i < gap_e) {
            /* Open gap — keep box interior color */
            attroff(COLOR_PAIR(CP_BOX_LINE));
            attron(COLOR_PAIR(CP_BOX));
            mvaddch(row, i, ' ');
            attroff(COLOR_PAIR(CP_BOX));
            attron(COLOR_PAIR(CP_BOX_LINE));
        } else if (i == gap_e) {
            mvaddch(row, i, g_ll);
        } else {
            mvaddch(row, i, g_hl);
        }
    }

    mvaddch(row, cols - 1, g_rt);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

/* ------------------------------------------------------------------ */
/* Database tab                                                        */
/* ------------------------------------------------------------------ */

static int
draw_presets(int row, void (*load_cb)(void *))
{
    int cols = getmaxx(stdscr);
    int bw   = cols;

    draw_section_title(row, 0, bw, "Saved settings");
    row++;

    draw_box_sides(row, 0, bw);
    draw_button(row, INDENT, "Load Preset", load_cb, NULL);
    if (g_preset_loaded_id >= 0) {
        int j;
        for (j = 0; j < g_preset_count; j++) {
            if (g_presets[j].id == g_preset_loaded_id) {
                attron(A_DIM | COLOR_PAIR(CP_BOX));
                mvprintw(row, INDENT + 15, "%s", g_presets[j].name);
                attroff(A_DIM | COLOR_PAIR(CP_BOX));
                break;
            }
        }
    }
    row++;

    return row;
}

static int
draw_save_section(int row)
{
    int cols     = getmaxx(stdscr);
    int bw       = cols;
    int fc       = field_col();
    int save_col = cols - 9;

    AutoUpdatePresetName();

    if (save_col < fc + 4) save_col = fc + 4;

    draw_section_title(row, 0, bw, "Save preset");
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Name:");
    attron(A_DIM | COLOR_PAIR(CP_BOX));
    mvprintw(row, fc, "%-*s", save_col - fc - 1, g_preset_name);
    attroff(A_DIM | COLOR_PAIR(CP_BOX));
    draw_button(row, save_col, "Save", cb_save_preset, NULL);
    row++;

    return row;
}

static int
draw_timer_fields(int row, CountdownTimer *t)
{
    int fc   = field_col();
    int cols = getmaxx(stdscr);
    int bw   = cols;
    int fw   = cols - fc - 3;
    if (fw < 10) fw = 10;

    draw_section_title(row, 0, bw, t->label);
    row++;

    if (t->has_temp) {
        draw_box_sides(row, 0, bw);
        draw_label(row, "Temperature:");
        draw_textfield(row, fc, 5, g_temp_buf, sizeof(g_temp_buf), FT_DIGITS, 0);
        attron(COLOR_PAIR(CP_BOX));
        mvprintw(row, fc + 5, " C");
        attroff(COLOR_PAIR(CP_BOX));
        row++;
    }

    if (t->has_recipe) {
        draw_box_sides(row, 0, bw);
        draw_label(row, "Film:");
        draw_dropdown(row, fc, fw, g_films.ptrs, &g_film_idx,
                      t->film_buf, sizeof(t->film_buf), NULL);
        row++;

        draw_box_sides(row, 0, bw);
        draw_label(row, "ISO:");
        draw_dropdown(row, fc, 10, g_isos.ptrs, &g_iso_nom_idx,
                      t->iso_buf, sizeof(t->iso_buf), NULL);
        row++;

        draw_box_sides(row, 0, bw);
        draw_label(row, "ISO used:");
        draw_dropdown(row, fc, 10, g_isos.ptrs, &g_iso_used_idx,
                      t->iso_used_buf, sizeof(t->iso_used_buf), NULL);
        row++;

        draw_box_sides(row, 0, bw);
        draw_label(row, "Developer:");
        draw_dropdown(row, fc, fw, g_devs.ptrs, &g_dev_idx,
                      t->dev_name_buf, sizeof(t->dev_name_buf), NULL);
        row++;

        draw_box_sides(row, 0, bw);
        draw_label(row, "Dilution:");
        draw_dropdown(row, fc, fw, g_dils.ptrs, &g_dil_idx,
                      t->dilution_buf, sizeof(t->dilution_buf), NULL);
        row++;
    }

    draw_box_sides(row, 0, bw);
    draw_label(row, "Time:");
    draw_hms_fields(row, fc, t->hh_buf, t->mm_buf, t->ss_buf);
    row++;

    if (t->has_alarm) {
        draw_box_sides(row, 0, bw);
        draw_label(row, "Alarm:");
        attron(COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "Every ");
        attroff(COLOR_PAIR(CP_BOX));
        draw_textfield(row, fc + 6,  5, t->alarm_buf,     sizeof(t->alarm_buf),     FT_DIGITS, 0);
        attron(COLOR_PAIR(CP_BOX));
        mvprintw(row, fc + 11, " sec  For ");
        attroff(COLOR_PAIR(CP_BOX));
        draw_textfield(row, fc + 21, 5, t->alarm_dur_buf, sizeof(t->alarm_dur_buf), FT_DIGITS, 0);
        attron(COLOR_PAIR(CP_BOX));
        mvprintw(row, fc + 26, " sec");
        attroff(COLOR_PAIR(CP_BOX));
        row++;
    }

    return row;
}

static int
draw_database_tab(int row)
{
    int i;
    row = draw_presets(row, cb_open_search_popup_db);
    for (i = 0; i < TIMER_COUNT; i++)
        row = draw_timer_fields(row, &g_timers[i]);
    row = draw_save_section(row);
    return row;
}

/* ------------------------------------------------------------------ */
/* Timer tab                                                           */
/* ------------------------------------------------------------------ */

static int
draw_timer_tab(int row)
{
    int fc   = field_col();
    int cols = getmaxx(stdscr);
    int bw   = cols;
    CountdownTimer *t;
    const char *btn_label;

    row = draw_presets(row, cb_open_search_popup);

    switch (g_workflow_phase) {
    case 0:  t = &g_timers[0]; btn_label = "Start development"; break;
    case 1:  t = &g_timers[0]; btn_label = NULL;                break;
    case 2:  t = &g_timers[1]; btn_label = "Stop bath";         break;
    case 3:  t = &g_timers[1]; btn_label = NULL;                break;
    case 4:  t = &g_timers[2]; btn_label = "Fix";               break;
    case 5:  t = &g_timers[2]; btn_label = NULL;                break;
    case 6:  t = &g_timers[2]; btn_label = "Reset";             break;
    default: t = &g_timers[0]; btn_label = "Start development"; break;
    }

    draw_section_title(row, 0, bw, t->label);
    row++;

    draw_box_sides(row, 0, bw);
    if (t->running) {
        attron(A_BOLD | A_BLINK | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "%s:%s:%s  RUNNING",
                 t->hh_buf, t->mm_buf, t->ss_buf);
        attroff(A_BOLD | A_BLINK | COLOR_PAIR(CP_BOX));
    } else if (g_workflow_phase == 6) {
        attron(A_BOLD | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "DONE");
        attroff(A_BOLD | COLOR_PAIR(CP_BOX));
    } else if (t->remaining == 0 &&
               strcmp(t->hh_buf, "00") == 0 &&
               strcmp(t->mm_buf, "00") == 0 &&
               strcmp(t->ss_buf, "00") == 0) {
        attron(COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "--:--:--");
        attroff(COLOR_PAIR(CP_BOX));
    } else {
        attron(COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "%s:%s:%s",
                 t->hh_buf, t->mm_buf, t->ss_buf);
        attroff(COLOR_PAIR(CP_BOX));
    }
    row++;

    /* Progress bar */
    if (t->total > 0 && g_workflow_phase != 6) {
        int bar_w    = bw - 2 * INDENT - 4; /* space for '[' and ']' caps */
        int elapsed  = t->total - t->remaining;
        int filled   = (int)((long)bar_w * elapsed / t->total);
        int interval = (t->has_alarm) ? (int)strtol(t->alarm_buf, NULL, 10) : 0;
        char alarm_mark[512];
        int i;
        if (filled < 0)     filled = 0;
        if (filled > bar_w) filled = bar_w;
        /* build alarm tick map */
        memset(alarm_mark, 0, sizeof(alarm_mark));
        if (interval > 0) {
            int n;
            for (n = interval; n < t->total; n += interval) {
                int pos = (int)((long)n * bar_w / t->total);
                if (pos >= 0 && pos < bar_w && pos < (int)sizeof(alarm_mark))
                    alarm_mark[pos] = 1;
            }
        }
        draw_box_sides(row, 0, bw);
        attron(COLOR_PAIR(CP_BOX));
        mvaddch(row, INDENT + 1, '[');
        mvaddch(row, INDENT + 2 + bar_w, ']');
        attroff(COLOR_PAIR(CP_BOX));
        for (i = 0; i < bar_w; i++) {
            int is_alarm = (i < (int)sizeof(alarm_mark) && alarm_mark[i]);
            if (is_alarm) {
                attron(COLOR_PAIR(CP_ALARM));
                mvaddch(row, INDENT + 2 + i, ' ');
                attroff(COLOR_PAIR(CP_ALARM));
            } else if (i < filled) {
                attron(COLOR_PAIR(CP_PROGRESS));
                mvaddch(row, INDENT + 2 + i, ' ');
                attroff(COLOR_PAIR(CP_PROGRESS));
            } else {
                attron(A_DIM | COLOR_PAIR(CP_BOX));
                mvaddch(row, INDENT + 2 + i, ACS_CKBOARD);
                attroff(A_DIM | COLOR_PAIR(CP_BOX));
            }
        }
        row++;
    }

    if (t->has_alarm && g_workflow_phase != 6) {
        draw_box_sides(row, 0, bw);
        attron(A_DIM | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "Alarm every %s sec, for %s sec",
                 t->alarm_buf, t->alarm_dur_buf);
        attroff(A_DIM | COLOR_PAIR(CP_BOX));
        row++;
    }

    draw_box_sides(row, 0, bw);
    if (btn_label)
        draw_button(row, INDENT, btn_label, cb_workflow_action, NULL);
    else
        draw_button(row, INDENT, "Stop", cb_workflow_stop, NULL);
    row++;

    return row;
}

/* ------------------------------------------------------------------ */
/* Calc tab                                                            */
/* ------------------------------------------------------------------ */

static void
cb_load_from_preset(void *arg)
{
    CountdownTimer *dev = &g_timers[0];
    (void)arg;
    snprintf(calc_mm_buf,      sizeof(calc_mm_buf),      "%s", dev->mm_buf);
    snprintf(calc_ss_buf,      sizeof(calc_ss_buf),       "%s", dev->ss_buf);
    snprintf(calc_base_temp,   sizeof(calc_base_temp),   "%s", g_temp_buf);
    snprintf(calc_actual_temp, sizeof(calc_actual_temp), "%s", g_temp_buf);
    snprintf(g_status, sizeof(g_status),
             "Loaded from Development: %s:%s at %s C",
             dev->mm_buf, dev->ss_buf, g_temp_buf);
}

static double
temp_compensate(int base_sec, double base_temp, double actual_temp)
{
    double exponent = (actual_temp - base_temp) / 10.0;
    double factor   = pow(2.0, exponent);
    if (factor <= 0.0) return -1.0;
    return (double)base_sec / factor;
}

static int
parse_dilution(const char *s, int *dev_parts, int *water_parts)
{
    static const char stock[] = "stock";
    const char *p = s;
    int i;

    for (i = 0; stock[i]; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c != (unsigned char)stock[i]) break;
    }
    if (stock[i] == '\0' && p[i] == '\0') {
        *dev_parts = 1; *water_parts = 0; return 1;
    }

    *dev_parts = (int)strtol(p, (char **)&p, 10);
    if (*dev_parts <= 0 || *p != ':') return 0;
    p++;
    *water_parts = (int)strtol(p, NULL, 10);
    if (*water_parts < 0) return 0;
    return 1;
}

static int
draw_calc_tab(int row)
{
    int fc   = field_col();
    int cols = getmaxx(stdscr);
    int bw   = cols;
    int base_mm, base_ss, base_sec;
    double base_temp_d, actual_temp_d, vol_d;
    int dev_parts, water_parts;

    draw_section_title(row, 0, bw, "Temperature compensation");
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Base time:");
    draw_textfield(row, fc,     4, calc_mm_buf, sizeof(calc_mm_buf), FT_SPINNER, 999);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 4, ":");
    attroff(COLOR_PAIR(CP_BOX));
    draw_textfield(row, fc + 5, 4, calc_ss_buf, sizeof(calc_ss_buf), FT_SPINNER, 59);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 9, " (MM:SS)");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Base temp:");
    draw_textfield(row, fc, 5, calc_base_temp, sizeof(calc_base_temp), FT_DIGITS, 0);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 5, " C");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Actual temp:");
    draw_textfield(row, fc, 5, calc_actual_temp, sizeof(calc_actual_temp), FT_DIGITS, 0);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 5, " C");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, bw);
    draw_button(row, fc, "Load from Development", cb_load_from_preset, NULL);
    row++;

    base_mm       = (int)strtol(calc_mm_buf,    NULL, 10);
    base_ss       = (int)strtol(calc_ss_buf,    NULL, 10);
    base_sec      = base_mm * 60 + base_ss;
    base_temp_d   = strtod(calc_base_temp,   NULL);
    actual_temp_d = strtod(calc_actual_temp, NULL);

    draw_box_sides(row, 0, bw);
    draw_label(row, "Adjusted:");
    if (base_sec > 0) {
        double adj   = temp_compensate(base_sec, base_temp_d, actual_temp_d);
        int    adj_i = (int)(adj + 0.5);
        double diff  = actual_temp_d - base_temp_d;
        attron(A_BOLD | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "%02d:%02d", adj_i / 60, adj_i % 60);
        attroff(A_BOLD | COLOR_PAIR(CP_BOX));
        if (diff != 0.0) {
            attron(COLOR_PAIR(CP_BOX));
            mvprintw(row, fc + 6, "  (%+.1f C, factor %.3f)",
                     diff, pow(2.0, -diff / 10.0));
            attroff(COLOR_PAIR(CP_BOX));
        }
    } else {
        attron(A_DIM | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "--:--");
        attroff(A_DIM | COLOR_PAIR(CP_BOX));
    }
    row++;

    draw_section_title(row, 0, bw, "Dilution");
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Dilution:");
    draw_textfield(row, fc, 10, calc_dilution, sizeof(calc_dilution), FT_TEXT, 0);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 10, "  (e.g. 1:100 or Stock)");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Volume:");
    draw_textfield(row, fc, 7, calc_volume, sizeof(calc_volume), FT_DIGITS, 0);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 7, " ml");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    vol_d = strtod(calc_volume, NULL);
    draw_box_sides(row, 0, bw);
    draw_label(row, "Developer:");
    if (vol_d > 0.0 && parse_dilution(calc_dilution, &dev_parts, &water_parts)) {
        int total_parts = dev_parts + water_parts;
        double dev_ml   = (total_parts > 0)
                          ? vol_d * dev_parts / total_parts
                          : vol_d;
        double water_ml = vol_d - dev_ml;
        attron(A_BOLD | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "%.2f ml developer", dev_ml);
        attroff(A_BOLD | COLOR_PAIR(CP_BOX));
        row++;
        draw_box_sides(row, 0, bw);
        draw_label(row, "Water:");
        attron(A_BOLD | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "%.2f ml water", water_ml);
        attroff(A_BOLD | COLOR_PAIR(CP_BOX));
        row++;
        draw_box_sides(row, 0, bw);
        attron(A_DIM | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "Total: %.0f ml", vol_d);
        attroff(A_DIM | COLOR_PAIR(CP_BOX));
        row++;
    } else {
        attron(A_DIM | COLOR_PAIR(CP_BOX));
        mvprintw(row, fc, "--");
        attroff(A_DIM | COLOR_PAIR(CP_BOX));
        row++;
    }

    return row;
}

/* ------------------------------------------------------------------ */
/* Config tab                                                          */
/* ------------------------------------------------------------------ */

static void
cb_cfg_save_edit(void *arg)
{
    DynList *dl = (DynList *)arg;
    if (!dl || !dl->edit_buf[0] || dl->count == 0) return;
    if (strcmp(dl->edit_buf, dl->items[dl->sel]) == 0) {
        snprintf(g_status, sizeof(g_status), "No change.");
        return;
    }
    RenameOption(dl->category, dl->items[dl->sel], dl->edit_buf);
    snprintf(dl->items[dl->sel], MAX_OPTION_LEN, "%s", dl->edit_buf);
    dynlist_rebuild_ptrs(dl);
    sync_dropdown_indices();
    LoadPresetList();
    snprintf(g_status, sizeof(g_status), "Saved: %s", dl->edit_buf);
}

static void
cb_cfg_save_new(void *arg)
{
    DynList *dl = (DynList *)arg;
    int i;
    if (!dl || !dl->edit_buf[0]) return;
    for (i = 0; i < dl->count; i++)
        if (strcmp(dl->items[i], dl->edit_buf) == 0) {
            snprintf(g_status, sizeof(g_status), "Already exists: %s", dl->edit_buf);
            return;
        }
    SaveOption(dl->category, dl->edit_buf);
    dynlist_add(dl, dl->edit_buf);
    snprintf(g_status, sizeof(g_status), "Added: %s", dl->edit_buf);
}

static void
cb_cfg_delete(void *arg)
{
    DynList *dl = (DynList *)arg;
    char deleted[MAX_OPTION_LEN];
    if (!dl || dl->count == 0) return;
    snprintf(deleted, sizeof(deleted), "%s", dl->items[dl->sel]);
    DeleteOption(dl->category, deleted);
    dynlist_delete(dl, dl->sel);
    sync_dropdown_indices();
    if (dl->count > 0)
        snprintf(dl->edit_buf, sizeof(dl->edit_buf), "%s", dl->items[dl->sel]);
    else
        dl->edit_buf[0] = '\0';
    snprintf(g_status, sizeof(g_status), "Deleted: %s", deleted);
}

static int
draw_dynlist_section(int row, DynList *dl, const char *title)
{
    int cols = getmaxx(stdscr);
    int bw   = cols;
    int fc   = field_col();
    int fw   = cols - fc - 3;
    if (fw < 20) fw = 20;

    draw_section_title(row, 0, bw, title);
    row++;

    /* Select row — dropdown (only if list non-empty) */
    draw_box_sides(row, 0, bw);
    if (dl->count > 0) {
        draw_label(row, "Select:");
        draw_dropdown(row, fc, fw, dl->ptrs, &dl->sel,
                      dl->edit_buf, sizeof(dl->edit_buf), NULL);
    } else {
        attron(A_DIM | COLOR_PAIR(CP_BOX));
        mvprintw(row, INDENT + 2, "(empty list)");
        attroff(A_DIM | COLOR_PAIR(CP_BOX));
    }
    row++;

    /* Name text field */
    draw_box_sides(row, 0, bw);
    draw_label(row, "Name:");
    draw_textfield(row, fc, fw, dl->edit_buf, sizeof(dl->edit_buf), FT_TEXT, 0);
    row++;

    /* Buttons row */
    draw_box_sides(row, 0, bw);
    if (dl->count > 0) {
        draw_button(row, fc,       "Save edit",   cb_cfg_save_edit, dl);
        draw_button(row, fc + 13,  "Save as new", cb_cfg_save_new,  dl);
        draw_button(row, fc + 27,  "Delete",      cb_cfg_delete,    dl);
    } else {
        draw_button(row, fc, "Save as new", cb_cfg_save_new, dl);
    }
    row++;

    return row;
}

static int
draw_config_tab(int row)
{
    int fc   = field_col();
    int cols = getmaxx(stdscr);
    int bw   = cols;

    row = draw_dynlist_section(row, &g_films, "Film stocks");
    row = draw_dynlist_section(row, &g_devs,  "Developers");
    row = draw_dynlist_section(row, &g_isos,  "ISO values");
    row = draw_dynlist_section(row, &g_dils,  "Dilutions");

    /* Defaults section */
    draw_section_title(row, 0, bw, "Defaults");
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Dev default:");
    draw_hms_fields(row, fc, cfg_def_dev_hh, cfg_def_dev_mm, cfg_def_dev_ss);
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Stop default:");
    draw_hms_fields(row, fc, cfg_def_stop_hh, cfg_def_stop_mm, cfg_def_stop_ss);
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Fix default:");
    draw_hms_fields(row, fc, cfg_def_fix_hh, cfg_def_fix_mm, cfg_def_fix_ss);
    row++;

    draw_box_sides(row, 0, bw);
    draw_label(row, "Temperature:");
    draw_textfield(row, fc, 5, cfg_def_temp, sizeof(cfg_def_temp), FT_DIGITS, 0);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, fc + 5, " C");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, bw);
    draw_button(row, fc, "Apply defaults to timers", cb_apply_defaults, NULL);
    row++;

    (void)fc;
    return row;
}

/* ------------------------------------------------------------------ */
/* Dropdown popup                                                      */
/* ------------------------------------------------------------------ */

static void
draw_dropdown_popup(void)
{
    int n, i, rows, cols, pw, max_vis, ph, pr, pc;
    const char *val;

    if (!g_popup_open || !g_popup_dm) return;

    for (n = 0; g_popup_dm->options[n]; n++);

    cols = getmaxx(stdscr);
    rows = getmaxy(stdscr);

    pw = g_popup_fwidth;
    for (i = 0; i < n; i++) {
        int l = (int)strlen(g_popup_dm->options[i]) + 4;
        if (l > pw) pw = l;
    }
    if (pw > cols) pw = cols;

    max_vis = rows - g_popup_frow - 3;
    if (max_vis < 3) max_vis = 3;
    if (max_vis > n) max_vis = n;

    ph = max_vis + 2;
    pr = g_popup_frow + 1;
    if (pr + ph > rows - 2)
        pr = g_popup_frow - ph;
    if (pr < 0) pr = 0;

    pc = g_popup_fcol;
    if (pc + pw > cols) pc = cols - pw;
    if (pc < 0) pc = 0;

    if (g_popup_sel < g_popup_scroll)
        g_popup_scroll = g_popup_sel;
    if (g_popup_sel >= g_popup_scroll + max_vis)
        g_popup_scroll = g_popup_sel - max_vis + 1;

    draw_popup_frame(pr, pc, ph, pw);

    for (i = 0; i < max_vis; i++) {
        int idx    = g_popup_scroll + i;
        int is_sel = (idx == g_popup_sel);
        int j, vlen;

        val  = g_popup_dm->options[idx];
        vlen = (int)strlen(val);

        if (is_sel) attron(A_REVERSE | A_BOLD);
        move(pr + 1 + i, pc + 1);
        if (i == 0 && g_popup_scroll > 0)
            addch(ACS_UARROW);
        else if (i == max_vis - 1 && g_popup_scroll + max_vis < n)
            addch(ACS_DARROW);
        else
            addch(' ');
        for (j = 0; j < pw - 3; j++)
            addch(j < vlen ? (unsigned char)val[j] : ' ');
        if (is_sel) attroff(A_REVERSE | A_BOLD);
    }
}

static void
draw_edit_popup(void)
{
    int cols, rows, pw, fw, pr, pc, len, i;

    if (!g_editpopup_open) return;

    cols = getmaxx(stdscr);
    rows = getmaxy(stdscr);

    pw = 54;
    if (pw > cols - 2) pw = cols - 2;
    fw = pw - 10;   /* text field width inside the box */

    pr = rows / 2 - 2;
    pc = (cols - pw) / 2;
    if (pc < 0) pc = 0;

    draw_popup_frame(pr, pc, 4, pw);

    /* Edit row */
    mvprintw(pr + 1, pc + 1, " Edit: ");
    len = (int)strlen(g_editpopup_buf);
    attron(A_REVERSE | A_BOLD);
    move(pr + 1, pc + 8);
    for (i = 0; i < fw; i++)
        addch(i < len ? (unsigned char)g_editpopup_buf[i] : ' ');
    attroff(A_REVERSE | A_BOLD);

    /* Hint row */
    attron(A_DIM);
    mvprintw(pr + 2, pc + 1, "%-*.*s", pw - 2, pw - 2, " Enter=save  Esc=cancel");
    attroff(A_DIM);

    /* Place cursor at end of text */
    {
        int cpos = len < fw ? len : fw - 1;
        move(pr + 1, pc + 8 + cpos);
        curs_set(1);
    }
}

static void
draw_search_popup(void)
{
#define SEARCH_VIS 8
    int rows, cols, pw, pr, pc, fw, i, len, r;

    if (!g_searchpopup_open) return;

    rows = getmaxy(stdscr);
    cols = getmaxx(stdscr);

    pw = 54;
    if (pw > cols - 2) pw = cols - 2;
    if (pw < 22) pw = 22;
    fw = pw - 12;   /* search field width */

    /* height: top + search + sep + SEARCH_VIS list rows + hint + bottom */
    pr = (rows - (SEARCH_VIS + 5)) / 2;
    if (pr < 0) pr = 0;
    pc = (cols - pw) / 2;
    if (pc < 0) pc = 0;

    /* scroll adjustment */
    if (g_searchpopup_sel < g_searchpopup_scroll)
        g_searchpopup_scroll = g_searchpopup_sel;
    if (g_searchpopup_sel >= g_searchpopup_scroll + SEARCH_VIS)
        g_searchpopup_scroll = g_searchpopup_sel - SEARCH_VIS + 1;

    draw_popup_frame(pr, pc, SEARCH_VIS + 5, pw);
    r = pr + 1;

    /* Search input row */
    len = (int)strlen(g_searchpopup_buf);
    mvprintw(r, pc + 1, " Search: ");
    attron(A_REVERSE | A_BOLD);
    move(r, pc + 10);
    for (i = 0; i < fw; i++)
        addch(i < len ? (unsigned char)g_searchpopup_buf[i] : ' ');
    attroff(A_REVERSE | A_BOLD);
    r++;

    /* Separator */
    mvaddch(r, pc, g_lt);
    mvhline(r, pc + 1, g_hl, pw - 2);
    mvaddch(r, pc + pw - 1, g_rt);
    r++;

    /* List rows */
    {
        int total = g_searchpopup_nmatches + (g_searchpopup_show_new ? 1 : 0);
        for (i = 0; i < SEARCH_VIS; i++) {
            int list_idx = g_searchpopup_scroll + i;
            if (list_idx < total) {
                int is_sel = (list_idx == g_searchpopup_sel);
                int j;
                if (is_sel) attron(A_REVERSE | A_BOLD);
                move(r, pc + 1);
                if (i == 0 && g_searchpopup_scroll > 0)
                    addch(ACS_UARROW);
                else if (i == SEARCH_VIS - 1 &&
                         g_searchpopup_scroll + SEARCH_VIS < total)
                    addch(ACS_DARROW);
                else
                    addch(' ');
                if (g_searchpopup_show_new && list_idx == 0) {
                    const char *label = "new";
                    int llen = 3;
                    for (j = 0; j < pw - 3; j++)
                        addch(j < llen ? (unsigned char)label[j] : ' ');
                } else {
                    int pi = list_idx - (g_searchpopup_show_new ? 1 : 0);
                    const char *name = g_presets[g_searchpopup_matches[pi]].name;
                    int nlen = (int)strlen(name);
                    for (j = 0; j < pw - 3; j++)
                        addch(j < nlen ? (unsigned char)name[j] : ' ');
                }
                if (is_sel) attroff(A_REVERSE | A_BOLD);
            } else if (i == 0 && total == 0) {
                attron(A_DIM);
                mvprintw(r, pc + 1, " %-*s", pw - 3, "No matches");
                attroff(A_DIM);
            } else {
                mvprintw(r, pc + 1, "%-*s", pw - 2, "");
            }
            r++;
        }
    }

    /* Hint row */
    attron(A_DIM);
    {
        const char *hint = " Enter=load  Esc=cancel  Up/Down=select";
        int hlen = (int)strlen(hint);
        int j;
        mvprintw(r, pc + 1, "%s", hint);
        for (j = hlen + 1; j < pw - 1; j++) mvaddch(r, pc + j, ' ');
    }
    attroff(A_DIM);

    /* Cursor in search field */
    {
        int cpos = len < fw ? len : fw - 1;
        move(pr + 1, pc + 10 + cpos);
        curs_set(1);
    }
#undef SEARCH_VIS
}

/* ------------------------------------------------------------------ */
/* Help tab                                                            */
/* ------------------------------------------------------------------ */

static int
draw_help_tab(int row)
{
    int cols = getmaxx(stdscr);
    int r = row + 1;

    attron(A_BOLD | COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "Keyboard Shortcuts");
    attroff(A_BOLD | COLOR_PAIR(CP_BOX));
    r++;

    attron(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "Global");
    attroff(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "  F1           Switch to Timer tab");
    mvprintw(r++, INDENT, "  F2           Switch to Database tab");
    mvprintw(r++, INDENT, "  F3           Switch to Calc tab");
    mvprintw(r++, INDENT, "  F4           Switch to Config tab");
    mvprintw(r++, INDENT, "  F11          Switch to Help tab");
    mvprintw(r++, INDENT, "  F12          Switch to Quit tab");
    mvprintw(r++, INDENT, "  Tab          Focus next field");
    mvprintw(r++, INDENT, "  Shift+Tab    Focus previous field");
    attroff(COLOR_PAIR(CP_BOX));
    r++;

    attron(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "Database tab");
    attroff(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "  Enter / Space   Open dropdown");
    mvprintw(r++, INDENT, "  Up / Down       Navigate list");
    mvprintw(r++, INDENT, "  Esc             Close dropdown");
    mvprintw(r++, INDENT, "  d               Delete preset");
    attroff(COLOR_PAIR(CP_BOX));
    r++;

    attron(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "Timer tab");
    attroff(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "  Enter           Activate timer");
    mvprintw(r++, INDENT, "  Load Preset     Search presets");
    attroff(COLOR_PAIR(CP_BOX));
    r++;

    attron(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "Config tab");
    attroff(A_UNDERLINE | COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(r++, INDENT, "  Up / Down       Select item");
    mvprintw(r++, INDENT, "  Enter           Add item");
    mvprintw(r++, INDENT, "  d               Delete item");
    attroff(COLOR_PAIR(CP_BOX));

    (void)cols;
    return r;
}

/* ------------------------------------------------------------------ */
/* Quit tab                                                            */
/* ------------------------------------------------------------------ */

static int
draw_quit_tab(int row)
{
    int cols = getmaxx(stdscr);
    int mid   = cols / 2;

    draw_section_title(row, 0, cols, "Exit");  row++;
    draw_box_sides(row, 0, cols);
    attron(A_BOLD | COLOR_PAIR(CP_BOX));
    mvprintw(row, mid - 9, "Press Enter to quit");
    attroff(A_BOLD | COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, cols);  row++;
    draw_box_sides(row, 0, cols);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, mid - 18, "All unsaved changes will be lost.");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, cols);  row++;
    draw_box_sides(row, 0, cols);
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, mid - 12, "Keyboard shortcut: F12");
    attroff(COLOR_PAIR(CP_BOX));
    row++;

    draw_box_sides(row, 0, cols);  row++;

    draw_button(row, mid - 4, "Quit", cb_quit, NULL);
    return row + 1;
}

/* ------------------------------------------------------------------ */
/* Main draw                                                           */
/* ------------------------------------------------------------------ */

static void
draw_all(void)
{
    int row  = 0;
    int rows = getmaxy(stdscr);
    int cols = getmaxx(stdscr);
    int r;
    int content_top, content_bot, visible_h, scroll, last_content_row;

    content_top = 3;
    content_bot = rows - 4;
    visible_h   = content_bot - content_top + 1;
    if (visible_h < 1) visible_h = 1;

    /* Auto-scroll: bring focused field into view using previous frame's positions */
    if (g_nfields > 0 && g_focus > 0 && g_focus < g_nfields) {
        Field *f = &g_fields[g_focus];
        if (f->row < content_top)
            g_content_scroll[g_tab] += (f->row - content_top);
        else if (f->row > content_bot)
            g_content_scroll[g_tab] += (f->row - content_bot);
        if (g_content_scroll[g_tab] < 0)
            g_content_scroll[g_tab] = 0;
    }
    scroll = g_content_scroll[g_tab];

    fields_reset();
    erase();

    /* ---- Top (button) box: 3 rows ---- */
    draw_box_top_plain(row, 0, cols);   row++;   /* ┌──...──┐          */
    draw_tab_box_buttons(row, cols);    row++;   /* │ tabs  Quit │     */
    draw_tab_box_bottom(row, cols);     row++;   /* └──[gap]──...──┘   */

    /* ---- Main content area: borders + fill ---- */
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvvline(row, 0,        g_vl, rows - 3 - row);
    mvvline(row, cols - 1, g_vl, rows - 3 - row);
    attroff(COLOR_PAIR(CP_BOX_LINE));
    attron(COLOR_PAIR(CP_BOX));
    for (r = row; r <= rows - 4; r++)
        mvhline(r, 1, ' ', cols - 2);
    attroff(COLOR_PAIR(CP_BOX));

    /* ---- Tab content (offset by scroll) ---- */
    if (g_tab == TAB_DATABASE)    last_content_row = draw_database_tab(row - scroll);
    else if (g_tab == TAB_TIMER)  last_content_row = draw_timer_tab(row - scroll);
    else if (g_tab == TAB_CONFIG) last_content_row = draw_config_tab(row - scroll);
    else if (g_tab == TAB_HELP)   last_content_row = draw_help_tab(row - scroll);
    else if (g_tab == TAB_QUIT)   last_content_row = draw_quit_tab(row - scroll);
    else                          last_content_row = draw_calc_tab(row - scroll);

    /* Clamp scroll: don't scroll past end of content */
    {
        int count      = last_content_row - (content_top - scroll);
        int max_scroll = count - visible_h;
        if (max_scroll < 0) max_scroll = 0;
        if (g_content_scroll[g_tab] > max_scroll)
            g_content_scroll[g_tab] = max_scroll;
    }

    /* Scroll indicators on right border */
    if (content_bot >= content_top) {
        if (scroll > 0) {
            attron(COLOR_PAIR(CP_BOX_LINE));
            mvaddch(content_top, cols - 1, ACS_UARROW);
            attroff(COLOR_PAIR(CP_BOX_LINE));
        }
        {
            int count      = last_content_row - (content_top - scroll);
            int max_scroll = count - visible_h;
            if (max_scroll > 0 && scroll < max_scroll) {
                attron(COLOR_PAIR(CP_BOX_LINE));
                mvaddch(content_bot, cols - 1, ACS_DARROW);
                attroff(COLOR_PAIR(CP_BOX_LINE));
            }
        }
    }

    /* ---- Redraw top chrome to cover any content that leaked upward ---- */
    draw_box_top_plain(0, 0, cols);
    draw_tab_chrome(1, cols);
    draw_tab_box_bottom(2, cols);

    /* ---- Separator: content bottom / status top ---- */
    draw_h_separator(rows - 3, 0, cols);   /* ├──...──┤ */

    /* ---- Status line ---- */
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(rows - 2, 0, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(rows - 2, 1, "%-*s", cols - 2, g_status);
    attroff(COLOR_PAIR(CP_BOX));
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(rows - 2, cols - 1, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));

    /* ---- Status box bottom border ---- */
    draw_box_bottom(rows - 1, 0, cols);              /* └──...──┘ */

    /* ---- Cursor on focused text field ---- */
    if (g_focus >= 0 && g_focus < g_nfields) {
        Field *f = &g_fields[g_focus];
        if (f->type == FT_TEXT || f->type == FT_DIGITS || f->type == FT_SPINNER) {
            int len  = (int)strlen(f->buf);
            int cpos = len < f->width ? len : f->width - 1;
            move(f->row, f->col + cpos);
            curs_set(1);
        } else {
            curs_set(0);
        }
    } else {
        curs_set(0);
    }

    draw_dropdown_popup();
    draw_edit_popup();
    draw_search_popup();
    refresh();
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

static int
buf_insert(char *buf, size_t bufsz, int ch)
{
    int len = (int)strlen(buf);
    if (len + 1 >= (int)bufsz) return 0;
    buf[len]     = (char)ch;
    buf[len + 1] = '\0';
    return 1;
}

static int
buf_backspace(char *buf)
{
    int len = (int)strlen(buf);
    if (len == 0) return 0;
    buf[len - 1] = '\0';
    return 1;
}

static void
confirm_popup(void)
{
    DropdownMeta *dm = g_popup_dm;
    *dm->index = g_popup_sel;
    snprintf(dm->buf, dm->bufsz, "%s", dm->options[g_popup_sel]);
    AutoUpdatePresetName();
    g_popup_open = 0;
    g_popup_dm   = NULL;
    if (dm->on_confirm) dm->on_confirm(g_popup_sel);
}

static int handle_key(int ch)
{
    Field *f;

    if (g_nfields == 0) return 0;
    if (g_focus < 0)          g_focus = 0;
    if (g_focus >= g_nfields) g_focus = g_nfields - 1;

    /* PageUp / PageDown — scroll content area */
    if (ch == KEY_PPAGE) {
        int rows_h = getmaxy(stdscr);
        int page = rows_h - 7; if (page < 1) page = 1;
        g_content_scroll[g_tab] -= page;
        if (g_content_scroll[g_tab] < 0) g_content_scroll[g_tab] = 0;
        return 0;
    }
    if (ch == KEY_NPAGE) {
        int rows_h = getmaxy(stdscr);
        int page = rows_h - 7; if (page < 1) page = 1;
        g_content_scroll[g_tab] += page;
        return 0;
    }

    /* Ctrl+Q — global quit (same guard as the Quit button) */
    if (ch == ('q' & 0x1f)) {
        if (AnyTimerRunning())
            snprintf(g_status, sizeof(g_status), "Stop all timers before quitting.");
        else
            g_want_quit = 1;
        return 0;
    }

    /* Edit popup mode - intercept all keys */
    if (g_editpopup_open) {
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
            buf_backspace(g_editpopup_buf);
        else if (ch >= 32 && ch < 127)
            buf_insert(g_editpopup_buf, sizeof(g_editpopup_buf), ch);
        else if (ch == '\n' || ch == '\r') {
            if (g_editpopup_buf[0] &&
                strcmp(g_editpopup_buf, g_editpopup_orig) != 0) {
                RenameOption(g_editpopup_dl->category,
                             g_editpopup_orig, g_editpopup_buf);
                snprintf(g_editpopup_dl->items[g_editpopup_idx],
                         MAX_OPTION_LEN, "%s", g_editpopup_buf);
                dynlist_rebuild_ptrs(g_editpopup_dl);
                sync_dropdown_indices();
                LoadPresetList();
                snprintf(g_status, sizeof(g_status),
                         "Renamed to: %s", g_editpopup_buf);
            }
            g_editpopup_open = 0;
            g_editpopup_dl   = NULL;
        } else if (ch == 27) { /* ESC */
            g_editpopup_open = 0;
            g_editpopup_dl   = NULL;
        }
        return 0;
    }

    /* Search preset popup mode - intercept all keys */
    if (g_searchpopup_open) {
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            buf_backspace(g_searchpopup_buf);
            rebuild_search_matches();
        } else if (ch >= 32 && ch < 127) {
            buf_insert(g_searchpopup_buf, sizeof(g_searchpopup_buf), ch);
            rebuild_search_matches();
        } else if (ch == KEY_UP) {
            if (g_searchpopup_sel > 0) {
                g_searchpopup_sel--;
                if (g_searchpopup_sel < g_searchpopup_scroll)
                    g_searchpopup_scroll = g_searchpopup_sel;
            }
        } else if (ch == KEY_DOWN) {
            int total = g_searchpopup_nmatches + (g_searchpopup_show_new ? 1 : 0);
            if (g_searchpopup_sel < total - 1) {
                g_searchpopup_sel++;
                if (g_searchpopup_sel >= g_searchpopup_scroll + 8)
                    g_searchpopup_scroll = g_searchpopup_sel - 7;
            }
        } else if (ch == '\n' || ch == '\r') {
            if (g_searchpopup_show_new && g_searchpopup_sel == 0) {
                g_preset_name[0]   = '\0';
                g_preset_loaded_id = -1;
                g_status[0]        = '\0';
                g_searchpopup_open = 0;
            } else if (g_searchpopup_nmatches > 0) {
                int pi  = g_searchpopup_sel - (g_searchpopup_show_new ? 1 : 0);
                int idx = g_searchpopup_matches[pi];
                if (AnyTimerRunning()) {
                    snprintf(g_status, sizeof(g_status),
                             "Stop all timers before loading a preset.");
                } else {
                    LoadPresetIntoTimers(g_presets[idx].id);
                    g_preset_loaded_id = g_presets[idx].id;
                    g_workflow_phase   = 0;
                    snprintf(g_preset_name, sizeof(g_preset_name),
                             "%s", g_presets[idx].name);
                    snprintf(g_status, sizeof(g_status),
                             "Loaded: %s", g_presets[idx].name);
                    g_searchpopup_open = 0;
                }
            }
        } else if (ch == 27) { /* ESC */
            g_searchpopup_open = 0;
        }
        return 0;
    }

    /* Popup mode - intercept all keys */
    if (g_popup_open && g_popup_dm) {
        int n;
        for (n = 0; g_popup_dm->options[n]; n++);
        if (ch == KEY_UP) {
            g_popup_sel = (g_popup_sel - 1 + n) % n;
        } else if (ch == KEY_DOWN) {
            g_popup_sel = (g_popup_sel + 1) % n;
        } else if (ch == '\n' || ch == '\r' || ch == ' ') {
            confirm_popup();
        } else if (ch == 27) { /* ESC */
            g_popup_open = 0;
            g_popup_dm   = NULL;
        } else if (ch == '\t') {
            confirm_popup();
            g_focus = (g_focus + 1) % g_nfields;
            g_status[0] = '\0';
        } else if (ch == KEY_BTAB) {
            confirm_popup();
            g_focus = (g_focus - 1 + g_nfields) % g_nfields;
            g_status[0] = '\0';
        }
        return 0;
    }

    /* F-key tab switch - always available, even in text/digit/spinner fields */
    if (ch == KEY_F(1))  { g_tab = TAB_TIMER;    g_focus = 0; return 0; }
    if (ch == KEY_F(2))  { g_tab = TAB_DATABASE; g_focus = 0; return 0; }
    if (ch == KEY_F(3))  { g_tab = TAB_CALC;     g_focus = 0; return 0; }
    if (ch == KEY_F(4))  { g_tab = TAB_CONFIG;   g_focus = 0; return 0; }
    if (ch == KEY_F(11)) { g_tab = TAB_HELP;     g_focus = 0; return 0; }
    if (ch == KEY_F(12)) { g_tab = TAB_QUIT; g_focus = 0; return 0; }

    /* Global tab switch shortcuts - only when not in a text/digit/spinner field */
    {
        int ft = g_fields[g_focus].type;
        if (ft != FT_TEXT && ft != FT_DIGITS && ft != FT_SPINNER) {
            if (ch == '1') { g_tab = TAB_TIMER;    g_focus = 0; return 0; }
            if (ch == '2') { g_tab = TAB_DATABASE; g_focus = 0; return 0; }
            if (ch == '3') { g_tab = TAB_CALC;     g_focus = 0; return 0; }
            if (ch == '4') { g_tab = TAB_CONFIG;   g_focus = 0; return 0; }
        }
    }

    /* Tab / Shift+Tab */
    if (ch == '\t') {
        g_focus = (g_focus + 1) % g_nfields;
        g_status[0] = '\0';
        return 0;
    }
    if (ch == KEY_BTAB) {
        g_focus = (g_focus - 1 + g_nfields) % g_nfields;
        g_status[0] = '\0';
        return 0;
    }

    /* Help tab: Up/Down scrolls content (no interactive fields) */
    if (g_tab == TAB_HELP) {
        if (ch == KEY_UP) {
            if (g_content_scroll[TAB_HELP] > 0) g_content_scroll[TAB_HELP]--;
            return 0;
        }
        if (ch == KEY_DOWN) {
            g_content_scroll[TAB_HELP]++;
            return 0;
        }
    }

    f = &g_fields[g_focus];

    switch (f->type) {

    case FT_TABS:
        if (ch == KEY_RIGHT) {
            g_tab = (g_tab + 1) % TAB_COUNT;
            g_focus = 0;
        } else if (ch == KEY_LEFT) {
            g_tab = (g_tab - 1 + TAB_COUNT) % TAB_COUNT;
            g_focus = 0;
        }
        break;

    case FT_TEXT:
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
            buf_backspace(f->buf);
        else if (ch >= 32 && ch < 127)
            buf_insert(f->buf, f->bufsz, ch);
        break;

    case FT_DIGITS:
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
            buf_backspace(f->buf);
        else if (ch >= '0' && ch <= '9')
            buf_insert(f->buf, f->bufsz, ch);
        break;

    case FT_SPINNER:
        if (ch == KEY_UP)
            AdjustBuf(f->buf, f->bufsz, +1, f->maxval);
        else if (ch == KEY_DOWN)
            AdjustBuf(f->buf, f->bufsz, -1, f->maxval);
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
            buf_backspace(f->buf);
        else if (ch >= '0' && ch <= '9')
            buf_insert(f->buf, f->bufsz, ch);
        break;

    case FT_BUTTON:
        if (ch == '\n' || ch == '\r' || ch == ' ')
            if (f->action) f->action(f->arg);
        break;

    case FT_LIST:
        if (f->arg != NULL) {
            /* Config DynList */
            DynList *dl = (DynList *)f->arg;
            if (ch == KEY_UP) {
                if (dl->sel > 0) {
                    dl->sel--;
                    if (dl->sel < dl->scroll)
                        dl->scroll = dl->sel;
                }
            } else if (ch == KEY_DOWN) {
                if (dl->sel < dl->count - 1) {
                    dl->sel++;
                    if (dl->sel >= dl->scroll + VISIBLE_CFG_LIST)
                        dl->scroll = dl->sel - VISIBLE_CFG_LIST + 1;
                }
            } else if (ch == 'd' && dl->count > 0) {
                DeleteOption(dl->category, dl->items[dl->sel]);
                dynlist_delete(dl, dl->sel);
                sync_dropdown_indices();
                snprintf(g_status, sizeof(g_status), "Deleted.");
            }
        } else {
            /* Preset list */
            if (ch == KEY_UP) {
                if (g_preset_sel > 0) {
                    g_preset_sel--;
                    if (g_preset_sel < g_preset_scroll)
                        g_preset_scroll = g_preset_sel;
                }
            } else if (ch == KEY_DOWN) {
                if (g_preset_sel < g_preset_count - 1) {
                    g_preset_sel++;
                    if (g_preset_sel >= g_preset_scroll + VISIBLE_PRESETS)
                        g_preset_scroll = g_preset_sel - VISIBLE_PRESETS + 1;
                }
            } else if (ch == '\n' || ch == '\r' || ch == ' ') {
                if (!AnyTimerRunning() && g_preset_sel >= 0 &&
                    g_preset_sel < g_preset_count) {
                    LoadPresetIntoTimers(g_presets[g_preset_sel].id);
                    g_preset_loaded_id = g_presets[g_preset_sel].id;
                    g_workflow_phase   = 0;
                    snprintf(g_preset_name, sizeof(g_preset_name),
                             "%s", g_presets[g_preset_sel].name);
                    snprintf(g_status, sizeof(g_status),
                             "Loaded: %s", g_presets[g_preset_sel].name);
                } else if (AnyTimerRunning()) {
                    snprintf(g_status, sizeof(g_status),
                             "Stop all timers before loading a preset.");
                }
            } else if (ch == 'd') {
                DeleteSelectedPreset();
            }
        }
        break;

    case FT_DROPDOWN: {
        DropdownMeta *dm = (DropdownMeta *)f->arg;
        int n;
        if (!dm) break;
        for (n = 0; dm->options[n]; n++);
        if (ch == KEY_UP || ch == KEY_DOWN ||
            ch == '\n' || ch == '\r' || ch == ' ') {
            open_dropdown_popup(f);
            if (ch == KEY_UP)
                g_popup_sel = (g_popup_sel - 1 + n) % n;
            else if (ch == KEY_DOWN)
                g_popup_sel = (g_popup_sel + 1) % n;
        }
        break;
    }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Signals / main                                                      */
/* ------------------------------------------------------------------ */

static void sigwinch_handler(int sig) { (void)sig; g_resize = 1; }

int
main(void)
{
    int ch, quit = 0;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGWINCH, sigwinch_handler);

    InitTimers();
    OpenDatabase();

    dynlist_init(&g_films, "film");
    dynlist_init(&g_devs,  "developer");
    dynlist_init(&g_isos,  "iso");
    dynlist_init(&g_dils,  "dilution");
    SeedOptionsIfEmpty();
    LoadOptions();

    /* Init edit_buf to first item of each list */
    if (g_films.count > 0)
        snprintf(g_films.edit_buf, sizeof(g_films.edit_buf), "%s", g_films.items[0]);
    if (g_devs.count > 0)
        snprintf(g_devs.edit_buf, sizeof(g_devs.edit_buf), "%s", g_devs.items[0]);
    if (g_isos.count > 0)
        snprintf(g_isos.edit_buf, sizeof(g_isos.edit_buf), "%s", g_isos.items[0]);
    if (g_dils.count > 0)
        snprintf(g_dils.edit_buf, sizeof(g_dils.edit_buf), "%s", g_dils.items[0]);

    /* Default indices after lists loaded */
    g_iso_nom_idx  = find_option_idx(g_isos.ptrs, "400");
    g_iso_used_idx = find_option_idx(g_isos.ptrs, "400");
    g_dil_idx      = find_option_idx(g_dils.ptrs, "Stock");

    sync_bufs_from_indices();
    LoadPresetList();

#ifdef __OpenBSD__
    if (pledge("stdio rpath wpath cpath flock tty", NULL) == -1) {
        perror("pledge");
        return 1;
    }
#endif

    setenv("NCURSES_NO_UTF8_ACS", "1", 0);
    setlocale(LC_ALL, "");
    initscr();
    init_box_chars();
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_BUTTON, COLOR_WHITE, COLOR_BLACK);
        if (can_change_color() && COLORS >= 16) {
            init_color(COLOR_BOX_BG,   12,  208, 208);
            init_color(COLOR_BORDER,  560,  420, 150);
            init_color(COLOR_APP_BG,  251,  251, 251);
            init_color(COLOR_INPUT_BG,   51,  282, 282);
            init_color(COLOR_PROG_GREEN,   0,  700, 200);
            init_pair(CP_BOX,        -1, COLOR_BOX_BG);
            init_pair(CP_BOX_LINE,   COLOR_BORDER, -1);
            init_pair(CP_BOX_BORDER, COLOR_BOX_BG, COLOR_APP_BG);
            init_pair(CP_BG,         -1, COLOR_APP_BG);
            init_pair(CP_INPUT,      COLOR_WHITE, COLOR_INPUT_BG);
            init_pair(CP_PROGRESS,   COLOR_BLACK, COLOR_PROG_GREEN);
            init_pair(CP_ALARM,      COLOR_WHITE, COLOR_RED);
        } else {
            init_pair(CP_BOX,        -1, COLOR_CYAN);
            init_pair(CP_BOX_LINE,   COLOR_YELLOW, -1);
            init_pair(CP_BOX_BORDER, COLOR_CYAN, COLOR_WHITE);
            init_pair(CP_BG,         -1, COLOR_WHITE);
            init_pair(CP_INPUT,      COLOR_YELLOW, COLOR_CYAN);
            init_pair(CP_PROGRESS,   COLOR_BLACK, COLOR_GREEN);
            init_pair(CP_ALARM,      COLOR_WHITE, COLOR_RED);
        }
        bkgd(COLOR_PAIR(CP_BG));
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);

    draw_all();

    while (!quit) {
        while ((ch = getch()) != ERR) {
            handle_key(ch);
            draw_all();
            if (g_want_quit) { quit = 1; break; }
        }
        if (quit) break;

        if (g_resize) {
            g_resize = 0;
            endwin();
            refresh();
            clear();
            draw_all();
            continue;
        }

        {
            long now  = now_ms();
            long wake = -1;
            int  ti;
            fd_set rfds;
            int    fd = fileno(stdin);
            struct timeval tv;

            for (ti = 0; ti < TIMER_COUNT; ti++) {
                CountdownTimer *t = &g_timers[ti];
                if (t->running &&
                    (wake < 0 || t->next_tick_ms < wake))
                    wake = t->next_tick_ms;
                if (t->alarm_pulses_left > 0 &&
                    (wake < 0 || t->next_alarm_pulse_ms < wake))
                    wake = t->next_alarm_pulse_ms;
            }

            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            if (wake < 0) {
                tv.tv_sec  = 0;
                tv.tv_usec = 200000;
            } else {
                long remaining = wake - now;
                if (remaining < 0) remaining = 0;
                tv.tv_sec  = remaining / 1000;
                tv.tv_usec = (remaining % 1000) * 1000;
            }
            select(fd + 1, &rfds, NULL, NULL, &tv);

            now = now_ms();
            for (ti = 0; ti < TIMER_COUNT; ti++) {
                CountdownTimer *t = &g_timers[ti];
                if (t->running && now >= t->next_tick_ms) {
                    CountdownTick(t);
                    t->next_tick_ms = now + TICK_MS;
                    draw_all();
                }
                if (t->alarm_pulses_left > 0 && now >= t->next_alarm_pulse_ms) {
                    FireAlarmPulse(t);
                    t->next_alarm_pulse_ms = now + ALARM_PULSE_MS;
                }
            }
        }
    }

    endwin();
    sqlite3_close(g_db);
    free(g_presets);
    return 0;
}
