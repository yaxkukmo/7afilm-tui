#include "date.h"

#include <stdio.h>
#include <time.h>

/* Civil-from-days / days-from-civil after Howard Hinnant's algorithms */

Day
day_from_ymd(int y, int m, int d)
{
    long yy  = (long)y - (m <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    long yoe = yy - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void
day_to_ymd(Day day, int *y, int *m, int *d)
{
    long z   = day + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp  = (5 * doy + 2) / 153;

    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

int
day_weekday(Day day)
{
    /* 1970-01-01 was a Thursday (4) */
    long w = (day + 3) % 7;
    if (w < 0) w += 7;
    return (int)w + 1;
}

int
days_in_month(int y, int m)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (m < 1 || m > 12) return 0;
    return mdays[m - 1] + (m == 2 && leap);
}

Day
day_today(void)
{
    time_t     now = time(NULL);
    struct tm *tm  = localtime(&now);
    return day_from_ymd(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
}

int
day_parse(const char *s, Day *out)
{
    int y, m, d, n = 0;

    if (sscanf(s, "%4d-%2d-%2d%n", &y, &m, &d, &n) != 3 || n != 10 || s[n] != '\0')
        return 0;
    if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m))
        return 0;
    *out = day_from_ymd(y, m, d);
    return 1;
}

void
day_format(Day day, char out[11])
{
    int y, m, d;
    day_to_ymd(day, &y, &m, &d);
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}
