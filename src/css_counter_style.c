/* Nordstjernen — @counter-style registry and counter representations. */

#include "css_counter_style.h"

#include <limits.h>
#include <string.h>

#include "css_syntax.h"

#define NS_COUNTER_STYLE_MAX_STYLES 4096
#define NS_COUNTER_STYLE_MAX_SYMBOLS 256
#define NS_COUNTER_STYLE_MAX_REPEAT 64
#define NS_COUNTER_STYLE_MAX_PAD 64
#define NS_COUNTER_STYLE_MAX_CHAIN 8

typedef enum counter_system {
    COUNTER_SYSTEM_SYMBOLIC,
    COUNTER_SYSTEM_CYCLIC,
    COUNTER_SYSTEM_FIXED,
    COUNTER_SYSTEM_ALPHABETIC,
    COUNTER_SYSTEM_NUMERIC,
    COUNTER_SYSTEM_ADDITIVE,
    COUNTER_SYSTEM_EXTENDS,
} counter_system;

typedef struct counter_range {
    gint64 low;
    gint64 high;
} counter_range;

typedef struct counter_style {
    counter_system system;
    gint64 first;
    char *extends;
    GPtrArray *symbols;
    GArray *weights;
    char *negative_before;
    char *negative_after;
    char *prefix;
    char *suffix;
    char *fallback;
    GArray *ranges;
    int pad_length;
    char *pad_symbol;
} counter_style;

static GMutex g_counter_styles_lock;
static GHashTable *g_counter_styles;

static void
counter_style_free(gpointer data)
{
    counter_style *cs = data;
    if (!cs) return;
    g_free(cs->extends);
    if (cs->symbols) g_ptr_array_free(cs->symbols, TRUE);
    if (cs->weights) g_array_free(cs->weights, TRUE);
    g_free(cs->negative_before);
    g_free(cs->negative_after);
    g_free(cs->prefix);
    g_free(cs->suffix);
    g_free(cs->fallback);
    if (cs->ranges) g_array_free(cs->ranges, TRUE);
    g_free(cs->pad_symbol);
    g_free(cs);
}

static GPtrArray *
descriptor_components(const char *value)
{
    gboolean valid = FALSE;
    GPtrArray *all = ns_css_component_values_parse(value, -1, &valid);
    if (!valid) {
        g_ptr_array_free(all, TRUE);
        return NULL;
    }
    for (guint i = all->len; i > 0; i--) {
        const ns_css_component *c = g_ptr_array_index(all, i - 1);
        if (c->type == NS_CSS_COMPONENT_WHITESPACE)
            g_ptr_array_remove_index(all, i - 1);
    }
    return all;
}

static gboolean
component_is_symbol(const ns_css_component *c)
{
    return c->type == NS_CSS_COMPONENT_STRING ||
           c->type == NS_CSS_COMPONENT_IDENT;
}

static gboolean
component_is_integer(const ns_css_component *c)
{
    return c->type == NS_CSS_COMPONENT_NUMBER &&
           c->number == (double)(gint64)c->number &&
           c->number >= INT_MIN && c->number <= INT_MAX;
}

static gboolean
component_ident_is(const ns_css_component *c, const char *ident)
{
    return c->type == NS_CSS_COMPONENT_IDENT && c->value &&
           g_ascii_strcasecmp(c->value, ident) == 0;
}

static char *
component_symbol_dup(const ns_css_component *c)
{
    return g_strdup(c->value ? c->value : "");
}

