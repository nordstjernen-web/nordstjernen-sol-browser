/* Nordstjernen — the HTML "parse a sizes attribute" algorithm for img srcset selection.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "css.h"
#include "css_syntax.h"

#include <math.h>
#include <string.h>

static char *
sizes_unescape(const char *s)
{
    GString *out = g_string_new(NULL);
    for (const char *p = s; *p; )
        ns_css_append_unescaped(out, &p);
    return g_string_free(out, FALSE);
}

static char *
sizes_source_text(const char *input, const ns_css_component *c)
{
    GString *out = g_string_new(NULL);
    for (gsize i = c->start; i < c->end; i++) {
        if (input[i] == '/' && i + 1 < c->end && input[i + 1] == '*') {
            const char *close = g_strstr_len(input + i + 2, c->end - i - 2, "*/");
            i = close ? (gsize)(close - input) + 1 : c->end;
            g_string_append_c(out, ' ');
            continue;
        }
        g_string_append_c(out, input[i]);
    }
    if (c->type == NS_CSS_COMPONENT_FUNCTION ||
        c->type == NS_CSS_COMPONENT_BLOCK) {
        int depth = 0;
        for (gsize i = 0; i < out->len; i++) {
            if (out->str[i] == '(') depth++;
            else if (out->str[i] == ')' && depth > 0) depth--;
        }
        while (depth-- > 0) g_string_append_c(out, ')');
    }
    return g_string_free(out, FALSE);
}

static gboolean
sizes_eval_length(const char *text, double *out_px)
{
    char *decl = g_strdup_printf("left:%s", text);
    GArray *decls = ns_css_parse_declarations(decl);
    g_free(decl);
    gboolean ok = FALSE;
    const ns_css_value *v = decls->len == 1
        ? g_array_index(decls, ns_css_decl, 0).value : NULL;
    if (v && v->kind == NS_CSS_V_LENGTH &&
        v->u.length.unit != NS_CSS_UNIT_PERCENT) {
        *out_px = ns_css_dimension_px(v, 16.0, 0);
        ok = TRUE;
    } else if (v && v->kind == NS_CSS_V_CALC) {
        gboolean has_pct = v->u.calc.pct != 0.0;
        gboolean math_fn = ns_css_calc_is_math_fn(v);
        if (math_fn)
            for (int i = 0; i < v->u.calc.n_args; i++)
                if (v->u.calc.args[i].pct != 0.0) has_pct = TRUE;
        if (!has_pct) {
            *out_px = math_fn ? ns_css_calc_math_fn_px(v, 1.0)
                              : v->u.calc.px +
                                (v->u.calc.em + v->u.calc.rem) * 16.0;
            ok = TRUE;
        }
    }
    for (guint i = 0; i < decls->len; i++)
        ns_css_value_free(g_array_index(decls, ns_css_decl, i).value);
    g_array_free(decls, TRUE);
    return ok;
}

