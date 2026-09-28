#include "tabbar.h"
#include "tui.h"
#include "utf8.h"

/* Tab t starts at the returned column and is width(name) + 2 wide */
static int
tab_col(const char *const *names, int t)
{
    int i, col = INDENT;
    for (i = 0; i < t; i++)
        col += utf8_width(names[i]) + 3;
    return col;
}

int
tabbar_end(const char *const *names, int n)
{
    return tab_col(names, n);
}

void
tabbar_draw(int row, int cols, const char *const *names, int n, int active)
{
    int gap_s = tab_col(names, active);                  /* inclusive */
    int gap_e = gap_s + utf8_width(names[active]) + 1;   /* inclusive */
    int t, i;

    /* Box top */
    draw_box_top_plain(row, 0, cols);

    /* Tab row: border, fill, tabs */
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row + 1, 0, g_vl);
    mvaddch(row + 1, cols - 1, g_vl);
    attroff(COLOR_PAIR(CP_BOX_LINE));
    attron(COLOR_PAIR(CP_BOX));
    mvhline(row + 1, 1, ' ', cols - 2);
    attroff(COLOR_PAIR(CP_BOX));

    for (t = 0; t < n; t++) {
        int col = tab_col(names, t);
        int w   = utf8_width(names[t]) + 2;

        if (col + w >= cols - 1) break;
        if (t == active) {
            attron(COLOR_PAIR(CP_BOX_LINE));
            mvaddch(row + 1, col,         g_vl);
            mvaddch(row + 1, col + w - 1, g_vl);
            mvaddch(row,     col,         g_tt);
            mvaddch(row,     col + w - 1, g_tt);
            attroff(COLOR_PAIR(CP_BOX_LINE));
            attron(A_BOLD | COLOR_PAIR(CP_BOX) | (g_basic_colors ? A_REVERSE : 0));
            mvprintw(row + 1, col + 1, "%s", names[t]);
            attroff(A_BOLD | COLOR_PAIR(CP_BOX) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(CP_BOX));
            mvprintw(row + 1, col, " %s ", names[t]);
            attroff(COLOR_PAIR(CP_BOX));
        }
    }

    /* Bottom border, open under the active tab */
    attron(COLOR_PAIR(CP_BOX_LINE));
    mvaddch(row + 2, 0, g_lt);
    for (i = 1; i < cols - 1; i++) {
        if (i == gap_s) {
            mvaddch(row + 2, i, g_lr);
        } else if (i > gap_s && i < gap_e) {
            attroff(COLOR_PAIR(CP_BOX_LINE));
            attron(COLOR_PAIR(CP_BOX));
            mvaddch(row + 2, i, ' ');
            attroff(COLOR_PAIR(CP_BOX));
            attron(COLOR_PAIR(CP_BOX_LINE));
        } else if (i == gap_e) {
            mvaddch(row + 2, i, g_ll);
        } else {
            mvaddch(row + 2, i, g_hl);
        }
    }
    mvaddch(row + 2, cols - 1, g_rt);
    attroff(COLOR_PAIR(CP_BOX_LINE));
}
