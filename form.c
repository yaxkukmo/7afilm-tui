#include "form.h"
#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Field g_fields[MAX_FIELDS];
int   g_nfields     = 0;
int   g_focus       = 0;
int   g_focus_stale = 0;

static DropdownMeta g_dd_store[MAX_DROPDOWNS];
static int          g_dd_count = 0;

/* Popup (open dropdown) state */
static int           g_popup_open   = 0;
static DropdownMeta *g_popup_dm     = NULL;
static int           g_popup_frow   = 0;
static int           g_popup_fcol   = 0;
static int           g_popup_fwidth = 0;
static int           g_popup_sel    = 0;
static int           g_popup_scroll = 0;

/* ------------------------------------------------------------------ */
/* Field registry                                                      */
/* ------------------------------------------------------------------ */

void fields_reset(void) { g_nfields = 0; g_dd_count = 0; }

int
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
/* Field widgets                                                       */
/* ------------------------------------------------------------------ */

int
draw_textfield(int row, int col, int width,
               char *buf, size_t bufsz, int type, int maxval)
{
    int idx     = field_reg(type, buf, bufsz, maxval, NULL, NULL, row, col, width);
    int focused = (idx >= 0 && idx == g_focus);
    int len     = (int)strlen(buf);
    int i;

    if (focused)
        attron(COLOR_PAIR(CP_INPUT) | A_BOLD | (g_basic_colors ? A_REVERSE : 0));
    else
        attron(COLOR_PAIR(CP_INPUT));
    move(row, col);
    for (i = 0; i < width; i++)
        addch(i < len ? (unsigned char)buf[i] : ' ');
    if (focused)
        attroff(COLOR_PAIR(CP_INPUT) | A_BOLD | A_REVERSE);
    else
        attroff(COLOR_PAIR(CP_INPUT));
    return idx;
}

int
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

void
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

int
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
        attron(COLOR_PAIR(CP_INPUT) | A_BOLD | (g_basic_colors ? A_REVERSE : 0));
    else
        attron(COLOR_PAIR(CP_INPUT));
    move(row, col);
    for (i = 0; i < width - 2; i++)
        addch(i < len ? (unsigned char)val[i] : ' ');
    addch(' ');
    addch(ACS_DARROW);
    if (focused)
        attroff(COLOR_PAIR(CP_INPUT) | A_BOLD | A_REVERSE);
    else
        attroff(COLOR_PAIR(CP_INPUT));
    return idx;
}

/* ------------------------------------------------------------------ */
/* Dropdown popup                                                      */
/* ------------------------------------------------------------------ */

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

static void
confirm_popup(void)
{
    DropdownMeta *dm = g_popup_dm;
    *dm->index = g_popup_sel;
    snprintf(dm->buf, dm->bufsz, "%s", dm->options[g_popup_sel]);
    g_popup_open = 0;
    g_popup_dm   = NULL;
    if (dm->on_confirm) dm->on_confirm(g_popup_sel);
}

void
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

/* Cursor on the focused text field, hidden elsewhere */
void
form_place_cursor(void)
{
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
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

int
buf_insert(char *buf, size_t bufsz, int ch)
{
    int len = (int)strlen(buf);
    if (len + 1 >= (int)bufsz) return 0;
    buf[len]     = (char)ch;
    buf[len + 1] = '\0';
    return 1;
}

int
buf_backspace(char *buf)
{
    int len = (int)strlen(buf);
    if (len == 0) return 0;
    buf[len - 1] = '\0';
    return 1;
}

static void
adjust_buf(char *buf, size_t bufsz, int delta, int maxval)
{
    long val = strtol(buf, NULL, 10) + delta;
    if (val < 0)      val = maxval;
    if (val > maxval) val = 0;
    snprintf(buf, bufsz, "%02ld", val);
}

/* Focus the nearest field in the row above (dir < 0) or below (dir > 0).
 * Returns 0 when there is no field in that direction. */
static int
focus_vertical(int dir)
{
    Field *cur = &g_fields[g_focus];
    int cx     = cur->col + cur->width / 2;
    int best   = -1, best_row = 0, best_dx = 0;
    int i;

    for (i = 0; i < g_nfields; i++) {
        Field *f = &g_fields[i];
        int dx;
        if (dir < 0 ? f->row >= cur->row : f->row <= cur->row)
            continue;
        dx = f->col + f->width / 2 - cx;
        if (dx < 0) dx = -dx;
        if (best < 0 ||
            (dir < 0 ? f->row > best_row : f->row < best_row) ||
            (f->row == best_row && dx < best_dx)) {
            best     = i;
            best_row = f->row;
            best_dx  = dx;
        }
    }
    if (best < 0) return 0;
    g_focus     = best;
    g_status[0] = '\0';
    return 1;
}

/* Keys for an open dropdown popup.  Returns 1 when the popup is open
 * (it takes every key), 0 otherwise. */
int
form_popup_key(int ch)
{
    int n;

    if (!g_popup_open || !g_popup_dm) return 0;

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
    return 1;
}

/* Field navigation and editing.  Left/Right on an FT_TABS field are left
 * to the app, which must handle them before calling this. */
void
form_field_key(int ch)
{
    Field *f;

    if (g_nfields == 0) return;

    /* Tab / Shift+Tab */
    if (ch == '\t') {
        g_focus = (g_focus + 1) % g_nfields;
        g_status[0] = '\0';
        return;
    }
    if (ch == KEY_BTAB) {
        g_focus = (g_focus - 1 + g_nfields) % g_nfields;
        g_status[0] = '\0';
        return;
    }

    f = &g_fields[g_focus];

    /* Arrow navigation between fields.  Left/Right = previous/next field
     * (except on the tab bar, where they switch tabs).  Up/Down move to the
     * row above/below, unless the field uses them itself (spinner,
     * dropdown). */
    if (f->type != FT_TABS && (ch == KEY_LEFT || ch == KEY_RIGHT)) {
        g_focus = (g_focus + (ch == KEY_RIGHT ? 1 : -1) + g_nfields) % g_nfields;
        g_status[0] = '\0';
        return;
    }
    if ((ch == KEY_UP || ch == KEY_DOWN) &&
        (f->type == FT_TEXT || f->type == FT_DIGITS ||
         f->type == FT_BUTTON || f->type == FT_TABS)) {
        focus_vertical(ch == KEY_UP ? -1 : +1);
        return;
    }

    switch (f->type) {

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
            adjust_buf(f->buf, f->bufsz, +1, f->maxval);
        else if (ch == KEY_DOWN)
            adjust_buf(f->buf, f->bufsz, -1, f->maxval);
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
            buf_backspace(f->buf);
        else if (ch >= '0' && ch <= '9')
            buf_insert(f->buf, f->bufsz, ch);
        break;

    case FT_BUTTON:
        if (ch == '\n' || ch == '\r' || ch == ' ')
            if (f->action) f->action(f->arg);
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
}
