#include "inputline.h"
#include "form.h"
#include "tui.h"
#include "utf8.h"

#include <string.h>

void
inputline_open(InputLine *il, const char *prompt, const char *placeholder)
{
    il->prompt      = prompt;
    il->placeholder = placeholder;
    il->text[0]     = '\0';
    il->open        = 1;
}

void
inputline_close(InputLine *il)
{
    il->open = 0;
}

int
inputline_key(InputLine *il, int ch)
{
    if (!il->open) return IL_IGNORED;

    if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
        buf_backspace(il->text);
    } else if (ch == ('u' & 0x1f)) {
        il->text[0] = '\0';
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        return IL_SUBMIT;
    } else if (ch == 27) { /* ESC */
        il->open = 0;
        return IL_CANCEL;
    } else if (tui_is_text(ch)) {
        buf_insert(il->text, sizeof(il->text), ch);
    }
    return IL_HANDLED;
}

void
inputline_draw(const InputLine *il, int row, int col, int cols)
{
    int pw = utf8_width(il->prompt);
    int fw = cols - pw;
    int tw = utf8_width(il->text);
    const char *shown = il->text;

    if (fw < 1) return;

    attron(COLOR_PAIR(CP_BOX) | A_BOLD);
    tui_put_text(row, col, pw, il->prompt);
    attroff(COLOR_PAIR(CP_BOX) | A_BOLD);

    /* Long text: show its tail so the cursor end stays visible */
    while (tw > fw - 1 && *shown) {
        unsigned cp;
        int n = utf8_decode(shown, &cp);
        tw -= utf8_cp_width(cp);
        shown += n;
    }

    if (!il->text[0] && il->placeholder) {
        attron(COLOR_PAIR(CP_INPUT) | A_DIM);
        tui_put_text(row, col + pw, fw, il->placeholder);
        attroff(COLOR_PAIR(CP_INPUT) | A_DIM);
    } else {
        attron(COLOR_PAIR(CP_INPUT));
        tui_put_text(row, col + pw, fw, shown);
        attroff(COLOR_PAIR(CP_INPUT));
    }
    move(row, col + pw + tw);
    curs_set(1);
}
