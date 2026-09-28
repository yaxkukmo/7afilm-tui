#ifndef QUICKADD_H
#define QUICKADD_H

/*
 * quickadd.h - parse a one-line "quick add" into a title, date, time
 * and priority.  Words are recognised anywhere in the line, in English
 * or Polish:
 *
 *   date      today dziś, tomorrow jutro, pojutrze, a weekday (mon /
 *             pn / poniedziałek / wtorek ...: the next one after today),
 *             +3 / +3d days, +2w weeks, 30.09 / 30.09.2026 / 2026-09-30
 *             (without a year: this year, or next once the day passed)
 *   time      15:00, 9:30, 3pm, 10:30am (a time alone means today)
 *   priority  !high !h !1 !wysoki, !normal !2, !low !l !3 !niski
 *
 * The rest is the title; "w", "we", "o", "at" and "on" right before a
 * date or time are dropped ("dentysta w piątek o 15:00").
 */

#include <stddef.h>

#include "date.h"

#define QA_TITLE_LEN 256

typedef struct {
    char title[QA_TITLE_LEN];
    int  has_date;
    Day  date;
    char time[6];        /* "HH:MM" or "" */
    int  priority;       /* 1 = high .. 3 = low, 0 = not given */
} QuickAdd;

/* Returns 1, or 0 with a message in err (e.g. "No such date: 31.02") */
int quickadd_parse(const char *s, Day today, QuickAdd *out,
                   char *err, size_t errsz);

/* Only a date and optional time ("jutro 15:00"); time gets "" when
 * none is given.  Returns 1, or 0 with a message in err. */
int quickadd_parse_when(const char *s, Day today, Day *date, char time[6],
                        char *err, size_t errsz);

/* A time alone ("15:00", "3pm"): 1 = ok, 0 = not a time, -1 = invalid */
int quickadd_parse_time(const char *s, char out[6]);

#endif /* QUICKADD_H */
