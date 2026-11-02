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

G_END_DECLS

#endif
