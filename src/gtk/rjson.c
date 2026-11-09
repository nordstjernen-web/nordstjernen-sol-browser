/* Nordstjernen — minimal JSON tree parser and writer for the remote-control channels. */

#include "rjson.h"
#include "../json_string.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RJ_MAX_DEPTH 64

typedef struct {
    const char *p;
    const char *end;
    int         depth;
} RjParser;

static RjValue *rj_parse_value(RjParser *ps);

static RjValue *
rj_new(RjType type)
{
    RjValue *v = g_new0(RjValue, 1);
    v->type = type;
    return v;
}

void
rj_free(RjValue *v)
{
    if (!v)
        return;
    g_free(v->str);
    if (v->keys)
        g_ptr_array_free(v->keys, TRUE);
    if (v->items)
        g_ptr_array_free(v->items, TRUE);
    g_free(v);
}

static void
rj_skip_ws(RjParser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static gboolean
rj_literal(RjParser *ps, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
        return FALSE;
    ps->p += n;
    return TRUE;
}

static char *
rj_parse_string(RjParser *ps)
{
    if (ps->p >= ps->end || *ps->p != '"')
        return NULL;
    const char *start = ps->p + 1;
    const char *stop = start;
    if (ns_json_decode_string(start, ps->end, NULL, &stop, 1) < 0)
        return NULL;
    char *s = g_malloc((gsize)(stop - start) + 1);
    ns_json_decode_string(start, ps->end, s, NULL, 1);
    ps->p = stop + 1;
    return s;
}

static RjValue *
rj_parse_number(RjParser *ps)
{
    const char *start = ps->p;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+'))
        ps->p++;
    while (ps->p < ps->end &&
           (g_ascii_isdigit(*ps->p) || *ps->p == '.' || *ps->p == 'e' ||
            *ps->p == 'E' || *ps->p == '-' || *ps->p == '+'))
        ps->p++;
    if (ps->p == start)
        return NULL;
    char *tmp = g_strndup(start, (gsize)(ps->p - start));
    char *endp = NULL;
    double d = g_ascii_strtod(tmp, &endp);
    gboolean ok = endp && *endp == '\0' && isfinite(d);
    g_free(tmp);
    if (!ok)
        return NULL;
    RjValue *v = rj_new(RJ_NUM);
    v->num = d;
    return v;
}

static RjValue *
rj_parse_container(RjParser *ps, gboolean object)
{
    if (++ps->depth > RJ_MAX_DEPTH)
        return NULL;
    ps->p++;
    RjValue *v = rj_new(object ? RJ_OBJ : RJ_ARR);
    v->items = g_ptr_array_new_with_free_func((GDestroyNotify)rj_free);
    if (object)
        v->keys = g_ptr_array_new_with_free_func(g_free);
    char close = object ? '}' : ']';
    rj_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        rj_skip_ws(ps);
        if (object) {
            char *key = rj_parse_string(ps);
            if (!key)
                goto fail;
            g_ptr_array_add(v->keys, key);
            rj_skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':')
                goto fail;
            ps->p++;
        }
        RjValue *item = rj_parse_value(ps);
        if (!item)
            goto fail;
        g_ptr_array_add(v->items, item);
        rj_skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            ps->depth--;
            return v;
        }
        goto fail;
    }
fail:
    rj_free(v);
    return NULL;
}

static RjValue *
rj_parse_value(RjParser *ps)
{
    rj_skip_ws(ps);
    if (ps->p >= ps->end)
        return NULL;
    char c = *ps->p;
    if (c == '{' || c == '[')
        return rj_parse_container(ps, c == '{');
    if (c == '"') {
        char *s = rj_parse_string(ps);
        if (!s)
            return NULL;
        RjValue *v = rj_new(RJ_STR);
        v->str = s;
        return v;
    }
    if (rj_literal(ps, "true")) {
        RjValue *v = rj_new(RJ_BOOL);
        v->b = TRUE;
        return v;
    }
    if (rj_literal(ps, "false"))
        return rj_new(RJ_BOOL);
    if (rj_literal(ps, "null"))
        return rj_new(RJ_NULL);
    return rj_parse_number(ps);
}

RjValue *
rj_parse(const char *text, gsize len)
{
    if (!text)
        return NULL;
    RjParser ps = { text, text + len, 0 };
    RjValue *v = rj_parse_value(&ps);
    if (!v)
        return NULL;
    rj_skip_ws(&ps);
    if (ps.p != ps.end) {
        rj_free(v);
        return NULL;
    }
    return v;
}

const RjValue *
rj_get(const RjValue *obj, const char *key)
{
    if (!obj || obj->type != RJ_OBJ || !key)
        return NULL;
    for (guint i = 0; i < obj->keys->len; i++)
        if (strcmp(g_ptr_array_index(obj->keys, i), key) == 0)
            return g_ptr_array_index(obj->items, i);
    return NULL;
}

const char *
rj_get_str(const RjValue *obj, const char *key)
{
    const RjValue *v = rj_get(obj, key);
    return v && v->type == RJ_STR ? v->str : NULL;
}

gboolean
rj_get_num(const RjValue *obj, const char *key, double *out)
{
    const RjValue *v = rj_get(obj, key);
    if (!v)
        return FALSE;
    if (v->type == RJ_NUM) {
        *out = v->num;
        return TRUE;
    }
    if (v->type == RJ_STR && v->str && *v->str) {
        char *endp = NULL;
        double d = g_ascii_strtod(v->str, &endp);
        if (endp && *endp == '\0' && isfinite(d)) {
            *out = d;
            return TRUE;
        }
    }
    return FALSE;
}

