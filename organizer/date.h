#ifndef DATE_H
#define DATE_H

/*
 * date.h - calendar dates as day numbers (days since 1970-01-01, proleptic
 * Gregorian), so date arithmetic is plain integer arithmetic.
 */

typedef long Day;

Day  day_from_ymd(int y, int m, int d);
void day_to_ymd(Day day, int *y, int *m, int *d);
int  day_weekday(Day day);            /* ISO: 1 = Monday .. 7 = Sunday */
int  days_in_month(int y, int m);
Day  day_today(void);                 /* local time */

/* "YYYY-MM-DD"; day_parse returns 0 for anything that is not a real date */
int  day_parse(const char *s, Day *out);
void day_format(Day day, char out[11]);

#endif /* DATE_H */
