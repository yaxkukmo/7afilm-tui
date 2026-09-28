#ifndef EDIT_H
#define EDIT_H

/*
 * edit.h - centered edit form for a calendar entry or a todo.
 * Tab / arrows move between fields, Enter in a text field or the Save
 * button (or Ctrl+S) saves, Esc or Cancel closes without saving.
 * Descriptions are edited as a single line.
 */

#include <sqlite3.h>

/* edit_key() results */
#define EDIT_IGNORED   0   /* form not open                */
#define EDIT_HANDLED   1
#define EDIT_SAVED     2   /* saved and closed             */
#define EDIT_CANCELLED 3   /* closed without saving        */

int  edit_open_entry(sqlite3 *db, sqlite3_int64 id);   /* 0, -1 if not found */
int  edit_open_todo(sqlite3 *db, sqlite3_int64 id);
int  edit_is_open(void);
void edit_draw(void);
int  edit_key(int ch);

#endif /* EDIT_H */
