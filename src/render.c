/* Nordstjernen — shared style/layout pipeline used by GUI and headless.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "render.h"

#include <math.h>
#include <string.h>

#include "engine.h"
#include "font.h"
#include "net.h"
#include "paint.h"

static gboolean g_render_page_uses_hover = FALSE;

gboolean
ns_render_page_uses_hover(void)
{
    return g_render_page_uses_hover;
}

static gboolean g_render_page_uses_active = FALSE;

gboolean
ns_render_page_uses_active(void)
{
    return g_render_page_uses_active;
}

static ns_css_page_rule g_render_page_rule;
static gboolean         g_render_page_rule_set = FALSE;

const ns_css_page_rule *
ns_render_page_rule(void)
{
    return g_render_page_rule_set ? &g_render_page_rule : NULL;
}

typedef struct render_font_usage {
    GArray *codepoints;
    GHashTable *seen;
    guint64 ascii_seen[2];
} render_font_usage;

static void
render_font_usage_free(gpointer data)
{
    render_font_usage *usage = data;
    if (!usage) return;
    g_array_free(usage->codepoints, TRUE);
    g_hash_table_destroy(usage->seen);
    g_free(usage);
}

static gboolean
render_font_list_contains(const char *list, const char *family)
{
    if (!list || !family || !*family) return FALSE;
    const char *p = list;
    while (*p) {
        while (g_ascii_isspace(*p) || *p == ',') p++;
        GString *token = g_string_new(NULL);
        char quote = 0;
        while (*p) {
            char ch = *p++;
            if (quote) {
                if (ch == '\\' && *p) ch = *p++;
                else if (ch == quote) {
                    quote = 0;
                    continue;
                }
            } else {
                if (ch == '\'' || ch == '"') {
                    quote = ch;
                    continue;
                }
                if (ch == ',') break;
                if (ch == '\\' && *p) ch = *p++;
            }
            g_string_append_c(token, ch);
        }
        g_strstrip(token->str);
        gboolean match = g_ascii_strcasecmp(token->str, family) == 0;
        g_string_free(token, TRUE);
        if (match) return TRUE;
    }
    return FALSE;
}

static void
render_font_usage_add_text(render_font_usage *usage, const char *text)
{
    const char *p = text;
    while (p && *p) {
        gunichar cp = g_utf8_get_char_validated(p, -1);
        if (cp == (gunichar)-1 || cp == (gunichar)-2) {
            p++;
            continue;
        }
        if (cp < 128) {
            guint64 bit = G_GUINT64_CONSTANT(1) << (cp & 63);
            if (usage->ascii_seen[cp >> 6] & bit) {
                p++;
                continue;
            }
            usage->ascii_seen[cp >> 6] |= bit;
        }
        gpointer key = GUINT_TO_POINTER(cp + 1u);
        if (g_hash_table_add(usage->seen, key))
            g_array_append_val(usage->codepoints, cp);
        p = g_utf8_next_char(p);
    }
}

static __thread GHashTable *g_font_list_matches;

static GPtrArray *
render_font_list_matches(const char *list, GHashTable *families)
{
    GPtrArray *hit = g_font_list_matches
        ? g_hash_table_lookup(g_font_list_matches, list) : NULL;
    if (hit) return hit;
    hit = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, families);
    while (g_hash_table_iter_next(&iter, &key, &val))
        if (render_font_list_contains(list, key))
            g_ptr_array_add(hit, val);
    if (g_font_list_matches)
        g_hash_table_insert(g_font_list_matches, (gpointer)list, hit);
    return hit;
}

static void
render_font_usage_add_styled(const ns_style *style, const char *text,
                             GHashTable *families)
{
    const ns_css_value *value = style
        ? style->values[NS_CSS_FONT_FAMILY] : NULL;
    const char *list = value && value->kind == NS_CSS_V_KEYWORD
        ? value->u.keyword : NULL;
    if (!list || g_hash_table_size(families) == 0) return;
    GPtrArray *matches = render_font_list_matches(list, families);
    for (guint i = 0; i < matches->len; i++)
        render_font_usage_add_text(g_ptr_array_index(matches, i), text);
    if (!g_font_list_matches) g_ptr_array_free(matches, TRUE);
}

static void
render_collect_pseudo_font_usage(const ns_node *host, const ns_style *pseudo,
                                 GHashTable *families)
{
    if (!pseudo) return;
    char *text = ns_layout_pseudo_content_text(pseudo->values[NS_CSS_CONTENT],
                                               host);
    if (text && *text) render_font_usage_add_styled(pseudo, text, families);
    g_free(text);
}

static void
render_collect_font_usage(const ns_node *root, GHashTable *styles,
                          GHashTable *families)
{
    const ns_node *node = root;
    while (node) {
        gboolean descend = TRUE;
        if (node->kind == NS_NODE_ELEMENT) {
            const ns_style *style = g_hash_table_lookup(styles, node);
            descend = !ns_display_is_none(ns_css_display_of(style));
            if (descend && style) {
                render_collect_pseudo_font_usage(node, style->before, families);
                render_collect_pseudo_font_usage(node, style->after, families);
            }
        } else if (node->kind == NS_NODE_TEXT && node->text && *node->text &&
                   node->parent) {
            render_font_usage_add_styled(g_hash_table_lookup(styles,
                                                             node->parent),
                                         node->text, families);
        }
        node = ns_node_next_in_subtree(node, root, descend);
    }
}

static gboolean
render_unicode_range_parse(const char *start, const char *end,
                           gunichar *out_start, gunichar *out_end)
{
    while (start < end && g_ascii_isspace(*start)) start++;
    while (end > start && g_ascii_isspace(end[-1])) end--;
    if (end - start < 3 || g_ascii_tolower(start[0]) != 'u' ||
        start[1] != '+')
        return FALSE;
    start += 2;
    guint32 lo = 0;
    guint32 hi = 0;
    guint digits = 0;
    guint wildcards = 0;
    while (start < end && digits < 6) {
        int hex = g_ascii_xdigit_value(*start);
        if (hex >= 0) {
            if (wildcards) return FALSE;
            lo = (lo << 4) | (guint32)hex;
            hi = (hi << 4) | (guint32)hex;
        } else if (*start == '?') {
            wildcards++;
            lo <<= 4;
            hi = (hi << 4) | 0xFu;
        } else {
            break;
        }
        digits++;
        start++;
    }
    if (!digits) return FALSE;
    if (wildcards) {
        if (start != end) return FALSE;
    } else if (start < end && *start == '-') {
        start++;
        guint32 range_end = 0;
        guint end_digits = 0;
        while (start < end && end_digits < 6) {
            int hex = g_ascii_xdigit_value(*start);
            if (hex < 0) break;
            range_end = (range_end << 4) | (guint32)hex;
            end_digits++;
            start++;
        }
        if (!end_digits || start != end) return FALSE;
        hi = range_end;
    } else if (start != end) {
        return FALSE;
    }
    if (lo > hi || lo > 0x10FFFFu) return FALSE;
    if (hi > 0x10FFFFu) hi = 0x10FFFFu;
    *out_start = (gunichar)lo;
    *out_end = (gunichar)hi;
    return TRUE;
}

static gboolean
render_unicode_range_matches(const char *range,
                             const render_font_usage *usage)
{
    if (!usage || usage->codepoints->len == 0) return FALSE;
    if (!range || !*range) return TRUE;
    gboolean valid = FALSE;
    const char *part = range;
    while (*part) {
        const char *end = strchr(part, ',');
        if (!end) end = part + strlen(part);
        gunichar lo = 0, hi = 0;
        if (render_unicode_range_parse(part, end, &lo, &hi)) {
            valid = TRUE;
            for (guint i = 0; i < usage->codepoints->len; i++) {
                gunichar cp = g_array_index(usage->codepoints, gunichar, i);
                if (cp >= lo && cp <= hi) return TRUE;
            }
        }
        part = *end ? end + 1 : end;
    }
    return !valid;
}

static void
render_feed_animations(const ns_render_ctx *c, GHashTable *styles)
{
    if (!c->anim) return;
    for (guint i = 0; i < c->n_sheets; i++)
        if (c->sheets[i]) ns_anim_load_from_stylesheet(c->anim, c->sheets[i]);
    ns_anim_observe_all(c->anim, styles, g_get_monotonic_time());
}

static void
render_request_fonts(const ns_render_ctx *c, GHashTable *styles)
{
    if (!ns_font_available()) return;
    GHashTable *families = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, render_font_usage_free);
    for (guint i = 0; i < c->n_sheets; i++) {
        const ns_css_stylesheet *sh = c->sheets[i];
        if (!sh || !sh->font_faces) continue;
        for (guint j = 0; j < sh->font_faces->len; j++) {
            const ns_css_font_face *ff =
                &g_array_index(sh->font_faces, ns_css_font_face, j);
            if (!ff->family || !*ff->family ||
                g_hash_table_contains(families, ff->family))
                continue;
            render_font_usage *usage = g_new0(render_font_usage, 1);
            usage->codepoints = g_array_new(FALSE, FALSE, sizeof(gunichar));
            usage->seen = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_hash_table_insert(families, g_strdup(ff->family), usage);
        }
    }
    if (g_hash_table_size(families) == 0) {
        g_hash_table_destroy(families);
        return;
    }
    g_font_list_matches = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL,
        (GDestroyNotify)g_ptr_array_unref);
    render_collect_font_usage(c->doc, styles, families);
    g_clear_pointer(&g_font_list_matches, g_hash_table_destroy);
    for (guint i = 0; i < c->n_sheets; i++) {
        const ns_css_stylesheet *sh = c->sheets[i];
        if (!sh || !sh->font_faces) continue;
        for (guint j = 0; j < sh->font_faces->len; j++) {
            const ns_css_font_face *ff =
                &g_array_index(sh->font_faces, ns_css_font_face, j);
            if (!ff->family || !ff->src_url) continue;
            render_font_usage *usage = g_hash_table_lookup(families,
                                                           ff->family);
            if (!render_unicode_range_matches(ff->unicode_range, usage))
                continue;
            char *abs = c->resolve_url
                ? c->resolve_url(ff->src_url, c->cb_ud)
                : ns_url_resolve(c->base_url, ff->src_url);
            if (!abs) continue;
            if (c->font_allowed && !c->font_allowed(abs, c->cb_ud)) {
                g_free(abs);
                continue;
            }
            const ns_node *sheet_doc = c->sheet_docs ? c->sheet_docs[i] : NULL;
            if (sheet_doc && sheet_doc != c->doc &&
                !ns_engine_frame_csp_allows(sheet_doc, NS_CSP_FONT, abs)) {
                g_free(abs);
                continue;
            }
            ns_font_request(ff->family, abs, c->base_url, ff->descriptors);
            g_free(abs);
        }
    }
    g_hash_table_destroy(families);
}

static void
render_apply_zoom(const ns_render_ctx *c, GHashTable *styles)
{
    double zoom = c->zoom > 0 ? c->zoom : 1.0;
    if (fabs(zoom - 1.0) <= 0.001) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val))
        ns_css_style_scale_font_size(val, zoom);
}

static guint64
render_container_entry_sig(const ns_box *b, const char *type, const char *names)
{
    gboolean queries_block = g_ascii_strcasecmp(type, "size") == 0;
    guint64 h = 1469598103934665603ULL;
    const guint64 parts[] = {
        (guint64)(guintptr)b->dom,
        g_str_hash(type),
        names ? g_str_hash(names) : 0,
        (guint64)(gint64)(b->content_width * 64.0),
        queries_block ? (guint64)(gint64)(b->content_height * 64.0) : 0,
    };
    for (gsize i = 0; i < G_N_ELEMENTS(parts); i++) {
        h ^= parts[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void
render_collect_containers(const ns_box *b, GHashTable *map, guint64 *sig)
{
    if (!b) return;
    if (b->dom && b->style) {
        const ns_css_value *ct = b->style->values[NS_CSS_CONTAINER_TYPE];
        if (ct && ct->kind == NS_CSS_V_KEYWORD && ct->u.keyword &&
            g_ascii_strcasecmp(ct->u.keyword, "normal") != 0) {
            const ns_css_value *nm = b->style->values[NS_CSS_CONTAINER_NAME];
            const char *names = (nm && nm->kind == NS_CSS_V_KEYWORD)
                ? nm->u.keyword : NULL;
            const ns_css_value *wm = b->style->values[NS_CSS_WRITING_MODE];
            gboolean vertical = wm && wm->kind == NS_CSS_V_KEYWORD &&
                wm->u.keyword && g_str_has_prefix(wm->u.keyword, "vertical");
            ns_css_container_map_add(map, b->dom, ct->u.keyword, names,
                                     b->content_width, b->content_height,
                                     vertical);
            if (sig) *sig += render_container_entry_sig(b, ct->u.keyword, names);
        }
    }
    for (const ns_box *ch = b->first_child; ch; ch = ch->next_sibling)
        render_collect_containers(ch, map, sig);
    for (guint i = 0; b->inline_atomics && i < b->inline_atomics->len; i++)
        render_collect_containers(
            g_array_index(b->inline_atomics, ns_inline_atomic, i).box, map, sig);
}

static const ns_node *g_cq_prev_doc;
static double g_cq_prev_width;
static GHashTable *g_cq_prev_map;
static gboolean g_cq_prev_settled;

static GHashTable *
render_cq_predicted_map(const ns_node *doc, double viewport_width,
                        gboolean want_cq)
{
    if (!want_cq || !g_cq_prev_map || g_cq_prev_doc != doc ||
        g_cq_prev_width != viewport_width)
        return NULL;
    return g_cq_prev_map;
}

static gboolean
render_cq_settled(GHashTable *predicted, GHashTable *measured)
{
    g_cq_prev_settled = predicted &&
                        ns_css_container_maps_equal(predicted, measured);
    return g_cq_prev_settled;
}

static void
render_cq_remember(const ns_node *doc, double viewport_width,
                   const ns_box *layout, gboolean want_cq)
{
    g_clear_pointer(&g_cq_prev_map, g_hash_table_destroy);
    g_cq_prev_doc = NULL;
    if (!want_cq) return;
    g_cq_prev_map = ns_css_container_map_new();
    render_collect_containers(layout, g_cq_prev_map, NULL);
    g_cq_prev_doc = doc;
    g_cq_prev_width = viewport_width;
}

static gboolean
render_dom_uses_container_units(const ns_node *root)
{
    for (const ns_node *node = root; node;
         node = ns_node_next_in_subtree(node, root, TRUE)) {
        if (node->kind != NS_NODE_ELEMENT) continue;
        const char *style = ns_element_get_attr(node, "style");
        if (ns_css_text_has_container_units(style, -1)) return TRUE;
    }
    return FALSE;
}

static gboolean
render_cq_wanted(const ns_render_ctx *c, gboolean *uses_units)
{
    gboolean units = render_dom_uses_container_units(c->doc);
    gboolean want = units;
    for (guint i = 0; i < c->n_sheets; i++) {
        if (ns_css_stylesheet_has_container_units(c->sheets[i]))
            units = want = TRUE;
        if (ns_css_stylesheet_has_container_rules(c->sheets[i]))
            want = TRUE;
    }
    *uses_units = units;
    return want;
}

static gboolean
render_selector_cache_wanted(const ns_render_ctx *c, GHashTable *predicted)
{
    if (predicted && g_cq_prev_settled) return FALSE;
    for (guint i = 0; i < c->n_sheets; i++)
        if (ns_css_stylesheet_has_container_rules(c->sheets[i]) ||
            ns_css_stylesheet_has_container_units(c->sheets[i]))
            return TRUE;
    return FALSE;
}

static void
render_style_pass(const ns_render_ctx *c, GHashTable *styles)
{
    render_feed_animations(c, styles);
    render_request_fonts(c, styles);
    render_apply_zoom(c, styles);
}

static gboolean
render_value_equal(const ns_css_value *a, const ns_css_value *b)
{
    if (a == b) return TRUE;
    if (!a || !b || a->kind != b->kind) return FALSE;
    gboolean equal = FALSE;
    switch (a->kind) {
    case NS_CSS_V_KEYWORD:
        equal = g_strcmp0(a->u.keyword, b->u.keyword) == 0;
        break;
    case NS_CSS_V_LENGTH:
        equal = a->u.length.v == b->u.length.v &&
                a->u.length.unit == b->u.length.unit;
        break;
    case NS_CSS_V_SIZE:
        equal = a->u.size.w == b->u.size.w &&
                a->u.size.h == b->u.size.h &&
                a->u.size.w_unit == b->u.size.w_unit &&
                a->u.size.h_unit == b->u.size.h_unit &&
                a->u.size.w_auto == b->u.size.w_auto &&
                a->u.size.h_auto == b->u.size.h_auto;
        break;
    case NS_CSS_V_COLOR:
        equal = a->u.color.r == b->u.color.r &&
                a->u.color.g == b->u.color.g &&
                a->u.color.b == b->u.color.b &&
                a->u.color.a == b->u.color.a;
        break;
    case NS_CSS_V_CALC:
        equal = a->u.calc.pct == b->u.calc.pct &&
                a->u.calc.px == b->u.calc.px &&
                a->u.calc.em == b->u.calc.em &&
                a->u.calc.rem == b->u.calc.rem;
        break;
    case NS_CSS_V_URL:
        equal = g_strcmp0(a->u.url, b->u.url) == 0;
        break;
    case NS_CSS_V_RECT:
        equal = TRUE;
        for (int i = 0; i < 4 && equal; i++)
            equal = a->u.rect.v[i] == b->u.rect.v[i] &&
                    a->u.rect.unit[i] == b->u.rect.unit[i] &&
                    a->u.rect.is_auto[i] == b->u.rect.is_auto[i];
        break;
    default:
        return FALSE;
    }
    return equal && render_value_equal(a->next_layer, b->next_layer);
}

static gboolean
render_vars_equal(const struct ns_var_map *a, const struct ns_var_map *b)
{
    if (a == b) return TRUE;
    GPtrArray *an = ns_var_map_names(a);
    GPtrArray *bn = ns_var_map_names(b);
    gboolean equal = an->len == bn->len;
    for (guint i = 0; equal && i < an->len; i++) {
        const char *name_a = g_ptr_array_index(an, i);
        const char *name_b = g_ptr_array_index(bn, i);
        equal = strcmp(name_a, name_b) == 0 &&
                g_strcmp0(ns_var_map_lookup(a, name_a),
                          ns_var_map_lookup(b, name_b)) == 0;
    }
    g_ptr_array_unref(an);
    g_ptr_array_unref(bn);
    return equal;
}

static gboolean
render_style_equal(const ns_style *a, const ns_style *b)
{
    if (a == b) return TRUE;
    if (!a || !b || a->display.box != b->display.box ||
        a->display.outer != b->display.outer ||
        a->display.inner != b->display.inner ||
        a->display.internal != b->display.internal ||
        a->display.list_item != b->display.list_item)
        return FALSE;
    for (int i = 0; i < NS_CSS_PROP_COUNT; i++)
        if (!render_value_equal(a->values[i], b->values[i])) return FALSE;
    return render_vars_equal(a->vars, b->vars) &&
           render_style_equal(a->before, b->before) &&
           render_style_equal(a->after, b->after) &&
           render_style_equal(a->first_letter, b->first_letter) &&
           render_style_equal(a->first_line, b->first_line) &&
           render_style_equal(a->placeholder, b->placeholder) &&
           render_style_equal(a->selection, b->selection) &&
           render_style_equal(a->marker, b->marker) &&
           render_style_equal(a->backdrop, b->backdrop) &&
           render_style_equal(a->file_selector_button,
                              b->file_selector_button);
}

static gboolean
render_style_tables_equal(GHashTable *a, GHashTable *b)
{
    if (g_hash_table_size(a) != g_hash_table_size(b)) return FALSE;
    GHashTableIter iter;
    gpointer node, style;
    g_hash_table_iter_init(&iter, a);
    while (g_hash_table_iter_next(&iter, &node, &style)) {
        ns_style *other = g_hash_table_lookup(b, node);
        if (!render_style_equal(style, other)) return FALSE;
    }
    return TRUE;
}

#ifdef NS_MOBILE
static const ns_node *
render_find_viewport_meta(const ns_node *n, int depth)
{
    if (!n || depth >= 512) return NULL;
    if (ns_node_is_element_named(n, "meta")) {
        const char *name = ns_element_get_attr(n, "name");
        if (name && g_ascii_strcasecmp(name, "viewport") == 0)
            return n;
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *found = render_find_viewport_meta(c, depth + 1);
        if (found) return found;
    }
    return NULL;
}

static double
render_parse_viewport_width(const char *content)
{
    if (!content || !*content) return 0;
    double out = 0;
    char **parts = g_strsplit_set(content, ",;", -1);
    for (int i = 0; parts && parts[i]; i++) {
        char *part = g_strstrip(parts[i]);
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = g_strstrip(part);
        char *value = g_strstrip(eq + 1);
        if (g_ascii_strcasecmp(key, "width") != 0) continue;
        if (g_ascii_strcasecmp(value, "device-width") == 0) break;
        char *end = NULL;
        double n = g_ascii_strtod(value, &end);
        if (end != value && n >= 320 && n <= 4096) {
            out = n;
            break;
        }
    }
    g_strfreev(parts);
    return out;
}

static double
render_effective_viewport_width(const ns_render_ctx *c)
{
    double width = c->viewport_width;
    const ns_node *meta = render_find_viewport_meta(c->doc, 0);
    const char *content = meta ? ns_element_get_attr(meta, "content") : NULL;
    double hint = render_parse_viewport_width(content);
    if (hint > width) width = hint;
    return width;
}
#else
static double
render_effective_viewport_width(const ns_render_ctx *c)
{
    return c->viewport_width;
}
#endif

static double
render_pass_begin(const ns_render_ctx *c)
{
    double viewport_width = render_effective_viewport_width(c);
    ns_css_set_viewport(viewport_width, c->viewport_height);
    ns_css_set_focus_node(c->focused_input);
    ns_css_set_hover_node(c->hover_node);

    g_render_page_rule_set = FALSE;
    for (guint i = 0; i < c->n_sheets; i++)
        if (c->sheets[i] && c->sheets[i]->page_rule) {
            g_render_page_rule = *c->sheets[i]->page_rule;
            g_render_page_rule_set = TRUE;
        }

    gboolean uses_hover = FALSE;
    for (guint i = 0; i < c->n_sheets && !uses_hover; i++)
        uses_hover = ns_css_stylesheet_has_hover_rules(c->sheets[i]);
    g_render_page_uses_hover = uses_hover;

    gboolean uses_active = FALSE;
    for (guint i = 0; i < c->n_sheets && !uses_active; i++)
        uses_active = ns_css_stylesheet_has_active_rules(c->sheets[i]);
    g_render_page_uses_active = uses_active;
    return viewport_width;
}

GHashTable *
ns_render_restyle(const ns_render_ctx *c)
{
    if (!c) return NULL;
    double viewport_width = render_pass_begin(c);
    ns_css_set_render_zoom(c->zoom > 0 ? c->zoom : 1.0);
    gboolean uses_cq_units = FALSE;
    gboolean want_cq = render_cq_wanted(c, &uses_cq_units);
    GHashTable *predicted =
        render_cq_predicted_map(c->doc, viewport_width, want_cq);
    gboolean cache_selectors = render_selector_cache_wanted(c, predicted);
    if (cache_selectors) ns_css_selector_cache_begin();
    ns_css_set_container_map(predicted);
    GHashTable *styles = ns_css_compute(c->doc, c->sheets, c->sheet_docs,
                                        c->n_sheets);
    ns_css_set_container_map(NULL);
    render_style_pass(c, styles);
    if (cache_selectors) ns_css_selector_cache_end();
    ns_css_set_focus_node(NULL);
    ns_css_set_hover_node(NULL);
    if (c->js) ns_js_set_style_table(c->js, styles);
    return styles;
}

GHashTable *
ns_render_relayout_profile(const ns_render_ctx *c, ns_box **out_layout,
                           ns_render_profile *profile)
{
    if (out_layout) *out_layout = NULL;
    if (!c || !out_layout) return NULL;
    if (profile) memset(profile, 0, sizeof *profile);

    double viewport_width = render_pass_begin(c);

    gint64 t0 = profile ? g_get_monotonic_time() : 0;
    ns_css_set_render_zoom(c->zoom > 0 ? c->zoom : 1.0);
    gboolean uses_cq_units = FALSE;
    gboolean want_cq = render_cq_wanted(c, &uses_cq_units);
    GHashTable *predicted =
        render_cq_predicted_map(c->doc, viewport_width, want_cq);
    gboolean cache_selectors = render_selector_cache_wanted(c, predicted);
    if (cache_selectors) ns_css_selector_cache_begin();
    ns_css_set_container_map(predicted);
    GHashTable *styles = ns_css_compute(c->doc, c->sheets, c->sheet_docs, c->n_sheets);
    ns_css_set_container_map(NULL);
    gint64 t1 = profile ? g_get_monotonic_time() : 0;

    render_style_pass(c, styles);
    gint64 t2 = profile ? g_get_monotonic_time() : 0;

    ns_paint_list_ordinals_begin();
    ns_box *layout = ns_layout_build(c->doc, styles, viewport_width,
                                     c->focused_input, c->caret_byte,
                                     c->sel_anchor_byte,
                                     c->images, c->base_url);
    ns_paint_list_ordinals_end();
    gint64 t3 = profile ? g_get_monotonic_time() : 0;
    if (profile) {
        profile->css1_us = t1 - t0;
        profile->style1_us = t2 - t1;
        profile->layout1_us = t3 - t2;
    }

    GHashTable *containers = ns_css_container_map_new();
    guint64 container_sig = 0;
    gint64 tc0 = profile ? g_get_monotonic_time() : 0;
    if (want_cq) render_collect_containers(layout, containers, &container_sig);
    gint64 tc1 = profile ? g_get_monotonic_time() : 0;
    guint n_containers = g_hash_table_size(containers);
    if (profile) {
        profile->container_us = tc1 - tc0;
        profile->containers = n_containers;
    }
    int container_passes = uses_cq_units ? 3 : 1;
    gboolean stale = predicted != NULL;
    if (render_cq_settled(predicted, containers)) container_passes = 0;
    for (int pass = 0; pass < container_passes && (n_containers > 0 || stale);
         pass++) {
        if (profile) {
            profile->container_pass = TRUE;
            profile->container_passes++;
        }
        ns_css_set_container_map(containers);
        ns_css_container_features_begin();
        gint64 t4 = profile ? g_get_monotonic_time() : 0;
        GHashTable *styles2 = ns_css_compute(c->doc, c->sheets, c->sheet_docs, c->n_sheets);
        gint64 t5 = profile ? g_get_monotonic_time() : 0;
        gboolean container_features_used = ns_css_container_features_used();
        ns_css_set_container_map(NULL);
        if ((!container_features_used && !stale) ||
            render_style_tables_equal(styles, styles2)) {
            if (profile) profile->css2_us += t5 - t4;
            g_hash_table_destroy(styles2);
            break;
        }
        render_style_pass(c, styles2);
        gint64 t6 = profile ? g_get_monotonic_time() : 0;
        ns_paint_list_ordinals_begin();
        ns_box *layout2 = ns_layout_build(c->doc, styles2, viewport_width,
                                          c->focused_input, c->caret_byte,
                                          c->sel_anchor_byte,
                                          c->images, c->base_url);
        ns_paint_list_ordinals_end();
        gint64 t7 = profile ? g_get_monotonic_time() : 0;
        if (profile) {
            profile->css2_us += t5 - t4;
            profile->style2_us += t6 - t5;
            profile->layout2_us += t7 - t6;
        }
        ns_box_free(layout);
        g_hash_table_destroy(styles);
        layout = layout2;
        styles = styles2;
        stale = FALSE;
        if (pass + 1 < container_passes) {
            g_hash_table_remove_all(containers);
            gint64 tr0 = profile ? g_get_monotonic_time() : 0;
            guint64 next_sig = 0;
            render_collect_containers(layout, containers, &next_sig);
            gint64 tr1 = profile ? g_get_monotonic_time() : 0;
            n_containers = g_hash_table_size(containers);
            if (profile) {
                profile->container_us += tr1 - tr0;
                profile->containers = n_containers;
            }
            if (next_sig == container_sig) break;
            container_sig = next_sig;
        }
    }
    g_hash_table_destroy(containers);
    render_cq_remember(c->doc, viewport_width, layout, want_cq);
    if (cache_selectors) ns_css_selector_cache_end();
    ns_css_set_focus_node(NULL);
    ns_css_set_hover_node(NULL);

    if (c->js) {
        ns_js_set_style_table(c->js, styles);
        ns_js_set_layout_root(c->js, layout);
    }
    *out_layout = layout;
    return styles;
}

GHashTable *
ns_render_relayout(const ns_render_ctx *c, ns_box **out_layout)
{
    return ns_render_relayout_profile(c, out_layout, NULL);
}
