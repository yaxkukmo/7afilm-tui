#ifndef FORM_H
#define FORM_H

/*
 * form.h - immediate-mode form fields: every frame the draw code
 * registers its focusable fields in order, and key handling works on
 * the fields registered in the previous frame.
 */

#include <stddef.h> /* size_t */

#define MAX_FIELDS     160
#define MAX_DROPDOWNS    8

#define FT_TEXT     0  /* free text                                    */
#define FT_DIGITS   1  /* digits only                                  */
#define FT_SPINNER  2  /* digits + up/down arrows (HH/MM/SS)          */
#define FT_BUTTON   3  /* Enter/Space = activate                       */
#define FT_TABS     4  /* tab bar - Left/Right handled by the app      */
#define FT_DROPDOWN 5  /* dropdown - opens popup                       */

typedef struct {
    int    type;
    char  *buf;
    size_t bufsz;
    int    maxval;
    void (*action)(void *);
    void  *arg;
    int    row, col, width;
} Field;

typedef struct {
    const char **options;
    int         *index;
    char        *buf;
    size_t       bufsz;
    void       (*on_confirm)(int); /* called with new index on confirm; may be NULL */
} DropdownMeta;

extern Field g_fields[MAX_FIELDS];
extern int   g_nfields;
extern int   g_focus;
extern int   g_focus_stale; /* g_fields still holds the previous screen */

void fields_reset(void);
int  field_reg(int type, char *buf, size_t bufsz, int maxval,
               void (*action)(void *), void *arg,
               int row, int col, int width);

int  draw_textfield(int row, int col, int width,
                    char *buf, size_t bufsz, int type, int maxval);
int  draw_button(int row, int col, const char *label,
                 void (*action)(void *), void *arg);
void draw_hms_fields(int row, int fc, char *hh, char *mm, char *ss);
int  draw_dropdown(int row, int col, int width,
                   const char **options, int *index, char *buf, size_t bufsz,
                   void (*on_confirm)(int));
void draw_dropdown_popup(void);
void form_place_cursor(void);

int  buf_insert(char *buf, size_t bufsz, int ch);
int  buf_backspace(char *buf);

int  form_popup_key(int ch);
void form_field_key(int ch);

#endif /* FORM_H */
