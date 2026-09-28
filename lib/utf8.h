#ifndef UTF8_H
#define UTF8_H

/*
 * utf8.h - small UTF-8 helpers.  Strings are stored and passed around
 * as UTF-8; these work on whole characters instead of bytes.  Invalid
 * bytes are treated as one single-column character each.
 */

#include <stddef.h> /* size_t */

/* Decode the character at s into *cp; returns its length in bytes
 * (0 at the terminating NUL). */
int  utf8_decode(const char *s, unsigned *cp);
/* Encode cp into out (at least 4 bytes); returns the length. */
int  utf8_encode(unsigned cp, char *out);

/* Display width of one code point / of a string, in columns */
int  utf8_cp_width(unsigned cp);
int  utf8_width(const char *s);
/* Bytes of s that fit in `cols` columns; their width goes to *width
 * unless it is NULL. */
size_t utf8_fit(const char *s, int cols, int *width);

/* Next line of s wrapped to `cols` columns: returns its length in bytes.
 * Breaks at '\n', else after the last space that fits, else mid-word.
 * *next gets the offset where the following line starts; wrapping is
 * done when s + *next points at the terminating NUL. */
size_t utf8_wrap(const char *s, int cols, size_t *next);

/* Append cp if it fits (with the NUL) in bufsz; returns 1 on success */
int  utf8_append(char *buf, size_t bufsz, unsigned cp);
/* Remove the last character; returns 0 when buf was empty */
int  utf8_pop(char *buf);

/* Case-insensitive substring search / equality */
int  utf8_contains_ci(const char *hay, const char *needle);
int  utf8_eq_ci(const char *a, const char *b);

#endif /* UTF8_H */