gboolean
rj_get_bool(const RjValue *obj, const char *key, gboolean dflt)
{
    const RjValue *v = rj_get(obj, key);
    if (!v)
        return dflt;
    if (v->type == RJ_BOOL)
        return v->b;
    if (v->type == RJ_NUM)
        return v->num != 0;
    if (v->type == RJ_STR)
        return g_ascii_strcasecmp(v->str, "true") == 0 ||
               g_strcmp0(v->str, "1") == 0;
    return dflt;
}

void
rj_append_str(GString *out, const char *s)
{
    char *valid = g_utf8_make_valid(s ? s : "", -1);
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)valid; *p; p++) {
        switch (*p) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if (*p < 0x20)
                g_string_append_printf(out, "\\u%04x", *p);
            else
                g_string_append_c(out, (char)*p);
        }
    }
    g_string_append_c(out, '"');
    g_free(valid);
}

void
rj_write(GString *out, const RjValue *v)
{
    if (!v) {
        g_string_append(out, "null");
        return;
    }
    switch (v->type) {
    case RJ_NULL:
        g_string_append(out, "null");
        break;
    case RJ_BOOL:
        g_string_append(out, v->b ? "true" : "false");
        break;
    case RJ_NUM: {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        if (v->num == floor(v->num) && fabs(v->num) < 9e15)
            g_string_append_printf(out, "%.0f", v->num);
        else
            g_string_append(out, g_ascii_dtostr(buf, sizeof buf, v->num));
        break;
    }
    case RJ_STR:
        rj_append_str(out, v->str);
        break;
    case RJ_ARR:
    case RJ_OBJ:
        g_string_append_c(out, v->type == RJ_OBJ ? '{' : '[');
        for (guint i = 0; i < v->items->len; i++) {
            if (i)
                g_string_append_c(out, ',');
            if (v->type == RJ_OBJ) {
                rj_append_str(out, g_ptr_array_index(v->keys, i));
                g_string_append_c(out, ':');
            }
            rj_write(out, g_ptr_array_index(v->items, i));
        }
        g_string_append_c(out, v->type == RJ_OBJ ? '}' : ']');
        break;
    }
}

char *
rj_to_string(const RjValue *v)
{
    GString *out = g_string_new(NULL);
    rj_write(out, v);
    return g_string_free(out, FALSE);
}

RjValue *
rj_new_null(void)
{
    return rj_new(RJ_NULL);
}

RjValue *
rj_new_bool(gboolean b)
{
    RjValue *v = rj_new(RJ_BOOL);
    v->b = b ? TRUE : FALSE;
    return v;
}

RjValue *
rj_new_num(double num)
{
    RjValue *v = rj_new(RJ_NUM);
    v->num = isfinite(num) ? num : 0;
    return v;
}

RjValue *
rj_new_str_take(char *s)
{
    RjValue *v = rj_new(RJ_STR);
    v->str = s ? s : g_strdup("");
    return v;
}

RjValue *
rj_new_str(const char *s)
{
    return rj_new_str_take(g_strdup(s ? s : ""));
}

static RjValue *
rj_new_container(gboolean object)
{
    RjValue *v = rj_new(object ? RJ_OBJ : RJ_ARR);
    v->items = g_ptr_array_new_with_free_func((GDestroyNotify)rj_free);
    if (object)
        v->keys = g_ptr_array_new_with_free_func(g_free);
    return v;
}

RjValue *
rj_new_arr(void)
{
    return rj_new_container(FALSE);
}

RjValue *
rj_new_obj(void)
{
    return rj_new_container(TRUE);
}

RjValue *
rj_copy(const RjValue *v)
{
    if (!v)
        return NULL;
    RjValue *c = v->type == RJ_ARR || v->type == RJ_OBJ
        ? rj_new_container(v->type == RJ_OBJ) : rj_new(v->type);
    c->b = v->b;
    c->num = v->num;
    c->str = g_strdup(v->str);
    for (guint i = 0; v->items && i < v->items->len; i++)
        g_ptr_array_add(c->items, rj_copy(g_ptr_array_index(v->items, i)));
    for (guint i = 0; v->keys && i < v->keys->len; i++)
        g_ptr_array_add(c->keys, g_strdup(g_ptr_array_index(v->keys, i)));
    return c;
}

void
rj_arr_add(RjValue *arr, RjValue *item)
{
    if (!arr || arr->type != RJ_ARR || !item) {
        rj_free(item);
        return;
    }
    g_ptr_array_add(arr->items, item);
}

void
rj_obj_set(RjValue *obj, const char *key, RjValue *item)
{
    if (!obj || obj->type != RJ_OBJ || !key || !item) {
        rj_free(item);
        return;
    }
    for (guint i = 0; i < obj->keys->len; i++) {
        if (strcmp(g_ptr_array_index(obj->keys, i), key) == 0) {
            rj_free(g_ptr_array_index(obj->items, i));
            g_ptr_array_index(obj->items, i) = item;
            return;
        }
    }
    g_ptr_array_add(obj->keys, g_strdup(key));
    g_ptr_array_add(obj->items, item);
}
