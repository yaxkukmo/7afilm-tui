#ifndef EDIT_H
#define EDIT_H

/*
 * edit.h - centered form to add or edit a calendar entry or a todo.
 * Tab / arrows move between fields, Enter in a text field or the Save
 * button (or Ctrl+S) saves, Esc or Cancel closes without saving.
 * Descriptions are edited as a single line.
 */

#include <sqlite3.h>

#include "date.h"

/* edit_key() results */
#define EDIT_IGNORED   0   /* form not open                */
#define EDIT_HANDLED   1
#define EDIT_SAVED     2   /* saved and closed             */
#define EDIT_CANCELLED 3   /* closed without saving        */

/* Empty form for a new item: a todo, or an entry on `date`; a Type
 * field switches between the two */
int  edit_new(sqlite3 *db, int todo, Day date);
/* After EDIT_SAVED from edit_new(): what was stored; date is the
 * entry's date, or the day passed to edit_new() for other kinds */
void edit_saved_item(int *is_todo, sqlite3_int64 *id, Day *date);

int  edit_open_entry(sqlite3 *db, sqlite3_int64 id);   /* 0, -1 if not found */
int  edit_open_todo(sqlite3 *db, sqlite3_int64 id);
int  edit_is_open(void);
void edit_draw(void);
int  edit_key(int ch);

#endif /* EDIT_H */
