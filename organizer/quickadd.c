#include "quickadd.h"
#include "utf8.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 128

#define T_DROP  (-1)  /* connector word before a date / time */
#define T_WORD  0
#define T_DATE  1
#define T_TIME  2
#define T_PRIO  3

static const struct { const char *word; int weekday; } weekdays[] = {
    { "mon", 1 }, { "monday", 1 }, { "pn", 1 }, { "pon", 1 },
    { "poniedziałek", 1 }, { "poniedzialek", 1 },
    { "tue", 2 }, { "tuesday", 2 }, { "wt", 2 }, { "wto", 2 }, { "wtorek", 2 },
    { "wed", 3 }, { "wednesday", 3 }, { "śr", 3 }, { "sr", 3 },
    { "środa", 3 }, { "sroda", 3 }, { "środę", 3 }, { "srode", 3 },
    { "thu", 4 }, { "thursday", 4 }, { "czw", 4 }, { "czwartek", 4 },
    { "fri", 5 }, { "friday", 5 }, { "pt", 5 }, { "pią", 5 }, { "pia", 5 },
    { "piątek", 5 }, { "piatek", 5 },
    { "sat", 6 }, { "saturday", 6 }, { "sob", 6 }, { "sobota", 6 },
    { "sobotę", 6 }, { "sobote", 6 },
    { "sun", 7 }, { "sunday", 7 }, { "nd", 7 }, { "ndz", 7 },
    { "niedziela", 7 }, { "niedzielę", 7 }, { "niedziele", 7 },
};

static const struct { const char *word; int offset; } relative_days[] = {
    { "today", 0 }, { "dziś", 0 }, { "dzis", 0 }, { "dzisiaj", 0 },
    { "tomorrow", 1 }, { "jutro", 1 }, { "pojutrze", 2 },
};

static const struct { const char *word; int priority; } priorities[] = {
    { "high", 1 }, { "h", 1 }, { "1", 1 }, { "wysoki", 1 },
    { "normal", 2 }, { "n", 2 }, { "2", 2 }, { "normalny", 2 },
    { "low", 3 }, { "l", 3 }, { "3", 3 }, { "niski", 3 },
};

static const char *const connectors[] = { "w", "we", "o", "at", "on" };

#define COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static int
all_digits(const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* 1 = a time, 0 = not a time, -1 = looks like a time but is invalid */
static int
parse_time(const char *s, char out[6])
{
    const char *p = s;
    unsigned h = 0, m = 0;
    int nd = 0, colon = 0, suffix = 0; /* 1 = am, 2 = pm */

    while (isdigit((unsigned char)*p) && nd < 3) {
        h = h * 10 + (unsigned)(*p - '0');
        p++;
        nd++;
    }
    if (nd == 0 || nd > 2) return 0;
    if (*p == ':') {
        if (!all_digits(p + 1, 2)) return 0;
        m = (unsigned)(p[1] - '0') * 10 + (unsigned)(p[2] - '0');
        p += 3;
        colon = 1;
    }
    if (tolower((unsigned char)p[0]) == 'a' || tolower((unsigned char)p[0]) == 'p') {
        if (tolower((unsigned char)p[1]) != 'm') return 0;
        suffix = tolower((unsigned char)p[0]) == 'a' ? 1 : 2;
        p += 2;
    }
    if (*p != '\0' || (!colon && !suffix)) return 0;

    if (suffix) {
        if (h < 1 || h > 12) return -1;
        h = h % 12 + (suffix == 2 ? 12 : 0);
    } else if (h > 23) {
        return -1;
    }
    if (m > 59) return -1;
    snprintf(out, 6, "%02u:%02u", h, m);
    return 1;
}

/* D.M without a year: the first valid date from today on */
static int
next_day_month(int d, int m, Day today, Day *out)
{
    int ty, tm, td, y;

    day_to_ymd(today, &ty, &tm, &td);
    for (y = ty; y <= ty + 8; y++) {          /* 29.02 may need a few years */
        if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) continue;
        if (day_from_ymd(y, m, d) >= today) {
            *out = day_from_ymd(y, m, d);
            return 1;
        }
    }
    return -1;
}

