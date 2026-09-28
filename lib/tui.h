#ifndef TUI_H
#define TUI_H

/*
 * tui.h - terminal setup, colors and drawing primitives shared by the
 * 7a curses applications.
 */

#ifndef NCURSES_WIDECHAR
#define NCURSES_WIDECHAR 1  /* get_wch() */
#endif

#include <curses.h>
#include <signal.h>

/* Keys from tui_getkey(): ASCII characters and curses KEY_* codes as
 * they are; other characters as TUI_WCHAR | code point, since code
 * points such as U+0105 (a-ogonek) overlap the KEY_* range.            */
#define TUI_WCHAR   0x40000000
#define TUI_CP(ch)  ((unsigned)((ch) & ~TUI_WCHAR))

#define CP_BUTTON     1  /* white on black - button background          */
#define CP_BOX        2  /* box interior fill: fg=default, bg=#033535   */
#define CP_BG         3  /* app background: fg=default, bg=#404040      */
#define CP_BOX_LINE   4  /* box border chars: fg=amber, bg=transparent  */
#define CP_BOX_BORDER 5  /* block border: fg=#033535, bg=#404040        */
#define CP_INPUT      6  /* input field: fg=amber, bg=#0d4848           */
#define CP_PROGRESS   7  /* progress bar: fg=black, bg=green            */
#define CP_ALARM      8  /* alarm tick: fg=black, bg=red                */

#define LABEL_W    14  /* label column width ("Temperature:" is widest) */
#define INDENT      2  /* section indent                               */

/* Box-drawing characters (ACS or ASCII fallback) */
extern chtype g_ul, g_ur, g_ll, g_lr; /* corners                     */
extern chtype g_hl, g_vl;             /* horizontal / vertical line  */
extern chtype g_lt, g_rt, g_tt, g_bt; /* T-junctions                 */

/* 8-color terminal (e.g. the wscons console): light text on black and
 * focused / selected elements shown in reverse video.                  */
extern int  g_basic_colors;

extern char g_status[128];
extern volatile sig_atomic_t g_resize;

void tui_init(void);
int  tui_getkey(void);      /* ERR when no key is waiting */
int  tui_is_text(int ch);   /* printable character, not a control key */

/* UTF-8 text clipped / padded with spaces to exactly `cols` columns,
 * in the current attributes. */
void tui_put_text(int row, int col, int cols, const char *s);

void draw_box_bottom(int row, int col, int width);
void draw_box_top_plain(int row, int col, int width);
void draw_h_separator(int row, int col, int width);
void draw_box_sides(int row, int col, int width);
void draw_popup_frame(int top, int left, int height, int width);
void draw_section_title(int row, int col, int width, const char *title);
/* Centered yes/no question box; the app handles y / n / Esc */
void draw_confirm_box(const char *question);
void draw_label(int row, const char *text);
int  field_col(void);

#endif /* TUI_H */
