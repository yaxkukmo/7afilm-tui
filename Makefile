SQLITE_CFLAGS != pkg-config --cflags sqlite3 2>/dev/null || echo "-I/usr/local/include"
SQLITE_LIBS   != pkg-config --libs   sqlite3 2>/dev/null || echo "-L/usr/local/lib -lsqlite3"

# Wide-character ncurses is needed for box drawing in UTF-8 locales
# (on Linux plain -lcurses is the narrow library and shows no frames).
CURSES_CFLAGS != pkg-config --cflags ncursesw 2>/dev/null || true
CURSES_LIBS   != pkg-config --libs   ncursesw 2>/dev/null || echo "-lcurses"

CC      = cc
CFLAGS  = -std=c99 -Wall -Wextra -O2 $(SQLITE_CFLAGS) $(CURSES_CFLAGS)
LDFLAGS = $(SQLITE_LIBS) $(CURSES_LIBS) -lm

OBJS = tui.o form.o listpopup.o db.o dynlist.o timer.o 7afilm-tui.o

all: 7afilm-tui

7afilm-tui: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

tui.o: tui.c tui.h
	$(CC) $(CFLAGS) -c tui.c

form.o: form.c form.h tui.h
	$(CC) $(CFLAGS) -c form.c

listpopup.o: listpopup.c listpopup.h form.h tui.h
	$(CC) $(CFLAGS) -c listpopup.c

db.o: db.c db.h
	$(CC) $(CFLAGS) -c db.c

dynlist.o: dynlist.c dynlist.h
	$(CC) $(CFLAGS) -c dynlist.c

timer.o: timer.c timer.h tui.h
	$(CC) $(CFLAGS) -c timer.c

7afilm-tui.o: 7afilm-tui.c db.h dynlist.h form.h listpopup.h timer.h tui.h
	$(CC) $(CFLAGS) -c 7afilm-tui.c

clean:
	rm -f 7afilm-tui $(OBJS)

.PHONY: all clean
