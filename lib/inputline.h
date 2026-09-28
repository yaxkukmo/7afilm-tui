#ifndef INPUTLINE_H
#define INPUTLINE_H

/*
 * inputline.h - one-line text prompt (e.g. in a status bar).  Text is
 * UTF-8, edited at the end: Backspace, Ctrl+U clears, Enter submits,
 * Esc cancels.
 */

#define IL_TEXT_LEN 256

/* inputline_key() results */
#define IL_IGNORED 0   /* prompt not open - key not used   */
#define IL_HANDLED 1   /* key consumed                      */
#define IL_SUBMIT  2   /* Enter - text in il->text          */
#define IL_CANCEL  3   /* Esc - prompt closed               */

typedef struct {
    const char *prompt;       /* e.g. "New: "                       */
    const char *placeholder;  /* shown dimmed while empty, or NULL  */
    char        text[IL_TEXT_LEN];
    int         open;
} InputLine;

void inputline_open(InputLine *il, const char *prompt, const char *placeholder);
void inputline_close(InputLine *il);
int  inputline_key(InputLine *il, int ch);
/* Draw prompt and text in `cols` columns and put the cursor at the end */
void inputline_draw(const InputLine *il, int row, int col, int cols);

#endif /* INPUTLINE_H */