static void
descriptor_system(counter_style *cs, GPtrArray *items)
{
    static const struct { const char *name; counter_system system; } names[] = {
        { "symbolic", COUNTER_SYSTEM_SYMBOLIC },
        { "cyclic", COUNTER_SYSTEM_CYCLIC },
        { "fixed", COUNTER_SYSTEM_FIXED },
        { "alphabetic", COUNTER_SYSTEM_ALPHABETIC },
        { "numeric", COUNTER_SYSTEM_NUMERIC },
        { "additive", COUNTER_SYSTEM_ADDITIVE },
        { "extends", COUNTER_SYSTEM_EXTENDS },
    };
    if (items->len < 1 || items->len > 2) return;
    const ns_css_component *kind = g_ptr_array_index(items, 0);
    const ns_css_component *arg =
        items->len == 2 ? g_ptr_array_index(items, 1) : NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
        if (!component_ident_is(kind, names[i].name)) continue;
        counter_system system = names[i].system;
        if (system == COUNTER_SYSTEM_EXTENDS) {
            if (!arg || arg->type != NS_CSS_COMPONENT_IDENT) return;
            g_free(cs->extends);
            cs->extends = g_strdup(arg->value);
        } else if (system == COUNTER_SYSTEM_FIXED) {
            if (arg && !component_is_integer(arg)) return;
            cs->first = arg ? (gint64)arg->number : 1;
        } else if (arg) {
            return;
        }
        cs->system = system;
        return;
    }
}

static void
descriptor_symbols(counter_style *cs, GPtrArray *items)
{
    if (items->len == 0 || items->len > NS_COUNTER_STYLE_MAX_SYMBOLS) return;
    for (guint i = 0; i < items->len; i++)
        if (!component_is_symbol(g_ptr_array_index(items, i))) return;
    if (cs->symbols) g_ptr_array_free(cs->symbols, TRUE);
    cs->symbols = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < items->len; i++)
        g_ptr_array_add(cs->symbols,
                        component_symbol_dup(g_ptr_array_index(items, i)));
}

static void
descriptor_additive_symbols(counter_style *cs, GPtrArray *items)
{
    GPtrArray *symbols = g_ptr_array_new_with_free_func(g_free);
    GArray *weights = g_array_new(FALSE, FALSE, sizeof(gint64));
    gboolean ok = items->len > 0;
    guint i = 0;
    while (ok && i < items->len) {
        if (i + 1 >= items->len) { ok = FALSE; break; }
        const ns_css_component *a = g_ptr_array_index(items, i);
        const ns_css_component *b = g_ptr_array_index(items, i + 1);
        const ns_css_component *weight = component_is_integer(a) ? a : b;
        const ns_css_component *symbol = weight == a ? b : a;
        if (!component_is_integer(weight) || weight->number < 0 ||
            !component_is_symbol(symbol)) { ok = FALSE; break; }
        gint64 w = (gint64)weight->number;
        if (weights->len > 0 &&
            w >= g_array_index(weights, gint64, weights->len - 1)) {
            ok = FALSE;
            break;
        }
        g_array_append_val(weights, w);
        g_ptr_array_add(symbols, component_symbol_dup(symbol));
        i += 2;
        if (i < items->len) {
            const ns_css_component *comma = g_ptr_array_index(items, i);
            if (comma->type != NS_CSS_COMPONENT_COMMA) { ok = FALSE; break; }
            i++;
            if (i >= items->len) ok = FALSE;
        }
    }
    if (!ok || symbols->len > NS_COUNTER_STYLE_MAX_SYMBOLS) {
        g_ptr_array_free(symbols, TRUE);
        g_array_free(weights, TRUE);
        return;
    }
    if (cs->symbols) g_ptr_array_free(cs->symbols, TRUE);
    if (cs->weights) g_array_free(cs->weights, TRUE);
    cs->symbols = symbols;
    cs->weights = weights;
}

static void
descriptor_symbol(char **slot, GPtrArray *items)
{
    if (items->len != 1 || !component_is_symbol(g_ptr_array_index(items, 0)))
        return;
    g_free(*slot);
    *slot = component_symbol_dup(g_ptr_array_index(items, 0));
}

static void
descriptor_negative(counter_style *cs, GPtrArray *items)
{
    if (items->len < 1 || items->len > 2) return;
    for (guint i = 0; i < items->len; i++)
        if (!component_is_symbol(g_ptr_array_index(items, i))) return;
    g_free(cs->negative_before);
    g_free(cs->negative_after);
    cs->negative_before = component_symbol_dup(g_ptr_array_index(items, 0));
    cs->negative_after = items->len == 2
        ? component_symbol_dup(g_ptr_array_index(items, 1)) : g_strdup("");
}

