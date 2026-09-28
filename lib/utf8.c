#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600   /* wcwidth() */
#endif

#include "utf8.h"

#include <string.h>
#include <wchar.h>
#include <wctype.h>

int
utf8_decode(const char *s, unsigned *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned c = p[0];
    int len, i;

    if (c == 0)    { *cp = 0; return 0; }
    if (c < 0x80)  { *cp = c; return 1; }
    if      ((c & 0xE0) == 0xC0) { len = 2; c &= 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; c &= 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; c &= 0x07; }
    else { *cp = c; return 1; }           /* stray continuation byte */

    for (i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = p[0]; return 1; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *cp = c;
    return len;
}

int
utf8_encode(unsigned cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int
utf8_cp_width(unsigned cp)
{
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

int
utf8_width(const char *s)
{
    unsigned cp;
    int n, w = 0;
    while ((n = utf8_decode(s, &cp)) > 0) {
        w += utf8_cp_width(cp);
        s += n;
    }
    return w;
}

size_t
utf8_fit(const char *s, int cols, int *width)
{
    const char *p = s;
    unsigned cp;
    int n, w = 0;
    while ((n = utf8_decode(p, &cp)) > 0) {
        int cw = utf8_cp_width(cp);
        if (w + cw > cols) break;
        w += cw;
        p += n;
    }
    if (width) *width = w;
    return (size_t)(p - s);
}

size_t
utf8_wrap(const char *s, int cols, size_t *next)
{
    const char *p   = s;
    const char *brk = NULL;     /* last space that fits */
    unsigned cp;
    int n, w = 0;

    if (cols < 1) cols = 1;
    while (*p && *p != '\n') {
        n = utf8_decode(p, &cp);
        if (w + utf8_cp_width(cp) > cols) {
            if (brk) {
                *next = (size_t)(brk + 1 - s);
                return (size_t)(brk - s);
            }
            if (p == s) p += n;             /* always make progress */
            *next = (size_t)(p - s);
            return (size_t)(p - s);
        }
        if (cp == ' ') brk = p;
        w += utf8_cp_width(cp);
        p += n;
    }
    *next = (size_t)(p - s) + (*p == '\n');
    return (size_t)(p - s);
}

int
utf8_append(char *buf, size_t bufsz, unsigned cp)
{
    char enc[4];
    int  n   = utf8_encode(cp, enc);
    size_t len = strlen(buf);
    if (len + (size_t)n + 1 > bufsz) return 0;
    memcpy(buf + len, enc, (size_t)n);
    buf[len + (size_t)n] = '\0';
    return 1;
}

int
utf8_pop(char *buf)
{
    size_t i = strlen(buf);
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80)
        i--;
    buf[i] = '\0';
    return 1;
}

int
utf8_eq_ci(const char *a, const char *b)
{
    unsigned ca, cb;
    int la, lb;

    for (;;) {
        la = utf8_decode(a, &ca);
        lb = utf8_decode(b, &cb);
        if (la == 0 || lb == 0) return la == lb;
        if (towlower((wint_t)ca) != towlower((wint_t)cb)) return 0;
        a += la;
        b += lb;
    }
}

int
utf8_contains_ci(const char *hay, const char *needle)
{
    const char *h = hay;
    unsigned a, b;
    int la, lb;

    for (;;) {
        const char *x = h, *y = needle;
        for (;;) {
            if ((lb = utf8_decode(y, &b)) == 0) return 1;
            if ((la = utf8_decode(x, &a)) == 0) return 0;
            if (towlower((wint_t)a) != towlower((wint_t)b)) break;
            x += la;
            y += lb;
        }
        if ((la = utf8_decode(h, &a)) == 0) return 0;
        h += la;
    }
}
