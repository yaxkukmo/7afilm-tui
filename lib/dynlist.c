#include "dynlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
dynlist_init(DynList *dl, const char *cat)
{
    memset(dl, 0, sizeof(*dl));
    dl->category = cat;
}

void
dynlist_rebuild_ptrs(DynList *dl)
{
    int i;
    for (i = 0; i < dl->count; i++)
        dl->ptrs[i] = dl->items[i];
    dl->ptrs[dl->count] = NULL;
}

static int
dynlist_cmp(const void *a, const void *b)
{
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    char *ea, *eb;
    long  na, nb;

    /* Pure integers: numeric sort (e.g. ISO values) */
    na = strtol(sa, &ea, 10);
    nb = strtol(sb, &eb, 10);
    if (*ea == '\0' && *eb == '\0')
        return (na > nb) - (na < nb);

    /* Dilution ratios "N:N": compare denominators numerically */
    if (strchr(sa, ':') && strchr(sb, ':')) {
        na = strtol(strchr(sa, ':') + 1, &ea, 10);
        nb = strtol(strchr(sb, ':') + 1, &eb, 10);
        if (*ea == '\0' && *eb == '\0')
            return (na > nb) - (na < nb);
    }

    return strcasecmp(sa, sb);
}

void
dynlist_sort(DynList *dl)
{
    qsort(dl->items, (size_t)dl->count,
          MAX_OPTION_LEN, dynlist_cmp);
    dynlist_rebuild_ptrs(dl);
}

void
dynlist_add(DynList *dl, const char *item)
{
    int i;
    if (!item || !item[0]) return;
    if (dl->count >= MAX_DYN_OPTIONS) return;
    for (i = 0; i < dl->count; i++)
        if (strcmp(dl->items[i], item) == 0) return;
    snprintf(dl->items[dl->count], MAX_OPTION_LEN, "%s", item);
    dl->count++;
    dynlist_sort(dl);
}

void
dynlist_delete(DynList *dl, int idx)
{
    int i;
    if (idx < 0 || idx >= dl->count) return;
    for (i = idx; i < dl->count - 1; i++)
        memcpy(dl->items[i], dl->items[i + 1], MAX_OPTION_LEN);
    dl->count--;
    dynlist_rebuild_ptrs(dl);
    if (dl->sel >= dl->count) dl->sel = dl->count - 1;
    if (dl->sel < 0) dl->sel = 0;
}