static gboolean
range_bound(const ns_css_component *c, gint64 infinite, gint64 *out)
{
    if (component_ident_is(c, "infinite")) {
        *out = infinite;
        return TRUE;
    }
    if (!component_is_integer(c)) return FALSE;
    *out = (gint64)c->number;
    return TRUE;
}

static void
descriptor_range(counter_style *cs, GPtrArray *items)
{
    if (items->len == 1 && component_ident_is(g_ptr_array_index(items, 0), "auto")) {
        if (cs->ranges) g_array_free(cs->ranges, TRUE);
        cs->ranges = NULL;
        return;
    }
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(counter_range));
    gboolean ok = items->len > 0;
    guint i = 0;
    while (ok && i < items->len) {
        counter_range r;
        if (i + 1 >= items->len ||
            !range_bound(g_ptr_array_index(items, i), G_MININT64, &r.low) ||
            !range_bound(g_ptr_array_index(items, i + 1), G_MAXINT64, &r.high) ||
            r.low > r.high) {
            ok = FALSE;
            break;
        }
        g_array_append_val(ranges, r);
        i += 2;
        if (i < items->len) {
            const ns_css_component *comma = g_ptr_array_index(items, i);
            if (comma->type != NS_CSS_COMPONENT_COMMA) ok = FALSE;
            i++;
            if (i >= items->len) ok = FALSE;
        }
    }
    if (!ok) {
        g_array_free(ranges, TRUE);
        return;
    }
    if (cs->ranges) g_array_free(cs->ranges, TRUE);
    cs->ranges = ranges;
}

static void
descriptor_pad(counter_style *cs, GPtrArray *items)
{
    if (items->len != 2) return;
    const ns_css_component *a = g_ptr_array_index(items, 0);
    const ns_css_component *b = g_ptr_array_index(items, 1);
    const ns_css_component *length = component_is_integer(a) ? a : b;
    const ns_css_component *symbol = length == a ? b : a;
    if (!component_is_integer(length) || length->number < 0 ||
        !component_is_symbol(symbol))
        return;
    cs->pad_length = (int)MIN(length->number, NS_COUNTER_STYLE_MAX_PAD);
    g_free(cs->pad_symbol);
    cs->pad_symbol = component_symbol_dup(symbol);
}

static void
descriptor_fallback(counter_style *cs, GPtrArray *items)
{
    if (items->len != 1) return;
    const ns_css_component *c = g_ptr_array_index(items, 0);
    if (c->type != NS_CSS_COMPONENT_IDENT) return;
    g_free(cs->fallback);
    cs->fallback = g_strdup(c->value);
}

static void
counter_style_descriptor(counter_style *cs, const char *name, const char *value)
{
    GPtrArray *items = descriptor_components(value);
    if (!items) return;
    if (g_ascii_strcasecmp(name, "system") == 0)
        descriptor_system(cs, items);
    else if (g_ascii_strcasecmp(name, "symbols") == 0)
        descriptor_symbols(cs, items);
    else if (g_ascii_strcasecmp(name, "additive-symbols") == 0)
        descriptor_additive_symbols(cs, items);
    else if (g_ascii_strcasecmp(name, "prefix") == 0)
        descriptor_symbol(&cs->prefix, items);
    else if (g_ascii_strcasecmp(name, "suffix") == 0)
        descriptor_symbol(&cs->suffix, items);
    else if (g_ascii_strcasecmp(name, "negative") == 0)
        descriptor_negative(cs, items);
    else if (g_ascii_strcasecmp(name, "range") == 0)
        descriptor_range(cs, items);
    else if (g_ascii_strcasecmp(name, "pad") == 0)
        descriptor_pad(cs, items);
    else if (g_ascii_strcasecmp(name, "fallback") == 0)
        descriptor_fallback(cs, items);
    g_ptr_array_free(items, TRUE);
}

