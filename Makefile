SQLITE_CFLAGS != pkg-config --cflags sqlite3 2>/dev/null || echo "-I/usr/local/include"
SQLITE_LIBS   != pkg-config --libs   sqlite3 2>/dev/null || echo "-L/usr/local/lib -lsqlite3"

# Wide-character ncurses is needed for box drawing in UTF-8 locales
# (on Linux plain -lcurses is the narrow library and shows no frames).
CURSES_CFLAGS != pkg-config --cflags ncursesw 2>/dev/null || true
CURSES_LIBS   != pkg-config --libs   ncursesw 2>/dev/null || echo "-lcurses"

CC      = cc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -Ilib $(SQLITE_CFLAGS) $(CURSES_CFLAGS)
LDFLAGS = $(SQLITE_LIBS) $(CURSES_LIBS) -lm

# Code shared by the 7a TUI apps
LIB     = lib/lib7a.a
LIBOBJS = lib/utf8.o lib/tui.o lib/form.o lib/listpopup.o lib/db.o \
          lib/dynlist.o lib/inputline.o

FILMOBJS = film/timer.o film/7afilm-tui.o

ORGOBJS  = organizer/date.o organizer/store.o organizer/quickadd.o organizer/edit.o

all: 7afilm-tui 7aorganizer-tui

$(LIB): $(LIBOBJS)
	ar rcs $@ $(LIBOBJS)

7afilm-tui: $(FILMOBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ $(FILMOBJS) $(LIB) $(LDFLAGS)

7aorganizer-tui: $(ORGOBJS) organizer/7aorganizer-tui.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(ORGOBJS) organizer/7aorganizer-tui.o $(LIB) $(LDFLAGS)

# lib/

lib/utf8.o: lib/utf8.c lib/utf8.h
	$(CC) $(CFLAGS) -c lib/utf8.c -o $@

lib/tui.o: lib/tui.c lib/tui.h lib/utf8.h
	$(CC) $(CFLAGS) -c lib/tui.c -o $@

lib/form.o: lib/form.c lib/form.h lib/tui.h lib/utf8.h
	$(CC) $(CFLAGS) -c lib/form.c -o $@

lib/listpopup.o: lib/listpopup.c lib/listpopup.h lib/form.h lib/tui.h lib/utf8.h
	$(CC) $(CFLAGS) -c lib/listpopup.c -o $@

lib/db.o: lib/db.c lib/db.h
	$(CC) $(CFLAGS) -c lib/db.c -o $@

lib/dynlist.o: lib/dynlist.c lib/dynlist.h
	$(CC) $(CFLAGS) -c lib/dynlist.c -o $@

lib/inputline.o: lib/inputline.c lib/inputline.h lib/form.h lib/tui.h lib/utf8.h
	$(CC) $(CFLAGS) -c lib/inputline.c -o $@

# organizer/

organizer/date.o: organizer/date.c organizer/date.h
	$(CC) $(CFLAGS) -c organizer/date.c -o $@

organizer/store.o: organizer/store.c organizer/store.h organizer/date.h lib/db.h
	$(CC) $(CFLAGS) -c organizer/store.c -o $@

organizer/quickadd.o: organizer/quickadd.c organizer/quickadd.h organizer/date.h lib/utf8.h
	$(CC) $(CFLAGS) -c organizer/quickadd.c -o $@

organizer/edit.o: organizer/edit.c organizer/edit.h organizer/date.h organizer/quickadd.h \
                  organizer/store.h lib/form.h lib/tui.h lib/utf8.h
	$(CC) $(CFLAGS) -c organizer/edit.c -o $@

organizer/7aorganizer-tui.o: organizer/7aorganizer-tui.c organizer/date.h organizer/edit.h \
                            organizer/quickadd.h organizer/store.h lib/inputline.h \
                            lib/db.h lib/tui.h lib/utf8.h
	$(CC) $(CFLAGS) -c organizer/7aorganizer-tui.c -o $@

organizer/test_store: organizer/test_store.c organizer/quickadd.h $(ORGOBJS) $(LIB)
	$(CC) $(CFLAGS) -o $@ organizer/test_store.c $(ORGOBJS) $(LIB) $(LDFLAGS)

check: organizer/test_store
	./organizer/test_store organizer/testdata/poc.db

# film/

film/timer.o: film/timer.c film/timer.h lib/tui.h
	$(CC) $(CFLAGS) -c film/timer.c -o $@

film/7afilm-tui.o: film/7afilm-tui.c film/timer.h lib/db.h lib/dynlist.h \
                   lib/form.h lib/listpopup.h lib/tui.h
	$(CC) $(CFLAGS) -c film/7afilm-tui.c -o $@

clean:
	rm -f 7afilm-tui 7aorganizer-tui $(FILMOBJS) $(ORGOBJS) \
	      organizer/7aorganizer-tui.o organizer/test_store \
	      $(LIBOBJS) $(LIB)

.PHONY: all check clean
