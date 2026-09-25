#ifndef DYNLIST_H
#define DYNLIST_H

#define MAX_DYN_OPTIONS 128
#define MAX_OPTION_LEN   64

typedef struct {
    char        items[MAX_DYN_OPTIONS][MAX_OPTION_LEN];
    const char *ptrs[MAX_DYN_OPTIONS + 1];
    int         count;
    int         sel;
    int         scroll;
    const char *category;
    char        edit_buf[MAX_OPTION_LEN]; /* text field for editing/adding */
} DynList;

void dynlist_init(DynList *dl, const char *cat);
void dynlist_rebuild_ptrs(DynList *dl);
void dynlist_sort(DynList *dl);
void dynlist_add(DynList *dl, const char *item);
void dynlist_delete(DynList *dl, int idx);

#endif /* DYNLIST_H */