static gboolean
counter_style_valid(const counter_style *cs)
{
    guint n = cs->symbols ? cs->symbols->len : 0;
    switch (cs->system) {
    case COUNTER_SYSTEM_EXTENDS:
        return cs->extends && !cs->symbols;
    case COUNTER_SYSTEM_ADDITIVE:
        return cs->weights && n > 0;
    case COUNTER_SYSTEM_ALPHABETIC:
    case COUNTER_SYSTEM_NUMERIC:
        return !cs->weights && n >= 2;
    default:
        return !cs->weights && n >= 1;
    }
}

static gboolean
counter_style_name_reserved(const char *name)
{
    static const char *const reserved[] = {
        "none", "decimal", "disc", "square", "circle", "disclosure-open",
        "disclosure-closed", "initial", "inherit", "unset", "revert",
        "revert-layer", "default",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(reserved); i++)
        if (g_ascii_strcasecmp(name, reserved[i]) == 0) return TRUE;
    return FALSE;
}

static void
counter_style_parse_body(counter_style *cs, const char *body, gsize body_len)
{
    gboolean valid = FALSE;
    GPtrArray *items = ns_css_component_values_parse(body, (gssize)body_len,
                                                     &valid);
    guint i = 0;
    while (valid && i < items->len) {
        const ns_css_component *name = g_ptr_array_index(items, i);
        guint colon = i + 1;
        while (colon < items->len &&
               ((const ns_css_component *)g_ptr_array_index(items, colon))->type ==
                   NS_CSS_COMPONENT_WHITESPACE)
            colon++;
        guint stop = i;
        while (stop < items->len &&
               ((const ns_css_component *)g_ptr_array_index(items, stop))->type !=
                   NS_CSS_COMPONENT_SEMICOLON)
            stop++;
        if (name->type == NS_CSS_COMPONENT_IDENT && colon < stop &&
            ((const ns_css_component *)g_ptr_array_index(items, colon))->type ==
                NS_CSS_COMPONENT_COLON &&
            colon + 1 < stop) {
            const ns_css_component *first = g_ptr_array_index(items, colon + 1);
            const ns_css_component *last = g_ptr_array_index(items, stop - 1);
            if (last->end <= body_len && first->start <= last->end) {
                char *value = g_strndup(body + first->start,
                                        last->end - first->start);
                counter_style_descriptor(cs, name->value, value);
                g_free(value);
            }
        }
        i = stop + 1;
    }
    g_ptr_array_free(items, TRUE);
}

void
ns_css_counter_style_register(const char *name, const char *body,
                              gsize body_len)
{
    if (!name || !*name || !body || counter_style_name_reserved(name)) return;
    counter_style *cs = g_new0(counter_style, 1);
    cs->system = COUNTER_SYSTEM_SYMBOLIC;
    cs->first = 1;
    counter_style_parse_body(cs, body, body_len);
    if (!counter_style_valid(cs)) {
        counter_style_free(cs);
        return;
    }
    g_mutex_lock(&g_counter_styles_lock);
    if (!g_counter_styles)
        g_counter_styles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, counter_style_free);
    if (g_hash_table_size(g_counter_styles) < NS_COUNTER_STYLE_MAX_STYLES ||
        g_hash_table_contains(g_counter_styles, name))
        g_hash_table_replace(g_counter_styles, g_strdup(name), cs);
    else
        counter_style_free(cs);
    g_mutex_unlock(&g_counter_styles_lock);
}

static char *
unichar_dup(gunichar c)
{
    char buf[8];
    buf[g_unichar_to_utf8(c, buf)] = '\0';
    return g_strdup(buf);
}

static counter_style *
predefined_cyclic(gunichar symbol)
{
    counter_style *cs = g_new0(counter_style, 1);
    cs->system = COUNTER_SYSTEM_CYCLIC;
    cs->first = 1;
    cs->symbols = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(cs->symbols, unichar_dup(symbol));
    cs->suffix = g_strdup(" ");
    return cs;
}

static gint64
additive_weight(guint i)
{
    gint64 weight = i % 9 + 1;
    for (guint k = 0; k < i / 9; k++) weight *= 10;
    return weight;
}