static gboolean
sizes_is_math_function(const char *name)
{
    static const char *const fns[] = {
        "calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign",
        "hypot",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(fns); i++)
        if (g_ascii_strcasecmp(name, fns[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
sizes_source_size_value(const char *input, const ns_css_component *c,
                        double *out_px)
{
    if (c->type == NS_CSS_COMPONENT_NUMBER) {
        *out_px = 0;
        return c->number == 0;
    }
    if (c->type == NS_CSS_COMPONENT_DIMENSION) {
        if (c->number < 0 || !isfinite(c->number)) return FALSE;
        char *unit = sizes_unescape(c->value);
        char *text = g_strdup_printf("%.17g%s", c->number, unit);
        gboolean ok = sizes_eval_length(text, out_px);
        g_free(text);
        g_free(unit);
        return ok;
    }
    if (c->type == NS_CSS_COMPONENT_FUNCTION &&
        sizes_is_math_function(c->value)) {
        char *text = sizes_source_text(input, c);
        gboolean ok = sizes_eval_length(text, out_px) && isfinite(*out_px);
        g_free(text);
        if (ok && *out_px < 0) *out_px = 0;
        return ok;
    }
    return FALSE;
}

static gboolean
sizes_is_ident(const ns_css_component *c, const char *name)
{
    if (c->type != NS_CSS_COMPONENT_IDENT) return FALSE;
    char *ident = sizes_unescape(c->value);
    gboolean eq = g_ascii_strcasecmp(ident, name) == 0;
    g_free(ident);
    return eq;
}

static gboolean
sizes_in_parens(const ns_css_component *c)
{
    return (c->type == NS_CSS_COMPONENT_BLOCK && c->delimiter == ')') ||
           c->type == NS_CSS_COMPONENT_FUNCTION;
}

static gboolean
sizes_media_condition_valid(ns_css_component **cs, guint n)
{
    if (n == 0) return FALSE;
    if (sizes_is_ident(cs[0], "not"))
        return n == 3 && cs[1]->type == NS_CSS_COMPONENT_WHITESPACE &&
               sizes_in_parens(cs[2]);
    if (!sizes_in_parens(cs[0])) return FALSE;
    const char *joiner = NULL;
    for (guint i = 1; i < n; i += 4) {
        if (i + 3 >= n) return FALSE;
        if (cs[i]->type != NS_CSS_COMPONENT_WHITESPACE ||
            cs[i + 2]->type != NS_CSS_COMPONENT_WHITESPACE ||
            !sizes_in_parens(cs[i + 3]))
            return FALSE;
        const char *op = sizes_is_ident(cs[i + 1], "and") ? "and"
                       : sizes_is_ident(cs[i + 1], "or") ? "or" : NULL;
        if (!op || (joiner && strcmp(joiner, op) != 0)) return FALSE;
        joiner = op;
    }
    return TRUE;
}

static gboolean
sizes_media_condition_matches(const char *input, ns_css_component **cs,
                              guint n)
{
    GString *query = g_string_new(NULL);
    for (guint i = 0; i < n; i++) {
        char *text = sizes_source_text(input, cs[i]);
        g_string_append(query, text);
        g_free(text);
    }
    gboolean matches = ns_css_media_query_matches(query->str);
    g_string_free(query, TRUE);
    return matches;
}

static double
sizes_default(void)
{
    double px = 0;
    if (sizes_eval_length("100vw", &px) && px > 0) return px;
    return ns_css_media_viewport_current_w();
}

double
ns_css_sizes_resolve(const char *sizes)
{
    if (!sizes) return sizes_default();
    GPtrArray *cs = ns_css_component_values_parse(sizes, -1, NULL);
    double result = -1;
    guint start = 0;
    while (start <= cs->len && result < 0) {
        guint end = start;
        while (end < cs->len &&
               ((ns_css_component *)cs->pdata[end])->type !=
                   NS_CSS_COMPONENT_COMMA)
            end++;
        ns_css_component **items = (ns_css_component **)cs->pdata + start;
        guint n = end - start;
        while (n > 0 && items[0]->type == NS_CSS_COMPONENT_WHITESPACE) {
            items++;
            n--;
        }
        while (n > 0 && items[n - 1]->type == NS_CSS_COMPONENT_WHITESPACE)
            n--;
        double size = 0;
        if (n > 0 && sizes_source_size_value(sizes, items[n - 1], &size)) {
            guint cond_n = n - 1;
            while (cond_n > 0 &&
                   items[cond_n - 1]->type == NS_CSS_COMPONENT_WHITESPACE)
                cond_n--;
            if (cond_n == 0 ||
                (sizes_media_condition_valid(items, cond_n) &&
                 sizes_media_condition_matches(sizes, items, cond_n)))
                result = size;
        }
        start = end + 1;
    }
    g_ptr_array_free(cs, TRUE);
    return result >= 0 ? result : sizes_default();
}
