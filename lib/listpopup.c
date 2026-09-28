#include "listpopup.h"
#include "form.h"
#include "tui.h"
#include "utf8.h"

#include <string.h>

static int
lp_total(const ListPopup *lp)
{
    return lp->nmatches + (lp->extra ? 1 : 0);
}

static void
rebuild_matches(ListPopup *lp)
{
    int n = lp->count(lp->ctx);
    int i, total;

    lp->nmatches = 0;
    for (i = 0; i < n && lp->nmatches < LP_MAX_MATCHES; i++)
        if (utf8_contains_ci(lp->item(lp->ctx, i), lp->query) ||
            (lp->more && utf8_contains_ci(lp->more(lp->ctx, i), lp->query)))
            lp->matches[lp->nmatches++] = i;

    total = lp_total(lp);
    if (lp->sel >= total)
        lp->sel = total > 0 ? total - 1 : 0;
    if (lp->sel < 0)
        lp->sel = 0;
}

void
listpopup_open(ListPopup *lp)
{
    lp->query[0] = '\0';
    lp->sel      = 0;
    lp->scroll   = 0;
    rebuild_matches(lp);
    lp->open     = 1;
}

void
listpopup_refresh(ListPopup *lp)
{
    rebuild_matches(lp);
}

void
listpopup_close(ListPopup *lp)
{
    lp->open = 0;
}

int
listpopup_selected(const ListPopup *lp)
{
    int i = lp->sel - (lp->extra ? 1 : 0);
    if (i < 0 || i >= lp->nmatches) return -1;
    return lp->matches[i];
}

void
listpopup_draw(ListPopup *lp)
{
    int rows, cols, pw, pr, pc, fc, fw, i, qw, r, total;

    if (!lp->open) return;

    rows = getmaxy(stdscr);
    cols = getmaxx(stdscr);

    pw = 54;
    if (pw > cols - 2) pw = cols - 2;
    if (pw < 22) pw = 22;
    fw = pw - 3 - (int)strlen(lp->prompt);   /* search field width */

    /* height: top + search + sep + LP_VISIBLE list rows + hint + bottom */
    pr = (rows - (LP_VISIBLE + 5)) / 2;
    if (pr < 0) pr = 0;
    pc = (cols - pw) / 2;
    if (pc < 0) pc = 0;
    fc = pc + 1 + (int)strlen(lp->prompt);

    /* scroll adjustment */
    if (lp->sel < lp->scroll)
        lp->scroll = lp->sel;
    if (lp->sel >= lp->scroll + LP_VISIBLE)
        lp->scroll = lp->sel - LP_VISIBLE + 1;

    draw_popup_frame(pr, pc, LP_VISIBLE + 5, pw);
    r = pr + 1;

    /* Search input row */
    qw = utf8_width(lp->query);
    mvprintw(r, pc + 1, "%s", lp->prompt);
    attron(A_REVERSE | A_BOLD);
    tui_put_text(r, fc, fw, lp->query);
    attroff(A_REVERSE | A_BOLD);
    r++;

    /* Separator */
    mvaddch(r, pc, g_lt);
    mvhline(r, pc + 1, g_hl, pw - 2);
    mvaddch(r, pc + pw - 1, g_rt);
    r++;

    /* List rows */
    total = lp_total(lp);
    for (i = 0; i < LP_VISIBLE; i++) {
        int row_idx = lp->scroll + i;
        if (row_idx < total) {
            int is_sel = (row_idx == lp->sel);
            int k      = row_idx - (lp->extra ? 1 : 0);
            const char *label = k < 0 ? lp->extra
                                      : lp->item(lp->ctx, lp->matches[k]);
            if (is_sel) attron(A_REVERSE | A_BOLD);
            move(r, pc + 1);
            if (i == 0 && lp->scroll > 0)
                addch(ACS_UARROW);
            else if (i == LP_VISIBLE - 1 && lp->scroll + LP_VISIBLE < total)
                addch(ACS_DARROW);
            else
                addch(' ');
            tui_put_text(r, pc + 2, pw - 3, label);
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

    /* Hint row */
    attron(A_DIM);
    {
        int hlen = (int)strlen(lp->hint);
        int j;
        mvprintw(r, pc + 1, "%s", lp->hint);
        for (j = hlen + 1; j < pw - 1; j++) mvaddch(r, pc + j, ' ');
    }
    attroff(A_DIM);

    /* Cursor in search field */
    {
        int cpos = qw < fw ? qw : fw - 1;
        move(pr + 1, fc + cpos);
        curs_set(1);
    }
}

int
listpopup_key(ListPopup *lp, int ch)
{
    int total;

    if (!lp->open) return LP_IGNORED;

    total = lp_total(lp);
    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        buf_backspace(lp->query);
        rebuild_matches(lp);
    } else if (tui_is_text(ch)) {
        buf_insert(lp->query, sizeof(lp->query), ch);
        rebuild_matches(lp);
    } else if (ch == KEY_UP) {
        if (lp->sel > 0) {
            lp->sel--;
            if (lp->sel < lp->scroll)
                lp->scroll = lp->sel;
        }
    } else if (ch == KEY_DOWN) {
        if (lp->sel < total - 1) {
            lp->sel++;
            if (lp->sel >= lp->scroll + LP_VISIBLE)
                lp->scroll = lp->sel - LP_VISIBLE + 1;
        }
    } else if (ch == '\n' || ch == '\r') {
        if (total > 0)
            return LP_PICKED;
    } else if (ch == 27) { /* ESC */
        lp->open = 0;
        return LP_CANCELLED;
    }
    return LP_HANDLED;
}