static counter_style *
predefined_additive(const gunichar *ascending, guint n, gint64 high)
{
    counter_style *cs = g_new0(counter_style, 1);
    cs->system = COUNTER_SYSTEM_ADDITIVE;
    cs->first = 1;
    cs->symbols = g_ptr_array_new_with_free_func(g_free);
    cs->weights = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (guint i = n; i > 0; i--) {
        gint64 weight = additive_weight(i - 1);
        g_ptr_array_add(cs->symbols, unichar_dup(ascending[i - 1]));
        g_array_append_val(cs->weights, weight);
    }
    counter_range range = { 1, high };
    cs->ranges = g_array_new(FALSE, FALSE, sizeof(counter_range));
    g_array_append_val(cs->ranges, range);
    return cs;
}

static counter_style *
predefined_armenian(gunichar one)
{
    gunichar symbols[36];
    for (guint i = 0; i < G_N_ELEMENTS(symbols); i++) symbols[i] = one + i;
    return predefined_additive(symbols, G_N_ELEMENTS(symbols), 9999);
}

static counter_style *
predefined_georgian(void)
{
    static const gunichar symbols[] = {
        0x10D0, 0x10D1, 0x10D2, 0x10D3, 0x10D4, 0x10D5, 0x10D6, 0x10F1,
        0x10D7, 0x10D8, 0x10D9, 0x10DA, 0x10DB, 0x10DC, 0x10F2, 0x10DD,
        0x10DE, 0x10DF, 0x10E0, 0x10E1, 0x10E2, 0x10F3, 0x10E4, 0x10E5,
        0x10E6, 0x10E7, 0x10E8, 0x10E9, 0x10EA, 0x10EB, 0x10EC, 0x10ED,
        0x10EE, 0x10F4, 0x10EF, 0x10F0, 0x10F5,
    };
    return predefined_additive(symbols, G_N_ELEMENTS(symbols), 19999);
}

static counter_style *
predefined_upper_armenian(void)
{
    return predefined_armenian(0x531);
}

static counter_style *
predefined_lower_armenian(void)
{
    return predefined_armenian(0x561);
}

static GHashTable *
predefined_counter_styles(void)
{
    static const struct { const char *name; gunichar symbol; } cyclic[] = {
        { "disc", 0x2022 }, { "circle", 0x25E6 }, { "square", 0x25AA },
        { "disclosure-open", 0x25BE }, { "disclosure-closed", 0x25B8 },
    };
    static const struct {
        const char *name;
        counter_style *(*build)(void);
    } additive[] = {
        { "armenian", predefined_upper_armenian },
        { "upper-armenian", predefined_upper_armenian },
        { "lower-armenian", predefined_lower_armenian },
        { "georgian", predefined_georgian },
    };
    static gsize once;
    static GHashTable *table;
    if (g_once_init_enter(&once)) {
        table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      counter_style_free);
        for (gsize i = 0; i < G_N_ELEMENTS(cyclic); i++)
            g_hash_table_insert(table, g_strdup(cyclic[i].name),
                                predefined_cyclic(cyclic[i].symbol));
        for (gsize i = 0; i < G_N_ELEMENTS(additive); i++)
            g_hash_table_insert(table, g_strdup(additive[i].name),
                                additive[i].build());
        g_once_init_leave(&once, 1);
    }
    return table;
}

static const counter_style *
author_counter_style_lookup(const char *name)
{
    return name && g_counter_styles
        ? g_hash_table_lookup(g_counter_styles, name) : NULL;
}

static const counter_style *
counter_style_lookup(const char *name)
{
    const counter_style *cs = author_counter_style_lookup(name);
    if (cs || !name) return cs;
    return g_hash_table_lookup(predefined_counter_styles(), name);
}

static const counter_style *
counter_style_algorithm(const counter_style *cs)
{
    for (int depth = 0; cs && cs->system == COUNTER_SYSTEM_EXTENDS; depth++) {
        if (depth >= NS_COUNTER_STYLE_MAX_CHAIN) return NULL;
        cs = counter_style_lookup(cs->extends);
    }
    return cs;
}

