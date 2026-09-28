SQLITE_CFLAGS != pkg-config --cflags sqlite3 2>/dev/null || echo "-I/usr/local/include"
SQLITE_LIBS   != pkg-config --libs   sqlite3 2>/dev/null || echo "-L/usr/local/lib -lsqlite3"

# Wide-character ncurses is needed for box drawing in UTF-8 locales
# (on Linux plain -lcurses is the narrow library and shows no frames).
CURSES_CFLAGS != pkg-config --cflags ncursesw 2>/dev/null || true
CURSES_LIBS   != pkg-config --libs   ncursesw 2>/dev/null || echo "-lcurses"

CC      = cc
CFLAGS  = -std=c99 -Wall -Wextra -O2 $(SQLITE_CFLAGS) $(CURSES_CFLAGS)
LDFLAGS = $(SQLITE_LIBS) $(CURSES_LIBS) -lm

OBJS = dynlist.o timer.o 7afilm-tui.o

all: 7afilm-tui

7afilm-tui: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

dynlist.o: dynlist.c dynlist.h
	$(CC) $(CFLAGS) -c dynlist.c

timer.o: timer.c timer.h
	$(CC) $(CFLAGS) -c timer.c

7afilm-tui.o: 7afilm-tui.c dynlist.h timer.h
	$(CC) $(CFLAGS) -c 7afilm-tui.c

clean:
	rm -f 7afilm-tui $(OBJS)

.PHONY: all clean
