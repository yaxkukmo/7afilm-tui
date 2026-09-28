#ifndef TABBAR_H
#define TABBAR_H

/*
 * tabbar.h - the tab bar at the top of the 7a apps.  Three rows from
 * `row`: the box top, the tab names with the active tab framed, and a
 * bottom border left open under the active tab, joined to a frame
 * below with side tees.  Tab names carry their key, e.g. "F1:Timer".
 */

void tabbar_draw(int row, int cols, const char *const *names, int n, int active);

/* First column after the last tab, for extra text on the tab row */
int  tabbar_end(const char *const *names, int n);

#endif /* TABBAR_H */