static const char *
counter_style_inherited(const counter_style *cs, gsize field_offset)
{
    for (int depth = 0; cs && depth < NS_COUNTER_STYLE_MAX_CHAIN; depth++) {
        const char *value = G_STRUCT_MEMBER(char *, cs, field_offset);
        if (value) return value;
        if (cs->system != COUNTER_SYSTEM_EXTENDS) return NULL;
        cs = counter_style_lookup(cs->extends);
    }
    return NULL;
}

static const counter_style *
counter_style_with_pad(const counter_style *cs)
{
    for (int depth = 0; cs && depth < NS_COUNTER_STYLE_MAX_CHAIN; depth++) {
        if (cs->pad_symbol) return cs;
        if (cs->system != COUNTER_SYSTEM_EXTENDS) return NULL;
        cs = counter_style_lookup(cs->extends);
    }
    return NULL;
}

static gboolean
counter_value_in_range(const counter_style *cs, const counter_style *algo,
                       gint64 value)
{
    const GArray *ranges = NULL;
    for (const counter_style *c = cs; c; ) {
        if (c->ranges) { ranges = c->ranges; break; }
        if (c->system != COUNTER_SYSTEM_EXTENDS || c == algo) break;
        c = counter_style_lookup(c->extends);
        if (c == cs) break;
    }
    if (ranges) {
        for (guint i = 0; i < ranges->len; i++) {
            const counter_range *r = &g_array_index(ranges, counter_range, i);
            if (value >= r->low && value <= r->high) return TRUE;
        }
        return FALSE;
    }
    if (!algo) return TRUE;
    switch (algo->system) {
    case COUNTER_SYSTEM_ALPHABETIC:
    case COUNTER_SYSTEM_SYMBOLIC:
        return value >= 1;
    case COUNTER_SYSTEM_ADDITIVE:
        return value >= 0;
    default:
        return TRUE;
    }
}

static gboolean
counter_system_uses_negative(counter_system system)
{
    return system == COUNTER_SYSTEM_SYMBOLIC ||
           system == COUNTER_SYSTEM_ALPHABETIC ||
           system == COUNTER_SYSTEM_NUMERIC ||
           system == COUNTER_SYSTEM_ADDITIVE;
}

static gboolean
counter_digits(const counter_style *algo, guint64 value, GString *out)
{
    guint n = algo->symbols->len;
    const char *digits[72];
    guint len = 0;
    switch (algo->system) {
    case COUNTER_SYSTEM_CYCLIC:
    case COUNTER_SYSTEM_FIXED:
        return FALSE;
    case COUNTER_SYSTEM_SYMBOLIC: {
        guint64 repeat = (value + n - 1) / n;
        if (value == 0 || repeat > NS_COUNTER_STYLE_MAX_REPEAT) return FALSE;
        const char *symbol = g_ptr_array_index(algo->symbols, (value - 1) % n);
        for (guint64 i = 0; i < repeat; i++) g_string_append(out, symbol);
        return TRUE;
    }
    case COUNTER_SYSTEM_ALPHABETIC:
        if (value == 0) return FALSE;
        while (value > 0 && len < G_N_ELEMENTS(digits)) {
            value--;
            digits[len++] = g_ptr_array_index(algo->symbols, value % n);
            value /= n;
        }
        break;
    case COUNTER_SYSTEM_NUMERIC:
        if (value == 0) {
            g_string_append(out, g_ptr_array_index(algo->symbols, 0));
            return TRUE;
        }
        while (value > 0 && len < G_N_ELEMENTS(digits)) {
            digits[len++] = g_ptr_array_index(algo->symbols, value % n);
            value /= n;
        }
        break;
    case COUNTER_SYSTEM_ADDITIVE: {
        if (value == 0) {
            gint64 smallest = g_array_index(algo->weights, gint64, n - 1);
            if (smallest != 0) return FALSE;
            g_string_append(out, g_ptr_array_index(algo->symbols, n - 1));
            return TRUE;
        }
        guint used = 0;
        for (guint i = 0; i < n && value > 0; i++) {
            guint64 weight = (guint64)g_array_index(algo->weights, gint64, i);
            if (weight == 0 || weight > value) continue;
            guint64 repeat = value / weight;
            used += (guint)MIN(repeat, NS_COUNTER_STYLE_MAX_REPEAT + 1);
            if (used > NS_COUNTER_STYLE_MAX_REPEAT) return FALSE;
            for (guint64 k = 0; k < repeat; k++)
                g_string_append(out, g_ptr_array_index(algo->symbols, i));
            value -= repeat * weight;
        }
        return value == 0;
    }
    default:
        return FALSE;
    }
    if (value > 0) return FALSE;
    for (guint i = len; i > 0; i--) g_string_append(out, digits[i - 1]);
    return TRUE;
}

