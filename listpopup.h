#ifndef LISTPOPUP_H
#define LISTPOPUP_H

/*
 * listpopup.h - centered popup with a search field over a filtered list.
 * Items come from the app through callbacks; matching is a
 * case-insensitive substring search.
 */

#define LP_QUERY_LEN     64
#define LP_MAX_MATCHES  512
#define LP_VISIBLE        8

/* listpopup_key() results */
#define LP_IGNORED   0  /* popup not open - key not used               */
#define LP_HANDLED   1  /* key consumed, nothing for the app to do      */
#define LP_PICKED    2  /* Enter on a row - see listpopup_selected()   */
#define LP_CANCELLED 3  /* Esc - popup closed                          */

typedef struct {
    /* Set by the app */
    const char  *prompt;   /* e.g. " Search: "                          */
    const char  *hint;     /* bottom row, e.g. " Enter=load  Esc=cancel" */
    const char  *extra;    /* optional first row (e.g. "new"), or NULL  */
    int        (*count)(void *ctx);
    const char *(*item)(void *ctx, int i);
    void        *ctx;

    /* Internal state */
    int  open;
    char query[LP_QUERY_LEN];
    int  sel, scroll;
    int  matches[LP_MAX_MATCHES];
    int  nmatches;
} ListPopup;

void listpopup_open(ListPopup *lp);
void listpopup_close(ListPopup *lp);
void listpopup_draw(ListPopup *lp);
int  listpopup_key(ListPopup *lp, int ch);
/* Item index of the selected row, or -1 for the extra row */
int  listpopup_selected(const ListPopup *lp);

#endif /* LISTPOPUP_H */
