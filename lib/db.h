#ifndef DB_H
#define DB_H

/*
 * db.h - SQLite helpers shared by the 7a applications.  Databases live
 * in ~/.7a/.
 */

#include <sqlite3.h>

/* Open (creating if needed) ~/.7a/<file> with WAL, busy timeout and
 * foreign keys enabled.  Prints an error and exits on failure. */
sqlite3    *db_open(const char *progname, const char *file);

int         db_table_exists(sqlite3 *db, const char *name);
int         db_column_exists(sqlite3 *db, const char *table, const char *col);

/* Column text, "" for NULL */
const char *db_col_str(sqlite3_stmt *s, int c);
/* Bind an id, or NULL when id < 0 */
void        db_bind_id(sqlite3_stmt *s, int param, sqlite3_int64 id);

#endif /* DB_H */