/* 1 = a date, 0 = not a date, -1 = looks like a date but is invalid */
static int
parse_date(const char *s, Day today, Day *out)
{
    int i;

    for (i = 0; i < COUNT(relative_days); i++)
        if (utf8_eq_ci(s, relative_days[i].word)) {
            *out = today + relative_days[i].offset;
            return 1;
        }
    for (i = 0; i < COUNT(weekdays); i++)
        if (utf8_eq_ci(s, weekdays[i].word)) {
            Day d = today + 1;
            while (day_weekday(d) != weekdays[i].weekday) d++;
            *out = d;
            return 1;
        }

    /* +N, +Nd, +Nw */
    if (s[0] == '+' && isdigit((unsigned char)s[1])) {
        char *end;
        long  n = strtol(s + 1, &end, 10);
        int   unit;
        if      (*end == '\0' || (tolower((unsigned char)*end) == 'd' && !end[1])) unit = 1;
        else if (tolower((unsigned char)*end) == 'w' && !end[1])                  unit = 7;
        else return 0;
        if (n > 3660) return -1;
        *out = today + n * unit;
        return 1;
    }

    /* 2026-09-30 */
    if (all_digits(s, 4) && s[4] == '-')
        return day_parse(s, out) ? 1 : -1;

    /* 30.09, 30.09., 30.09.2026 */
    if (isdigit((unsigned char)s[0])) {
        const char *p = s;
        int d = 0, m = 0, y = 0, nd;

        for (nd = 0; isdigit((unsigned char)*p) && nd < 3; nd++, p++) d = d * 10 + (*p - '0');
        if (nd > 2 || *p != '.') return 0;
        p++;
        for (nd = 0; isdigit((unsigned char)*p) && nd < 3; nd++, p++) m = m * 10 + (*p - '0');
        if (nd == 0 || nd > 2) return 0;
        if (*p == '.') {
            p++;
            if (*p == '\0')
                return next_day_month(d, m, today, out);
            if (!all_digits(p, 4) || p[4] != '\0') return 0;
            y = atoi(p);
            if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return -1;
            *out = day_from_ymd(y, m, d);
            return 1;
        }
        if (*p != '\0') return 0;
        return next_day_month(d, m, today, out);
    }
    return 0;
}

static int
parse_priority(const char *s)
{
    int i;
    if (s[0] != '!') return 0;
    for (i = 0; i < COUNT(priorities); i++)
        if (utf8_eq_ci(s + 1, priorities[i].word)) return priorities[i].priority;
    return 0;
}

static int
is_connector(const char *s)
{
    int i;
    for (i = 0; i < COUNT(connectors); i++)
        if (utf8_eq_ci(s, connectors[i])) return 1;
    return 0;
}

static int
parse_line(const char *s, Day today, QuickAdd *out, char *err, size_t errsz,
           int need_title)
{
    char  buf[512];
    char *tok[MAX_TOKENS];
    int   kind[MAX_TOKENS];
    int   ntok = 0, i, r;
    char *p;
    size_t len = 0;

    memset(out, 0, sizeof(*out));
    err[0] = '\0';
    snprintf(buf, sizeof(buf), "%s", s);

    for (p = buf; *p && ntok < MAX_TOKENS; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tok[ntok++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    for (i = 0; i < ntok; i++) {
        Day  d;
        char t[6];
        int  prio;

        kind[i] = T_WORD;
        if ((r = parse_date(tok[i], today, &d)) != 0) {
            if (r < 0) {
                snprintf(err, errsz, "No such date: %s", tok[i]);
                return 0;
            }
            if (out->has_date) {
                snprintf(err, errsz, "More than one date: %s", tok[i]);
                return 0;
            }
            out->has_date = 1;
            out->date     = d;
            kind[i]       = T_DATE;
        } else if ((r = parse_time(tok[i], t)) != 0) {
            if (r < 0) {
                snprintf(err, errsz, "No such time: %s", tok[i]);
                return 0;
            }
            if (out->time[0]) {
                snprintf(err, errsz, "More than one time: %s", tok[i]);
                return 0;
            }
            memcpy(out->time, t, sizeof(out->time));
            kind[i] = T_TIME;
        } else if ((prio = parse_priority(tok[i])) != 0) {
            if (out->priority) {
                snprintf(err, errsz, "More than one priority: %s", tok[i]);
                return 0;
            }
            out->priority = prio;
            kind[i]       = T_PRIO;
        }
    }

    for (i = 0; i + 1 < ntok; i++)
        if (kind[i] == T_WORD && (kind[i + 1] == T_DATE || kind[i + 1] == T_TIME) &&
            is_connector(tok[i]))
            kind[i] = T_DROP;

    for (i = 0; i < ntok; i++) {
        if (kind[i] != T_WORD) continue;
        len += (size_t)snprintf(out->title + len, sizeof(out->title) - len,
                                "%s%s", len ? " " : "", tok[i]);
        if (len >= sizeof(out->title)) break;
    }
    if (need_title && !out->title[0]) {
        snprintf(err, errsz, "Missing title");
        return 0;
    }
    if (out->time[0] && !out->has_date) {
        out->has_date = 1;
        out->date     = today;
    }
    return 1;
}

int
quickadd_parse(const char *s, Day today, QuickAdd *out, char *err, size_t errsz)
{
    return parse_line(s, today, out, err, errsz, 1);
}

int
quickadd_parse_when(const char *s, Day today, Day *date, char time[6],
                    char *err, size_t errsz)
{
    QuickAdd qa;

    if (!parse_line(s, today, &qa, err, errsz, 0))
        return 0;
    if (qa.title[0] || qa.priority) {
        snprintf(err, errsz, "Only a date and time, e.g. \"jutro 15:00\"");
        return 0;
    }
    if (!qa.has_date) {
        snprintf(err, errsz, "Missing date");
        return 0;
    }
    *date = qa.date;
    memcpy(time, qa.time, 6);
    return 1;
}
