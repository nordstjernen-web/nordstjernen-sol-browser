/* Nordstjernen — minimal JSON tree parser and writer for the remote-control channels. */

#ifndef NORDSTJERNEN_GTK_RJSON_H
#define NORDSTJERNEN_GTK_RJSON_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    RJ_NULL,
    RJ_BOOL,
    RJ_NUM,
    RJ_STR,
    RJ_ARR,
    RJ_OBJ
} RjType;

typedef struct RjValue {
    RjType     type;
    gboolean   b;
    double     num;
    char      *str;
    GPtrArray *keys;
    GPtrArray *items;
} RjValue;

RjValue       *rj_parse(const char *text, gsize len);
void           rj_free(RjValue *v);

const RjValue *rj_get(const RjValue *obj, const char *key);
const char    *rj_get_str(const RjValue *obj, const char *key);
gboolean       rj_get_num(const RjValue *obj, const char *key, double *out);
gboolean       rj_get_bool(const RjValue *obj, const char *key, gboolean dflt);

void           rj_write(GString *out, const RjValue *v);
void           rj_append_str(GString *out, const char *s);
char          *rj_to_string(const RjValue *v);

RjValue       *rj_new_null(void);
RjValue       *rj_new_bool(gboolean b);
RjValue       *rj_new_num(double num);
RjValue       *rj_new_str(const char *s);
RjValue       *rj_new_str_take(char *s);
RjValue       *rj_new_arr(void);
RjValue       *rj_new_obj(void);
RjValue       *rj_copy(const RjValue *v);
void           rj_arr_add(RjValue *arr, RjValue *item);
void           rj_obj_set(RjValue *obj, const char *key, RjValue *item);

G_END_DECLS

#endif
