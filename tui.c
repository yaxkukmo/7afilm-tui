#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "tui.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COLOR_BOX_BG     8  /* custom color #033535                    */
#define COLOR_APP_BG     9  /* custom color #404040                    */
#define COLOR_BORDER    10  /* custom color amber safelight ~#E68000   */
#define COLOR_INPUT_BG  11  /* custom color #0d4848 - lighter teal     */
#define COLOR_PROG_GREEN 12 /* custom green for progress bar           */

chtype g_ul, g_ur, g_ll, g_lr;
chtype g_hl, g_vl;
chtype g_lt, g_rt, g_tt;

int  g_basic_colors = 0;
char g_status[128]  = "";
volatile sig_atomic_t g_resize = 0;

static void sigwinch_handler(int sig) { (void)sig; g_resize = 1; }

static void
init_box_chars(void)
{
    /* The OpenBSD wscons console (/dev/ttyC*) understands DEC Special
     * Graphics, but the built-in framebuffer fonts have no box-drawing
     * glyphs and show '?' instead, so default to ASCII there.
     * FORCE_ACS enables ACS on the console (e.g. with a loaded font);
     * NO_ACS forces ASCII everywhere.  Elsewhere ncurses falls back to
     * ASCII by itself when the terminal has no acsc capability.       */
    char *tty   = ttyname(STDIN_FILENO);
    int   ascii = (getenv("NO_ACS") != NULL) ||
                  (tty && strncmp(tty, "/dev/ttyC", 9) == 0 &&
                   getenv("FORCE_ACS") == NULL);
    if (ascii) {
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

static void
init_colors(void)
{
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
        /* 8-color terminals (e.g. the wscons console): light text
         * on black everywhere, with explicit colors - the default
         * ones are unknown.  Input fields are yellow; the focused
         * one is drawn in reverse video.                           */
        g_basic_colors = 1;
        init_pair(CP_BOX,        COLOR_WHITE, COLOR_BLACK);
        init_pair(CP_BOX_LINE,   COLOR_YELLOW, COLOR_BLACK);
        init_pair(CP_BOX_BORDER, COLOR_WHITE, COLOR_BLACK);
        init_pair(CP_BG,         COLOR_WHITE, COLOR_BLACK);
        init_pair(CP_INPUT,      COLOR_YELLOW, COLOR_BLACK);
        init_pair(CP_PROGRESS,   COLOR_BLACK, COLOR_GREEN);
        init_pair(CP_ALARM,      COLOR_WHITE, COLOR_RED);
    }
    bkgd(COLOR_PAIR(CP_BG));
}

void
tui_init(void)
{
    signal(SIGWINCH, sigwinch_handler);

    setenv("NCURSES_NO_UTF8_ACS", "1", 0);
    /* ESC closes popups / leaves fields; don't wait the default 1 s
     * for a possible escape sequence.                                */
    setenv("ESCDELAY", "25", 0);
    setlocale(LC_ALL, "");
    {
        /* The wscons console (/dev/ttyC*) does not decode UTF-8: in a
         * UTF-8 locale ncurses would send Unicode box characters that
         * show up as garbage.  Use the C ctype there so ncurses emits
         * DEC Special Graphics, which wscons does support.            */
        char *tty = ttyname(STDIN_FILENO);
        if (tty && strncmp(tty, "/dev/ttyC", 9) == 0)
            setlocale(LC_CTYPE, "C");
    }
    initscr();
    init_box_chars();
    if (has_colors())
        init_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(1);
}

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                  */
/* ------------------------------------------------------------------ */

void
draw_box_bottom(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_ll);
    mvhline(row, col + 1, g_hl, width - 2);
    mvaddch(row, col + width - 1, g_lr);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

void
draw_box_top_plain(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_ul);
    mvhline(row, col + 1, g_hl, width - 2);
    mvaddch(row, col + width - 1, g_ur);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

void
draw_h_separator(int row, int col, int width)
{
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row, col, g_lt);
    mvhline(row, col + 1, g_hl, width - 2);
    mvaddch(row, col + width - 1, g_rt);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}

void
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
void
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
void
draw_section_title(int row, int col, int width, const char *title)
{
    draw_box_sides(row, col, width);
    attron(A_BOLD | COLOR_PAIR(CP_BOX));
    mvprintw(row, col + INDENT, "  %s", title);
    attroff(A_BOLD | COLOR_PAIR(CP_BOX));
}

void
draw_label(int row, const char *text)
{
    int tw  = (int)strlen(text);
    int col = INDENT + LABEL_W - tw;
    if (col < INDENT) col = INDENT;
    attron(COLOR_PAIR(CP_BOX));
    mvprintw(row, col, "%s", text);
    attroff(COLOR_PAIR(CP_BOX));
}

int field_col(void) { return INDENT + LABEL_W + 1; }