static gboolean
counter_initial_representation(const counter_style *algo, gint64 value,
                               GString *out)
{
    guint n = algo->symbols->len;
    if (algo->system == COUNTER_SYSTEM_CYCLIC) {
        gint64 index = (value - 1) % (gint64)n;
        if (index < 0) index += n;
        g_string_append(out, g_ptr_array_index(algo->symbols, (guint)index));
        return TRUE;
    }
    if (algo->system == COUNTER_SYSTEM_FIXED) {
        if (value < algo->first || value - algo->first >= (gint64)n)
            return FALSE;
        g_string_append(out, g_ptr_array_index(algo->symbols,
                                               (guint)(value - algo->first)));
        return TRUE;
    }
    guint64 magnitude = value < 0 ? (guint64)(-(value + 1)) + 1 : (guint64)value;
    return counter_digits(algo, magnitude, out);
}

static char *
counter_style_representation(const char *name, gint64 value, int depth)
{
    const counter_style *cs = counter_style_lookup(name);
    if (!cs || depth > NS_COUNTER_STYLE_MAX_CHAIN)
        return g_strdup_printf("%" G_GINT64_FORMAT, value);
    const counter_style *algo = counter_style_algorithm(cs);
    GString *digits = g_string_new(NULL);
    gboolean ok = counter_value_in_range(cs, algo, value);
    if (ok && algo)
        ok = counter_initial_representation(algo, value, digits);
    else if (ok)
        g_string_append_printf(digits, "%" G_GINT64_FORMAT,
                               value < 0 ? -value : value);
    if (!ok) {
        g_string_free(digits, TRUE);
        const char *fallback =
            counter_style_inherited(cs, G_STRUCT_OFFSET(counter_style, fallback));
        return counter_style_representation(fallback, value, depth + 1);
    }
    gboolean negative = value < 0 &&
        (!algo || counter_system_uses_negative(algo->system));
    const counter_style *padded = counter_style_with_pad(cs);
    if (padded) {
        glong have = g_utf8_strlen(digits->str, -1) + (negative ? 1 : 0);
        for (glong i = have; i < padded->pad_length; i++)
            g_string_prepend(digits, padded->pad_symbol);
    }
    if (negative) {
        const char *before = counter_style_inherited(
            cs, G_STRUCT_OFFSET(counter_style, negative_before));
        const char *after = counter_style_inherited(
            cs, G_STRUCT_OFFSET(counter_style, negative_after));
        g_string_prepend(digits, before ? before : "-");
        if (after) g_string_append(digits, after);
    }
    return g_string_free(digits, FALSE);
}

char *
ns_css_counter_style_text(const char *name, int value)
{
    g_mutex_lock(&g_counter_styles_lock);
    char *text = counter_style_lookup(name)
        ? counter_style_representation(name, value, 0) : NULL;
    g_mutex_unlock(&g_counter_styles_lock);
    return text;
}

char *
ns_css_counter_style_marker(const char *name, int value)
{
    g_mutex_lock(&g_counter_styles_lock);
    const counter_style *cs = author_counter_style_lookup(name);
    char *marker = NULL;
    if (cs) {
        char *text = counter_style_representation(name, value, 0);
        const char *prefix = counter_style_inherited(
            cs, G_STRUCT_OFFSET(counter_style, prefix));
        const char *suffix = counter_style_inherited(
            cs, G_STRUCT_OFFSET(counter_style, suffix));
        marker = g_strconcat(prefix ? prefix : "", text,
                             suffix ? suffix : ". ", NULL);
        g_free(text);
    }
    g_mutex_unlock(&g_counter_styles_lock);
    return marker;
}
