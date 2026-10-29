/* Nordstjernen — public C embedding API implementation.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "libnordstjernen.h"
#include "media_os.h"
#include "layers.h"

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "anim.h"
#include "bytecode_cache.h"
#include "cache.h"
#include "debuglog.h"
#include "history.h"
#include "config.h"
#include "css.h"
#include "dom.h"
#include "engine.h"
#include "font.h"
#include "forms.h"
#include "html.h"
#include "image.h"
#include "js.h"
#include "layout.h"
#include "net.h"
#include "spellcheck.h"
#include "paint.h"
#include "print.h"
#include "pdf.h"
#include "render.h"
#include "safebrowsing.h"
#include "security.h"
#include "selection.h"
#include "svg_external.h"
#include "video.h"
#include "camera.h"
#include "webgl.h"
#include "glctx.h"

#define NS_IMAGE_RELAYOUT_BATCH 8

struct ns_browser {
    ns_node        *doc;
    ns_box         *layout;
    GHashTable     *styles;
    GHashTable     *retired_styles;
    guint64         styles_serial;
    ns_js          *js;
    ns_anim        *anim;
    ns_image_cache *images;
    ns_video_cache *videos;
    GHashTable     *css_cache;
    char           *base_url;
    char           *doc_charset;
    char           *doc_language;
    int             vw;
    double          vh;
    gboolean        images_fetched;
    gboolean        has_deferred_lazy;
    gboolean        bfcache_ok;
    double          cur_scroll_x;
    double          cur_scroll_y;
    double          cur_scale;
    double          js_scroll_x;
    double          js_scroll_y;
    double          cur_viewport_h;
    int             pending_scroll_x;
    int             pending_scroll_y;
    gboolean        pending_scroll;
    const ns_node  *scroll_anchor;
    int             scroll_anchor_y;
    GPtrArray      *img_sessions;
    guint           image_arrivals_since_layout;
    guint           media_events_source;
    gboolean        images_arrived_since_layout;
    gint64          load_delay_deadline_us;
    GHashTable     *img_requested;
    gboolean        dirty;
    gboolean        repaint_requested;
    double          dppx;
    gboolean        cascade_dirty;
    gboolean        anim_paint_dirty;
    gboolean        scroll_pass;
    gboolean        relaying;
    char           *pending_nav;
    gboolean        soft_nav_pushed;
    char           *pending_download;
    char           *pending_clipboard;
    char           *pending_window_action;
    GString        *pending_audio;
    char           *refresh_url;
    gint64          refresh_due_us;
    char           *pending_post_body;
    gsize           pending_post_len;
    char           *pending_post_ct;
    gsize           caret_byte;
    gsize           sel_anchor_byte;
    const ns_node  *caret_blink_node;
    gsize           caret_blink_byte;
    gsize           caret_blink_anchor;
    gint64          caret_blink_epoch_us;
    gboolean        caret_blink_active;
    gboolean        caret_paint_visible;
    ns_selection    selection;
    gboolean        selection_dragged;
    const ns_node  *hover_node;
    const ns_node  *open_select;
    gboolean        datalist_suppressed;
    const ns_node  *press_node;
    ns_node        *range_drag;
    char           *range_drag_start;
    int             press_x;
    int             press_y;
    int             press_mods;
    gboolean        press_active;
    int             buttons_down;
    const ns_node  *aux_press_node;
    int             aux_press_button;
    int             aux_press_mods;
    gboolean        keydown_prevented;
    gboolean        key_text_suppressed;
    char           *search_query;
    gboolean        search_case;
    const ns_box   *search_active;
    GString        *console_buf;
    guint64         layout_sig[2];
    int             layout_osc;
    gint64          last_layout_us;
    gint64          damp_until_us;
    gboolean        damp_logged;
    gint64          hover_relayout_us;
    gint64          relayout_cost_us;
    guint           layout_seq;
    guint           page_size_seq;
    int             page_size_vw;
    double          page_size_vh;
    int             page_size_w;
    int             page_size_h;
    guint           videos_noted_seq;
    guint64         svg_external_seen;
    gboolean        hover_restyle_pending;
    ns_box         *sb_box;
    const ns_node  *sb_node;
    double          sb_grab;
    gboolean        sb_dragging;
    int             security;
    char           *remote_ip;
};

#define NS_LAYOUT_OSC_THRESHOLD 6
#define NS_LAYOUT_RAPID_US (250 * 1000)
#define NS_LAYOUT_DAMP_US (700 * 1000)
#define NS_LAYOUT_BACKGROUND_MIN_US (16 * 1000)
#define NS_LAYOUT_BACKGROUND_MAX_US (120 * 1000)
#define NS_CARET_BLINK_US (530 * 1000)
#define NS_CARET_BLINK_TIMEOUT_US (10 * G_USEC_PER_SEC)

static gboolean
browser_doc_has_node(const ns_node *root, const ns_node *target)
{
    for (const ns_node *n = root; n; n = ns_node_next_in_subtree(n, root, TRUE))
        if (n == target) return TRUE;
    return FALSE;
}

static gboolean
browser_node_alive(const ns_browser *browser, const ns_node *node)
{
    if (!node || !browser || !browser->doc) return FALSE;
    return browser_doc_has_node(browser->doc, node);
}

static void
browser_prune_cached_nodes(ns_browser *browser)
{
    if (!browser || !browser->doc) return;
    if (browser->press_node && !browser_node_alive(browser, browser->press_node))
        browser->press_node = NULL;
    if (browser->hover_node && !browser_node_alive(browser, browser->hover_node))
        browser->hover_node = NULL;
    if (browser->aux_press_node &&
        !browser_node_alive(browser, browser->aux_press_node))
        browser->aux_press_node = NULL;
    if (browser->open_select && !browser_node_alive(browser, browser->open_select))
        browser->open_select = NULL;
    if (browser->range_drag && !browser_node_alive(browser, browser->range_drag)) {
        browser->range_drag = NULL;
        g_clear_pointer(&browser->range_drag_start, g_free);
    }
}

static guint64
layout_signature_walk(const ns_box *b, guint64 h)
{
    if (!b) return h;
    gint32 q[4] = {
        (gint32)(b->x * 4), (gint32)(b->y * 4),
        (gint32)(b->content_width * 4), (gint32)(b->content_height * 4),
    };
    const guchar *bytes = (const guchar *)q;
    h ^= (guint64)b->kind;
    h *= 0x100000001b3ULL;
    for (gsize i = 0; i < sizeof q; i++) {
        h ^= bytes[i];
        h *= 0x100000001b3ULL;
    }
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        h = layout_signature_walk(c, h);
    if (b->inline_atomics)
        for (guint i = 0; i < b->inline_atomics->len; i++)
            h = layout_signature_walk(
                g_array_index(b->inline_atomics, ns_inline_atomic, i).box, h);
    return h;
}

static guint64
layout_signature(const ns_box *root)
{
    return layout_signature_walk(root, 0xcbf29ce484222325ULL);
}

static void
browser_damp_reset(ns_browser *b)
{
    b->layout_osc = 0;
    b->damp_until_us = 0;
    b->damp_logged = FALSE;
}

static void browser_ensure_images(ns_browser *browser);
static void browser_schedule_media_events(ns_browser *b);
static void browser_relayout_scroll_pass(ns_browser *b);

static void
browser_relayout(ns_browser *b)
{
    if (b->relaying) { b->dirty = TRUE; return; }
    b->relaying = TRUE;
    b->cascade_dirty = FALSE;
    b->anim_paint_dirty = FALSE;
    if (b->js)
        (void)ns_js_consume_mutated(b->js);
    b->image_arrivals_since_layout = 0;
    GHashTable *scroll_save = ns_box_scroll_save(b->layout);
    ns_css_set_viewport((double)b->vw, b->vh);
    ns_css_set_doc_language(b->doc_language);
    if (b->js) ns_js_sync_window_metrics(b->js);
    b->search_active = NULL;
    ns_selection_mark selection_mark;
    ns_selection_save(b->layout, &b->selection, &selection_mark);
    ns_selection_clear(&b->selection);
    if (b->js && b->layout) ns_js_set_layout_root(b->js, NULL);
    if (b->layout) {
        ns_paint_3d_invalidate();
        ns_box_free(b->layout);
        b->layout = NULL;
        b->sb_box = NULL;
        b->sb_node = NULL;
        b->sb_dragging = FALSE;
    }
    g_clear_pointer(&b->retired_styles, g_hash_table_destroy);
    if (b->js && b->styles) ns_js_set_style_table(b->js, NULL);
    if (b->styles) { g_hash_table_destroy(b->styles); b->styles = NULL; }
    browser_prune_cached_nodes(b);
    ns_layout_set_open_select(b->open_select);
    {
        const ns_node *fn = b->js ? ns_js_focused_node(b->js) : NULL;
        gboolean dl_open = !b->datalist_suppressed && fn &&
                           ns_node_is_element_named(fn, "input") &&
                           ns_element_get_attr(fn, "list") != NULL;
        ns_layout_set_datalist_open(dl_open);
    }
    gint64 relayout_t0 = g_get_monotonic_time();
    b->styles = ns_engine_relayout(b->doc, b->base_url, b->vw, b->vh,
                                   b->images, b->anim, b->js,
                                   b->css_cache,
                                   b->js ? ns_js_focused_node(b->js) : NULL,
                                   b->hover_node,
                                   b->caret_byte, b->sel_anchor_byte,
                                   &b->layout);
    b->relayout_cost_us = g_get_monotonic_time() - relayout_t0;
    b->layout_seq++;
    b->relaying = FALSE;
    b->styles_serial = b->js ? ns_js_mutation_serial(b->js) : 0;
    ns_box_scroll_restore(b->layout, scroll_save);
    ns_selection_restore(b->layout, &selection_mark, &b->selection);
    b->images_fetched = FALSE;
    b->has_deferred_lazy = FALSE;
    b->images_arrived_since_layout = FALSE;
    if (b->js) {
        ns_js_set_style_table(b->js, b->styles);
        ns_js_set_layout_root(b->js, b->layout);
        browser_ensure_images(b);
        browser_schedule_media_events(b);
    }

    gint64 now = g_get_monotonic_time();
    gboolean rapid = now - b->last_layout_us < NS_LAYOUT_RAPID_US;
    b->last_layout_us = now;
    guint64 sig = layout_signature(b->layout);
    if (sig == b->layout_sig[0] || sig == b->layout_sig[1]) {
        if (rapid && b->layout_osc < G_MAXINT) b->layout_osc++;
    } else {
        browser_damp_reset(b);
    }
    b->layout_sig[1] = b->layout_sig[0];
    b->layout_sig[0] = sig;
    browser_relayout_scroll_pass(b);
}

static gboolean
browser_sync_scroll_timelines(ns_browser *b)
{
    gboolean layout = FALSE;
    double view_h = b->cur_viewport_h > 0.0 ? b->cur_viewport_h : b->vh;
    if (!ns_engine_anim_scroll_sync(b->anim, b->layout, b->vw, view_h, &layout))
        return FALSE;
    b->anim_paint_dirty = TRUE;
    b->dirty = b->dirty || layout;
    return TRUE;
}

static void
browser_scrolled(ns_browser *b)
{
    ns_box_note_scroll();
    browser_sync_scroll_timelines(b);
}

static void
browser_relayout_scroll_pass(ns_browser *b)
{
    if (b->scroll_pass || !ns_anim_scroll_fresh(b->anim) ||
        !browser_sync_scroll_timelines(b))
        return;
    b->scroll_pass = TRUE;
    browser_relayout(b);
    b->scroll_pass = FALSE;
}

static void
browser_note_anim_tick(ns_browser *b)
{
    if (ns_anim_paint_only(b->anim))
        b->anim_paint_dirty = TRUE;
    else
        b->cascade_dirty = TRUE;
}

static gboolean
browser_mutation_relayout_due(ns_browser *b)
{
    if (!b->layout || b->last_layout_us <= 0) return TRUE;
    gint64 min_gap = b->relayout_cost_us;
    if (min_gap < NS_LAYOUT_BACKGROUND_MIN_US)
        min_gap = NS_LAYOUT_BACKGROUND_MIN_US;
    if (min_gap > NS_LAYOUT_BACKGROUND_MAX_US)
        min_gap = NS_LAYOUT_BACKGROUND_MAX_US;
    return g_get_monotonic_time() - b->last_layout_us >= min_gap;
}

static gboolean
browser_relayout_from_mutation(ns_browser *b)
{
    if (b->layout && b->layout_osc >= NS_LAYOUT_OSC_THRESHOLD) {
        gint64 now = g_get_monotonic_time();
        if (now < b->damp_until_us) {
            b->dirty = TRUE;
            return FALSE;
        }
        b->damp_until_us = now + NS_LAYOUT_DAMP_US;
        if (!b->damp_logged) {
            b->damp_logged = TRUE;
            g_message("nordstjernen: layout dampener engaged "
                      "(script reflow loop with no user input)");
        }
    }
    browser_relayout(b);
    return TRUE;
}

static gboolean
overflow_keyword_hidden(const char *kw)
{
    return kw && (g_ascii_strcasecmp(kw, "hidden") == 0 ||
                  g_ascii_strcasecmp(kw, "clip") == 0);
}

static gboolean
box_axis_overflow_hidden(const ns_box *b, ns_css_prop axis)
{
    if (!b || !b->style) return FALSE;
    const char *kw = ns_style_keyword(b->style, axis);
    if (!kw) kw = ns_style_keyword(b->style, NS_CSS_OVERFLOW);
    return overflow_keyword_hidden(kw);
}

static gboolean
box_overflow_visible(const ns_box *b)
{
    if (!b || !b->style) return TRUE;
    const ns_css_prop axes[] = { NS_CSS_OVERFLOW_X, NS_CSS_OVERFLOW_Y };
    for (size_t i = 0; i < G_N_ELEMENTS(axes); i++) {
        const char *kw = ns_style_keyword(b->style, axes[i]);
        if (!kw) kw = ns_style_keyword(b->style, NS_CSS_OVERFLOW);
        if (kw && g_ascii_strcasecmp(kw, "visible") != 0) return FALSE;
    }
    return TRUE;
}

static const ns_box *
find_element_box(const ns_box *b, const char *name)
{
    if (!b) return NULL;
    if (b->dom && b->dom->name && strcmp(b->dom->name, name) == 0) return b;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling) {
        const ns_box *found = find_element_box(c, name);
        if (found) return found;
    }
    return NULL;
}

static gboolean
root_axis_overflow_hidden(const ns_box *b, ns_css_prop axis)
{
    const ns_box *root = find_element_box(b, "html");
    if (root && !box_overflow_visible(root))
        return box_axis_overflow_hidden(root, axis);
    const ns_box *body = find_element_box(root ? root : b, "body");
    return box_axis_overflow_hidden(body, axis);
}

static int browser_images_outstanding(ns_browser *browser);

static gboolean
browser_fire_media_events(gpointer user_data)
{
    ns_browser *b = user_data;
    b->media_events_source = 0;
    if (b->js && b->layout) ns_js_fire_media_load_events(b->js, b->layout);
    return G_SOURCE_REMOVE;
}

static void
browser_schedule_media_events(ns_browser *b)
{
    if (!b->media_events_source)
        b->media_events_source =
            g_timeout_add(0, browser_fire_media_events, b);
}

static void browser_flush(gpointer user_data);

static gboolean
browser_load_waits_for_images(gpointer user_data)
{
    ns_browser *b = user_data;
    if (g_get_monotonic_time() >= b->load_delay_deadline_us) return FALSE;
    if (!b->relaying) browser_flush(b);
    return b->media_events_source != 0 || b->images_arrived_since_layout ||
           browser_images_outstanding(b) > 0;
}

static void
browser_image_arrived(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b) return;
    b->images_arrived_since_layout = TRUE;
    b->image_arrivals_since_layout++;
    if (b->image_arrivals_since_layout >= NS_IMAGE_RELAYOUT_BATCH ||
        browser_images_outstanding(b) == 0) {
        b->image_arrivals_since_layout = 0;
        b->dirty = TRUE;
    }
}

static int
browser_images_outstanding(ns_browser *browser)
{
    if (!browser->img_sessions) return 0;
    int total = 0;
    for (guint i = 0; i < browser->img_sessions->len; ) {
        ns_engine_img_session *s = g_ptr_array_index(browser->img_sessions, i);
        int o = ns_engine_img_session_outstanding(s);
        if (o == 0) {
            ns_engine_img_session_close(s);
            g_ptr_array_remove_index_fast(browser->img_sessions, i);
            continue;
        }
        total += o;
        i++;
    }
    return total;
}

static char *
browser_document_base(const ns_browser *b)
{
    return ns_js_base_url_for_document(b->js, b->doc, b->base_url);
}

static void
browser_ensure_images(ns_browser *browser)
{
    if (browser->images_fetched && !browser->has_deferred_lazy) return;
    if (!browser->img_requested)
        browser->img_requested = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    if (!browser->img_sessions)
        browser->img_sessions = g_ptr_array_new();
    double vp_h = browser->cur_viewport_h > 0.0 ? browser->cur_viewport_h
                                                : browser->vh;
    gboolean deferred = FALSE;
    g_autofree char *img_base = browser_document_base(browser);
    ns_engine_img_session *s =
        ns_engine_fetch_images_start(browser->layout,
                                     img_base ? img_base : browser->base_url,
                                     browser->images, browser->img_requested,
                                     browser->cur_scroll_y, vp_h, &deferred,
                                     browser_image_arrived, browser);
    if (s) g_ptr_array_add(browser->img_sessions, s);
    browser->images_fetched = TRUE;
    browser->has_deferred_lazy = deferred;
}

static void
browser_wait_images(ns_browser *browser)
{
    browser_ensure_images(browser);
    gint64 deadline = g_get_monotonic_time() + (gint64)15 * G_USEC_PER_SEC;
    while (browser_images_outstanding(browser) > 0 &&
           g_get_monotonic_time() < deadline) {
        int dispatched = 0;
        while (g_main_context_pending(NULL) && dispatched++ < 64)
            g_main_context_iteration(NULL, FALSE);
        if (dispatched == 0) g_usleep(1000);
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
}

static void
browser_flush(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !b->js) return;
    if (ns_js_consume_mutated(b->js))
        b->dirty = TRUE;
    if (!b->layout || b->dirty || b->cascade_dirty) {
        browser_relayout(b);
        b->dirty = FALSE;
    }
}

static void
browser_flush_style(gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !b->js || b->relaying) return;
    if (!b->layout || !b->styles) {
        browser_flush(user_data);
        return;
    }
    if (ns_js_consume_mutated(b->js))
        b->dirty = TRUE;
    if (!b->cascade_dirty && !b->anim_paint_dirty &&
        b->styles_serial == ns_js_mutation_serial(b->js))
        return;
    b->relaying = TRUE;
    ns_css_set_viewport((double)b->vw, b->vh);
    ns_css_set_doc_language(b->doc_language);
    GHashTable *fresh = ns_engine_restyle(b->doc, b->base_url, b->vw, b->vh,
                                          b->images, b->anim, b->js,
                                          b->css_cache,
                                          ns_js_focused_node(b->js),
                                          b->hover_node);
    b->relaying = FALSE;
    if (!fresh) return;
    if (b->retired_styles)
        g_hash_table_destroy(b->styles);
    else
        b->retired_styles = b->styles;
    b->styles = fresh;
    b->styles_serial = ns_js_mutation_serial(b->js);
    b->cascade_dirty = FALSE;
    b->anim_paint_dirty = FALSE;
    b->dirty = TRUE;
}

static char *
browser_url_fragment(const char *url, gboolean *has_fragment)
{
    const char *hash = url ? strchr(url, '#') : NULL;
    if (has_fragment) *has_fragment = hash != NULL;
    if (!hash) return NULL;
    char *decoded = g_uri_unescape_string(hash + 1, NULL);
    return decoded ? decoded : g_strdup(hash + 1);
}

static gboolean
browser_reveal_fragment_target(ns_browser *browser, ns_node *target)
{
    gboolean changed = FALSE;
    for (ns_node *cur = target; cur; cur = cur->parent) {
        if (ns_element_hidden_until_found(cur)) {
            ns_element_remove_attr(cur, "hidden");
            changed = TRUE;
        }
        if (cur->parent && ns_details_fragment_needs_open(cur->parent, cur) &&
            !ns_element_get_attr(cur->parent, "open")) {
            ns_element_set_attr(cur->parent, "open", "");
            changed = TRUE;
        }
        if (cur == browser->doc) break;
    }
    return changed;
}

static const ns_box *
browser_box_for_node(const ns_box *box, const ns_node *target)
{
    if (!box || !target) return NULL;
    if (box->dom == target) return box;
    for (const ns_box *child = box->first_child; child;
         child = child->next_sibling) {
        const ns_box *found = browser_box_for_node(child, target);
        if (found) return found;
    }
    if (box->inline_atomics)
        for (guint i = 0; i < box->inline_atomics->len; i++) {
            const ns_inline_atomic *atomic = &g_array_index(
                box->inline_atomics, ns_inline_atomic, i);
            const ns_box *found = browser_box_for_node(atomic->box, target);
            if (found) return found;
        }
    return NULL;
}

static gboolean
browser_target_scroll_y(ns_browser *browser, const ns_node *target, int *out_y)
{
    if (!browser || !browser->layout || !target) return FALSE;
    const ns_box *box = browser_box_for_node(browser->layout, target);
    double y = 0;
    if (box) {
        y = box->y + box->margin.top;
    } else {
        double x = 0, w = 0, h = 0;
        if (!ns_box_inline_rect_for_dom(browser->layout, target,
                                        &x, &y, &w, &h))
            return FALSE;
    }
    *out_y = (int)floor(MAX(0.0, y));
    return TRUE;
}

static void
browser_queue_scroll_to(ns_browser *browser, const ns_node *target,
                        gboolean reveal)
{
    if (!browser || !target) return;
    if (reveal && browser_reveal_fragment_target(browser, (ns_node *)target))
        browser->dirty = TRUE;
    browser_flush(browser);
    int y = 0;
    if (!browser_target_scroll_y(browser, target, &y)) return;
    browser->pending_scroll_y = y;
    browser->pending_scroll = TRUE;
    if (reveal) {
        browser->scroll_anchor = target;
        browser->scroll_anchor_y = y;
    }
}

static void
browser_follow_scroll_anchor(ns_browser *browser)
{
    if (!browser || !browser->scroll_anchor) return;
    if (fabs(browser->cur_scroll_y - (double)browser->scroll_anchor_y) > 1.0 &&
        !browser->pending_scroll) {
        browser->scroll_anchor = NULL;
        return;
    }
    int y = 0;
    if (browser_target_scroll_y(browser, browser->scroll_anchor, &y) &&
        y != browser->scroll_anchor_y) {
        browser->scroll_anchor_y = y;
        browser->pending_scroll_y = y;
        browser->pending_scroll = TRUE;
    }
    if (!ns_browser_animating(browser) && !browser->pending_scroll)
        browser->scroll_anchor = NULL;
}

static void
browser_js_viewport_scroll(double *x, double *y, gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b) return;
    browser_flush(b);
    int page_w = 0, page_h = 0;
    ns_browser_page_size(b, &page_w, &page_h);
    double view_h = b->cur_viewport_h > 0.0 ? b->cur_viewport_h : b->vh;
    double max_x = MAX((double)page_w - (double)b->vw, 0.0);
    double max_y = MAX((double)page_h - view_h, 0.0);
    *x = round(CLAMP(*x, 0.0, max_x));
    *y = round(CLAMP(*y, 0.0, max_y));
    b->pending_scroll_x = (int)*x;
    b->pending_scroll_y = (int)*y;
    b->pending_scroll = TRUE;
    b->js_scroll_x = *x;
    b->js_scroll_y = *y;
    b->scroll_anchor = NULL;
}

static void
browser_js_scroll_to(const ns_node *target, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (browser) browser->scroll_anchor = NULL;
    browser_queue_scroll_to(browser, target, FALSE);
}

static void
browser_js_soft_navigate(const char *url, gboolean replace, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (!browser || !url) return;
    if (!replace) browser->soft_nav_pushed = TRUE;
    gboolean has_fragment = FALSE;
    g_autofree char *fragment = browser_url_fragment(url, &has_fragment);
    ns_css_set_target_fragment(has_fragment && fragment && *fragment
                                   ? fragment : NULL);
    g_free(browser->base_url);
    browser->base_url = g_strdup(url);
    browser->dirty = TRUE;
}

static void
browser_js_fragment_navigate(const char *url, gpointer user_data)
{
    ns_browser *browser = user_data;
    if (!browser || !url) return;
    gboolean has_fragment = FALSE;
    g_autofree char *fragment = browser_url_fragment(url, &has_fragment);
    if (!has_fragment) return;
    ns_css_set_target_fragment(fragment && *fragment ? fragment : NULL);
    browser->dirty = TRUE;
    browser->scroll_anchor = NULL;
    if (!fragment || !*fragment) {
        browser->pending_scroll_y = 0;
        browser->pending_scroll = TRUE;
        return;
    }
    ns_node *target = ns_node_find_fragment_target(browser->doc, fragment);
    if (target) browser_queue_scroll_to(browser, target, TRUE);
}

static gboolean
settle_quit_cb(gpointer user_data)
{
    g_main_loop_quit(user_data);
    return G_SOURCE_CONTINUE;
}

typedef struct settle_ctx {
    ns_browser *b;
    GMainLoop  *loop;
    int         quiet_ticks;
    gint64      deadline_us;
} settle_ctx;

#define NS_SETTLE_QUIET_TICKS 3

static gboolean
browser_settle_quiet(ns_browser *b, gint64 deadline_us)
{
    if (b->dirty) return FALSE;
    if (b->js && ns_js_has_work_before(b->js, deadline_us)) return FALSE;
    if (b->images && ns_image_cache_has_pending(b->images)) return FALSE;
    if (b->videos && ns_video_cache_has_pending(b->videos)) return FALSE;
    if (ns_svg_external_pending() > 0) return FALSE;
    if (g_main_context_pending(NULL)) return FALSE;
    return TRUE;
}

static gboolean
settle_tick_skipped(settle_ctx *ctx, gint64 now)
{
    if (now >= ctx->deadline_us) {
        g_main_loop_quit(ctx->loop);
        return TRUE;
    }
    return ns_js_page_busy(ctx->b->js);
}

static gboolean
settle_tick_cb(gpointer user_data)
{
    settle_ctx *ctx = user_data;
    ns_browser *b = ctx->b;
    gint64 now = g_get_monotonic_time();
    if (settle_tick_skipped(ctx, now)) return G_SOURCE_CONTINUE;
    if (b->images) ns_image_cache_tick(b->images, now);
    if (b->videos && b->layout) {
        if (b->videos_noted_seq != b->layout_seq) {
            ns_video_cache_discover(b->videos, b->layout, b->doc, now);
            ns_video_cache_note_layout(b->videos, b->layout);
            b->videos_noted_seq = b->layout_seq;
        }
        ns_video_cache_tick(b->videos, now);
    }
    browser_sync_scroll_timelines(b);
    if (b->anim && ns_anim_tick(b->anim, now)) {
        browser_note_anim_tick(b);
        if (ns_anim_needs_layout(b->anim)) b->dirty = TRUE;
    }
    if (b->anim && b->js) ns_js_dispatch_anim_events(b->js, b->anim);
    if (b->js) ns_js_run_animation_frame(b->js);
    if (b->js && ns_js_consume_mutated(b->js))
        b->dirty = TRUE;
    browser_sync_scroll_timelines(b);
    if (b->dirty && browser_mutation_relayout_due(b)) {
        if (browser_relayout_from_mutation(b))
            b->dirty = FALSE;
    }
    if (browser_settle_quiet(b, ctx->deadline_us)) {
        if (++ctx->quiet_ticks >= NS_SETTLE_QUIET_TICKS) {
            g_main_loop_quit(ctx->loop);
            return G_SOURCE_CONTINUE;
        }
    } else {
        ctx->quiet_ticks = 0;
    }
    return G_SOURCE_CONTINUE;
}

static void
browser_settle(ns_browser *b, int settle_ms)
{
    if (settle_ms <= 0) return;
    if (b->videos && b->layout) {
        ns_video_cache_discover(b->videos, b->layout, b->doc, g_get_monotonic_time());
        ns_video_cache_note_layout(b->videos, b->layout);
    }
    gint64 deadline_us = g_get_monotonic_time() + (gint64)settle_ms * 1000;
    if (browser_settle_quiet(b, deadline_us)) return;
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    settle_ctx ctx = {
        .b = b,
        .loop = loop,
        .deadline_us = deadline_us,
    };
    guint quit = g_timeout_add(settle_ms, settle_quit_cb, loop);
    guint tick = g_timeout_add(16, settle_tick_cb, &ctx);
    g_main_loop_run(loop);
    g_source_remove(tick);
    g_source_remove(quit);
    g_main_loop_unref(loop);
}

static void
browser_js_log(const char *line, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !line) return;
    ns_debug_log_emit(NS_DLOG_JS, "console", "%s", line);
    if (!b->console_buf) b->console_buf = g_string_new(NULL);
    if (b->console_buf->len > 256u * 1024u)
        g_string_erase(b->console_buf, 0,
                       (gssize)(b->console_buf->len - 192u * 1024u));
    g_string_append(b->console_buf, line);
    g_string_append_c(b->console_buf, '\n');
}
static void browser_js_mutated(gpointer ud) { ns_browser *b = ud; if (b) b->dirty = TRUE; }

static gboolean
browser_url_is_chrome_page(const char *url)
{
    if (!url || g_ascii_strncasecmp(url, "about:", 6) != 0) return FALSE;
    const char *page = url + 6;
    gsize len = strcspn(page, "?#");
    if (len == 5 && g_ascii_strncasecmp(page, "blank", 5) == 0) return FALSE;
    if (len == 6 && g_ascii_strncasecmp(page, "srcdoc", 6) == 0) return FALSE;
    return TRUE;
}

static gboolean
browser_url_is_privileged_page(const char *url)
{
    if (!url) return FALSE;
    if (g_ascii_strncasecmp(url, "view-source:", 12) == 0) return TRUE;
    if (g_ascii_strncasecmp(url, "about:", 6) != 0) return FALSE;
    const char *page = url + 6;
    return g_ascii_strncasecmp(page, "settings", 8) == 0 ||
           g_ascii_strncasecmp(page, "config", 6) == 0 ||
           g_ascii_strncasecmp(page, "history", 7) == 0 ||
           g_ascii_strncasecmp(page, "ai", 2) == 0;
}

static gboolean
browser_url_is_http(const char *url)
{
    return g_ascii_strncasecmp(url, "http://", 7) == 0 ||
           g_ascii_strncasecmp(url, "https://", 8) == 0;
}

static gboolean
browser_url_continues_interstitial(const char *url, const char *from)
{
    const char *rest = url + strlen(NS_UNSAFE_CONTINUE_SCHEME);
    if (!browser_url_is_http(rest)) return FALSE;
    g_autofree char *target_host = ns_url_host_from(rest);
    g_autofree char *from_host = from ? ns_url_host_from(from) : NULL;
    return target_host && from_host && *target_host &&
           g_ascii_strcasecmp(target_host, from_host) == 0;
}

static gboolean
browser_allows_navigation_url(ns_browser *b, const char *url)
{
    if (!url) return TRUE;
    const char *from = b ? b->base_url : NULL;
    if (g_ascii_strncasecmp(url, "file:", 5) == 0)
        return from && g_ascii_strncasecmp(from, "file:", 5) == 0;
    if (browser_url_is_privileged_page(url))
        return browser_url_is_chrome_page(from);
    if (g_str_has_prefix(url, NS_UNSAFE_CONTINUE_SCHEME))
        return browser_url_continues_interstitial(url, from);
    return TRUE;
}

static char *
browser_resolve_navigation(ns_browser *b, const char *href)
{
    if (!b || !href) return NULL;
    g_autofree char *base = browser_document_base(b);
    char *abs = ns_url_resolve(base ? base : b->base_url, href);
    if (!browser_allows_navigation_url(b, abs)) {
        g_free(abs);
        return NULL;
    }
    return abs;
}

static void browser_js_navigate(const char *url, gboolean reload, gpointer ud)
{
    (void)reload;
    ns_browser *b = ud;
    if (!b || !url || !*url) return;
    g_free(b->pending_nav);
    b->pending_nav = browser_resolve_navigation(b, url);
}

static void browser_js_download(const char *url, const char *filename, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !url || !*url) return;
    g_autofree char *base = browser_document_base(b);
    char *abs = ns_url_resolve(base ? base : b->base_url, url);
    const char *target = abs ? abs : url;
    if (!browser_allows_navigation_url(b, target)) {
        g_free(abs);
        return;
    }
    g_free(b->pending_download);
    b->pending_download = g_strdup_printf("%s\t%s", target,
                                          filename ? filename : "");
    g_free(abs);
}

static void
browser_js_window_action(const char *action, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !action || !*action) return;
    g_free(b->pending_window_action);
    b->pending_window_action = g_strdup(action);
}

#define NS_PENDING_CLIPBOARD_MAX 1048576

static gboolean
browser_js_clipboard_write(const char *text, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !text) return FALSE;
    if (strlen(text) > NS_PENDING_CLIPBOARD_MAX) return FALSE;
    g_free(b->pending_clipboard);
    b->pending_clipboard = g_strdup(text);
    return TRUE;
}

static void browser_sync_js_selection(ns_browser *browser);

static gboolean
browser_js_selection_cmd(const char *command, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !command || !b->layout) return FALSE;
    gboolean ok = FALSE;
    if (strcmp(command, "selectAll") == 0)
        ok = ns_selection_select_all(&b->selection, b->layout);
    else if (strcmp(command, "unselect") == 0) {
        ns_selection_clear(&b->selection);
        ok = TRUE;
    }
    if (ok) {
        browser_sync_js_selection(b);
        b->dirty = TRUE;
    }
    return ok;
}

#define NS_PENDING_AUDIO_MAX 15000

static void
browser_js_audio(const char *command, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !command || !*command) return;
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[audio-cmd] %s\n", command);
    if (!b->pending_audio) b->pending_audio = g_string_new(NULL);
    if (b->pending_audio->len >= NS_PENDING_AUDIO_MAX) return;
    g_string_append(b->pending_audio, command);
    g_string_append_c(b->pending_audio, '\n');
}

static void
browser_js_video(const void *node, const char *kind, double value, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->js) return;
    ns_js_video_event(b->js, node, kind, value);
}

static gboolean
browser_media_seek(const void *node, double seconds, gpointer ud)
{
    ns_browser *b = ud;
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[media-seek] to=%.3f\n", seconds);
    if (!b || !b->videos) return FALSE;
    return ns_video_cache_seek_node(b->videos, node, seconds,
                                    g_get_monotonic_time());
}

static ns_texture *
browser_media_frame(const void *node, const char **out_url,
                    const char **out_cors, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return NULL;
    return ns_video_cache_frame_for_node(b->videos, node, out_url, out_cors);
}

static void
browser_js_repaint(gpointer ud)
{
    ns_browser *b = ud;
    if (b) b->repaint_requested = TRUE;
}

static void
browser_media_play(const void *node, gboolean play, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return;
    ns_video_cache_set_node_playing(b->videos, node, play,
                                    g_get_monotonic_time());
    if (b->js) ns_js_request_repaint(b->js);
}

static void
browser_media_muted(const void *node, gboolean muted, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return;
    ns_video_cache_set_node_muted(b->videos, node, muted);
    if (b->js) ns_js_request_repaint(b->js);
}

static void
browser_media_volume(const void *node, double volume, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return;
    ns_video_cache_set_node_volume(b->videos, node, volume);
    if (b->js) ns_js_request_repaint(b->js);
}

static gboolean
browser_mse_data(guint stream_id, char kind, const guint8 *data, gsize len,
                 gboolean eos, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return FALSE;
    if (eos) {
        ns_video_cache_mse_eos(b->videos, stream_id);
        return TRUE;
    }
    return ns_video_cache_mse_append(b->videos, stream_id, kind, data, len);
}

static double
browser_mse_buffered(guint stream_id, char kind, int index, double *start,
                     gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) {
        if (start) *start = 0.0;
        return 0.0;
    }
    return ns_video_cache_mse_buffered(b->videos, stream_id, kind, index,
                                       start);
}

static gboolean
browser_mse_remove(guint stream_id, char kind, double start, double end,
                   gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return FALSE;
    return ns_video_cache_mse_remove(b->videos, stream_id, kind, start, end);
}

static gsize
browser_mse_bytes(guint stream_id, char kind, gpointer ud)
{
    ns_browser *b = ud;
    if (!b || !b->videos) return 0;
    return ns_video_cache_mse_bytes(b->videos, stream_id, kind);
}

char *
ns_browser_take_pending_audio(ns_browser *browser)
{
    if (!browser || !browser->pending_audio ||
        browser->pending_audio->len == 0)
        return NULL;
    char *out = g_strdup(browser->pending_audio->str);
    g_string_truncate(browser->pending_audio, 0);
    return out;
}

int
ns_browser_init(void)
{
    ns_config_init();
    if (ns_config_get()->harden_allocator)
        ns_security_harden_allocator();
    ns_net_init();
    ns_net_set_early_hints_handler(ns_engine_early_hints);
    ns_net_set_allow_file_urls(TRUE);
    ns_cache_init();
    ns_bytecode_cache_init();
    ns_history_init();
    ns_font_init();
    ns_spell_init();
    return 0;
}

void
ns_browser_sandbox(const char *self_exe)
{
    gboolean gpu_apis = ns_config_get()->webgl_enabled ||
                        ns_config_get()->webgpu_enabled;
    if (!gpu_apis)
        g_setenv("ORC_CODE", "backup", TRUE);
    ns_os_media_preinit();
    ns_security_win32_mitigations_init(FALSE);
    if (ns_config_get()->webgl_enabled || ns_config_get()->webgpu_enabled)
        ns_security_allow_gpu_devices();
    ns_security_runtime_dir_read_only();
    ns_security_sandbox_init(self_exe);
    if (ns_config_get()->webgl_enabled)
        ns_gl_prepare_for_sandbox();
    ns_security_seccomp_deny_unix_sockets();
    if (!gpu_apis)
        ns_security_seccomp_deny_exec_memory();
    ns_security_seccomp_init();
}

void
ns_browser_shutdown(void)
{
    ns_font_shutdown();
    ns_bytecode_cache_shutdown();
    ns_history_shutdown();
    ns_cache_shutdown();
    ns_net_shutdown();
    ns_config_shutdown();
}

static char *
resolve_local_path(const char *url)
{
    if (!url || strstr(url, "://") ||
        g_str_has_prefix(url, "about:") || g_str_has_prefix(url, "data:") ||
        !g_file_test(url, G_FILE_TEST_EXISTS))
        return NULL;
    char *abs = g_canonicalize_filename(url, NULL);
    char *file_url = g_filename_to_uri(abs, NULL, NULL);
    g_free(abs);
    return file_url;
}

static const char *
browser_find_meta_refresh(const ns_node *n, int depth)
{
    if (!n || depth > 1024) return NULL;
    if (ns_node_is_element_named(n, "meta")) {
        const char *equiv = ns_element_get_attr(n, "http-equiv");
        if (equiv && g_ascii_strcasecmp(equiv, "refresh") == 0) {
            const char *content = ns_element_get_attr(n, "content");
            if (content && *content) return content;
        }
    }
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const char *found = browser_find_meta_refresh(c, depth + 1);
        if (found) return found;
    }
    return NULL;
}

static void
browser_arm_declarative_refresh(ns_browser *b, const char *header_value)
{
    double seconds = 0.0;
    char *target = NULL;
    gboolean armed = header_value &&
        ns_net_parse_refresh(header_value, &seconds, &target);
    if (!armed) {
        const char *meta = browser_find_meta_refresh(b->doc, 0);
        armed = meta && ns_net_parse_refresh(meta, &seconds, &target);
    }
    if (!armed) return;
    if (target) {
        b->refresh_url = browser_resolve_navigation(b, target);
        g_free(target);
        if (!b->refresh_url) return;
    } else {
        b->refresh_url = g_strdup(b->base_url);
    }
    b->refresh_due_us = g_get_monotonic_time() + (gint64)(seconds * 1e6);
}

static gboolean
browser_content_type_starts(const char *content_type, const char *prefix)
{
    return content_type && prefix &&
        g_ascii_strncasecmp(content_type, prefix, strlen(prefix)) == 0;
}

static gboolean
browser_content_type_is_html(const char *content_type)
{
    return browser_content_type_starts(content_type, "text/html") ||
           browser_content_type_starts(content_type, "application/xhtml");
}

static gboolean
browser_content_type_is_json(const char *content_type)
{
    return browser_content_type_starts(content_type, "application/json") ||
           browser_content_type_starts(content_type, "text/json") ||
           (content_type && strstr(content_type, "+json") != NULL);
}

static gboolean
browser_content_type_is_xml(const char *content_type)
{
    if (!content_type) return FALSE;
    if (strstr(content_type, "xhtml") || strstr(content_type, "svg"))
        return FALSE;
    return browser_content_type_starts(content_type, "text/xml") ||
           browser_content_type_starts(content_type, "application/xml") ||
           strstr(content_type, "+xml") != NULL;
}

static gboolean
browser_content_type_is_pdf(const char *content_type)
{
    return browser_content_type_starts(content_type, "application/pdf") ||
           browser_content_type_starts(content_type, "application/x-pdf");
}

static char *
browser_text_document(const char *url, const char *text)
{
    char *esc_url = ns_html_escape_text(url && *url ? url : "text file");
    char *esc_text = ns_html_escape_text(text ? text : "");
    char *html = g_strconcat(
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>",
        esc_url,
        "</title><style>"
        "body{margin:0;background:#fff;color:#111}"
        "pre{margin:0;padding:12px;font:13px/1.45 ui-monospace,"
        "\"SF Mono\",Menlo,Consolas,monospace;white-space:pre-wrap;"
        "overflow-wrap:anywhere}"
        "</style></head><body><pre>",
        esc_text,
        "</pre></body></html>",
        NULL);
    g_free(esc_url);
    g_free(esc_text);
    return html;
}

static void
browser_prepare_document_response(ns_response *resp)
{
    if (!resp || !resp->body || !resp->content_type)
        return;
    const char *final_url = resp->final_url ? resp->final_url : "";
    char *html = NULL;
    if (browser_content_type_starts(resp->content_type, "image/")) {
        html = ns_html_image_document(final_url);
    } else if (browser_content_type_is_pdf(resp->content_type)) {
        html = ns_pdf_document_html(resp->body->data, resp->body->len,
                                    final_url);
    } else if (browser_content_type_is_json(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = ns_html_json_document(final_url, decoded,
                                     decoded ? strlen(decoded) : 0);
        if (!html) html = browser_text_document(final_url, decoded);
        g_free(decoded);
    } else if (browser_content_type_is_xml(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = ns_html_xml_document(final_url, decoded,
                                    decoded ? strlen(decoded) : 0);
        if (!html) html = browser_text_document(final_url, decoded);
        g_free(decoded);
    } else if (browser_content_type_starts(resp->content_type, "text/") &&
               !browser_content_type_is_html(resp->content_type)) {
        char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                                 resp->body->len,
                                                 resp->content_type, NULL);
        html = browser_text_document(final_url, decoded);
        g_free(decoded);
    }
    if (!html)
        return;
    g_byte_array_set_size(resp->body, 0);
    g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
    g_free(html);
    g_free(resp->content_type);
    resp->content_type = g_strdup("text/html; charset=utf-8");
}

static void
browser_collect_meta_csp(GPtrArray *out, const ns_node *node, int depth)
{
    if (depth > 1024) return;
    for (const ns_node *c = node ? node->first_child : NULL; c;
         c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "meta") == 0) {
            const char *he = ns_element_get_attr(c, "http-equiv");
            const char *content = ns_element_get_attr(c, "content");
            if (he && g_ascii_strcasecmp(he, "content-security-policy") == 0 &&
                content && *content)
                g_ptr_array_add(out, (gpointer)content);
        }
        browser_collect_meta_csp(out, c, depth + 1);
    }
}

static void
browser_register_early_csp(const ns_node *doc, const char *url,
                           const char *csp_header)
{
    GPtrArray *headers = g_ptr_array_new();
    if (csp_header && *csp_header)
        g_ptr_array_add(headers, (gpointer)csp_header);
    browser_collect_meta_csp(headers, doc, 0);
    g_ptr_array_add(headers, NULL);
    if (headers->len > 1)
        ns_net_csp_register(doc, url, (const char *const *)headers->pdata);
    g_ptr_array_free(headers, TRUE);
}

static void
browser_apply_meta_csp(ns_js *js, const ns_node *node, int depth)
{
    if (depth > 1024) return;
    for (const ns_node *c = node ? node->first_child : NULL; c;
         c = c->next_sibling) {
        if (c->kind == NS_NODE_ELEMENT && c->name &&
            g_ascii_strcasecmp(c->name, "meta") == 0) {
            const char *he = ns_element_get_attr(c, "http-equiv");
            if (he && g_ascii_strcasecmp(he, "content-security-policy") == 0) {
                const char *content = ns_element_get_attr(c, "content");
                if (content && *content)
                    ns_js_add_csp_header(js, content);
            }
        }
        browser_apply_meta_csp(js, c, depth + 1);
    }
}

static gboolean
headers_have_no_store(const char *raw)
{
    if (!raw) return FALSE;
    char *low = g_ascii_strdown(raw, -1);
    gboolean found = strstr(low, "no-store") != NULL;
    g_free(low);
    return found;
}

static void browser_js_form_submit(const ns_node *form, const ns_node *submitter,
                                   gpointer user_data);

static ns_browser *
browser_build_from_doc(ns_node *doc, char *base, int viewport_width,
                       double viewport_height, int settle_ms,
                       gboolean bfcache_ok, char *refresh_hdr,
                       char *doc_language, char *csp_header, char *doc_charset,
                       const char *url,
                       const ns_js_navigation_timing *navigation_timing)
{
    int vw = viewport_width > 0 ? viewport_width : 1000;
    double vh = viewport_height > 0.0
        ? viewport_height
        : (double)vw * 0.75;
    ns_css_set_viewport((double)vw, vh);
    gboolean has_fragment = FALSE;
    g_autofree char *fragment = browser_url_fragment(url, &has_fragment);
    ns_css_set_target_fragment(has_fragment && fragment && *fragment
                                   ? fragment : NULL);

    browser_register_early_csp(doc, url, csp_header);
    if (ns_config_get()->speculative_preload)
        ns_engine_speculative_preload(doc, base, FALSE);

    ns_browser *b = g_new0(ns_browser, 1);
    b->pending_scroll_x = -1;
    b->dppx = ns_css_device_pixel_ratio();
    b->doc = doc;
    b->doc_charset = doc_charset;
    b->doc_language = doc_language;
    b->base_url = base;
    ns_css_set_doc_language(b->doc_language);
    b->vw = vw;
    b->vh = vh;
    b->bfcache_ok = bfcache_ok;
    b->caret_paint_visible = TRUE;
    b->css_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)g_bytes_unref);
    b->images = ns_image_cache_new();
    b->videos = ns_video_cache_new();
    ns_video_cache_set_base(b->videos, base);
    b->styles = ns_engine_compute_cascade(doc, base, b->css_cache, NULL);

    b->anim = ns_anim_new();
    ns_engine_anim_track_layout(b->anim, &b->layout);
    ns_engine_load_keyframes(b->anim, doc, base, b->css_cache);
    ns_engine_anim_observe(b->anim, b->styles, g_get_monotonic_time());

    b->js = ns_js_new(browser_js_log, b,
                      browser_js_mutated, b,
                      browser_js_navigate, b,
                      navigation_timing);
    if (b->js) {
        b->load_delay_deadline_us =
            g_get_monotonic_time() + (gint64)10 * G_USEC_PER_SEC;
        ns_js_set_load_delay_cb(b->js, browser_load_waits_for_images, b);
        ns_js_set_style_table(b->js, b->styles);
        ns_js_set_image_cache(b->js, b->images);
        ns_js_set_anim(b->js, b->anim);
        ns_js_set_form_submit_cb(b->js, browser_js_form_submit, b);
        ns_js_set_repaint_cb(b->js, browser_js_repaint, b);
        ns_js_set_layout_flush_cb(b->js, browser_flush, b);
        ns_js_set_style_flush_cb(b->js, browser_flush_style);
        ns_js_set_viewport_scroll_cb(b->js, browser_js_viewport_scroll, b);
        ns_js_set_scroll_to_cb(b->js, browser_js_scroll_to, b);
        ns_js_set_fragment_nav_cb(b->js, browser_js_fragment_navigate, b);
        ns_js_set_soft_nav_cb(b->js, browser_js_soft_navigate, b);
        ns_js_set_download_cb(b->js, browser_js_download, b);
        ns_js_set_clipboard_write_cb(b->js, browser_js_clipboard_write, b);
        ns_js_set_selection_cmd_cb(b->js, browser_js_selection_cmd, b);
        ns_js_set_audio_cb(b->js, browser_js_audio, b);
        ns_js_set_media_seek_cb(b->js, browser_media_seek, b);
        ns_js_set_media_play_cb(b->js, browser_media_play, b);
        ns_js_set_media_muted_cb(b->js, browser_media_muted, b);
        ns_js_set_media_frame_cb(b->js, browser_media_frame, b);
        ns_js_set_mse_cb(b->js, browser_mse_data, b);
        ns_js_set_mse_buffered_cb(b->js, browser_mse_buffered, b);
        ns_js_set_mse_remove_cb(b->js, browser_mse_remove, b);
        ns_js_set_mse_bytes_cb(b->js, browser_mse_bytes, b);
        ns_js_set_media_volume_cb(b->js, browser_media_volume, b);
        ns_js_set_window_action_cb(b->js, browser_js_window_action, b);
        ns_js_add_csp_header(b->js, csp_header);
        browser_apply_meta_csp(b->js, doc, 0);
        const ns_config *run_cfg = ns_config_get();
        if (!run_cfg || run_cfg->javascript_enabled)
            ns_js_run_scripts_in_doc(b->js, doc, base);
    }
    g_free(csp_header);
    if (b->videos) {
        ns_video_cache_set_js_cb(b->videos, browser_js_video, b);
        ns_video_cache_set_audio_cb(b->videos, browser_js_audio, b);
    }

    browser_arm_declarative_refresh(b, refresh_hdr);
    g_free(refresh_hdr);

    if (!b->layout || b->dirty)
        browser_relayout(b);
    browser_settle(b, settle_ms);
    if (!b->layout || b->dirty)
        browser_relayout(b);
    ns_node *fragment_target = fragment && *fragment
        ? ns_node_find_fragment_target(b->doc, fragment) : NULL;
    if (fragment_target) browser_queue_scroll_to(b, fragment_target, TRUE);
    else if (has_fragment && (!fragment || !*fragment)) {
        b->pending_scroll_y = 0;
        b->pending_scroll = TRUE;
    }
    return b;
}

static char *g_pending_referrer;
static char *g_pending_initiator;
static int g_pending_user_activated = -1;
static gboolean g_pending_reload;

ns_session_storage *
ns_session_storage_new(void)
{
    return (ns_session_storage *)ns_js_session_storage_area_new();
}

void
ns_session_storage_free(ns_session_storage *storage)
{
    if (storage) g_hash_table_unref((GHashTable *)storage);
}

void
ns_browser_set_next_session_storage(ns_session_storage *storage)
{
    ns_js_set_next_session_storage_area((GHashTable *)storage);
}

void
ns_browser_set_next_referrer(const char *url)
{
    g_free(g_pending_referrer);
    g_pending_referrer = (url && *url) ? g_strdup(url) : NULL;
}

void
ns_browser_set_next_initiator(const char *url)
{
    g_free(g_pending_initiator);
    g_pending_initiator = (url && *url) ? g_strdup(url) : NULL;
}

void
ns_browser_set_next_user_activated(int user_activated)
{
    g_pending_user_activated = user_activated ? 1 : 0;
}

void
ns_browser_set_next_reload(int reload)
{
    g_pending_reload = reload != 0;
}

void
ns_browser_set_color_scheme(int dark)
{
    ns_css_set_color_scheme(dark ? NS_CSS_COLOR_SCHEME_DARK
                                 : NS_CSS_COLOR_SCHEME_LIGHT);
}

void
ns_browser_set_reduced_motion(int reduce)
{
    ns_css_set_reduced_motion(reduce ? NS_CSS_REDUCED_MOTION_REDUCE
                                     : NS_CSS_REDUCED_MOTION_NO_PREFERENCE);
}

static ns_browser *
browser_open_blocked(const char *url, const char *host, int viewport_width,
                     double viewport_height, int settle_ms)
{
    char *html = ns_safebrowsing_interstitial(url, host);
    ns_node *doc = ns_html_parse(html, html ? (gssize)strlen(html) : 0);
    g_free(html);
    return browser_build_from_doc(doc, g_strdup(url), viewport_width,
                                  viewport_height, settle_ms, FALSE,
                                  NULL, NULL, NULL, g_strdup("UTF-8"),
                                  url, NULL);
}

static ns_browser *
browser_open_common(const char *url, int viewport_width, double viewport_height,
                    int settle_ms,
                    const void *body, size_t body_len, const char *content_type)
{
    if (!url || !*url) return NULL;

    g_autofree char *referrer = g_pending_referrer;
    g_pending_referrer = NULL;
    g_autofree char *initiator = g_pending_initiator;
    g_pending_initiator = NULL;
    int pending_user_activated = g_pending_user_activated;
    g_pending_user_activated = -1;
    gboolean reload = g_pending_reload;
    g_pending_reload = FALSE;

    if (g_str_has_prefix(url, NS_UNSAFE_CONTINUE_SCHEME)) {
        const char *rest = url;
        while (g_str_has_prefix(rest, NS_UNSAFE_CONTINUE_SCHEME))
            rest += strlen(NS_UNSAFE_CONTINUE_SCHEME);
        if (!browser_url_is_http(rest)) return NULL;
        char *real = g_strdup(rest);
        char *host = ns_url_host_from(real);
        if (host) {
            ns_safebrowsing_allow_host(host);
            g_free(host);
        }
        g_pending_referrer = g_steal_pointer(&referrer);
        g_pending_initiator = g_steal_pointer(&initiator);
        g_pending_user_activated = pending_user_activated;
        g_pending_reload = reload;
        ns_browser *b = browser_open_common(real, viewport_width,
                                            viewport_height, settle_ms,
                                            body, body_len, content_type);
        g_free(real);
        return b;
    }

    {
        char *host = ns_url_host_from(url);
        if (host && ns_safebrowsing_blocked(host)) {
            ns_browser *b = browser_open_blocked(url, host, viewport_width,
                                                 viewport_height, settle_ms);
            g_free(host);
            return b;
        }
        g_free(host);
    }

    char *file_url = resolve_local_path(url);
    const char *fetch_url = file_url ? file_url : url;

    char *stripped_url = body ? NULL : ns_url_strip_tracking_params(fetch_url);
    if (stripped_url)
        fetch_url = stripped_url;

    char *https_url = body ? NULL : ns_net_https_first_upgrade(fetch_url);

    GError *err = NULL;
    ns_response *resp = NULL;
    gboolean user_activated = pending_user_activated >= 0
        ? pending_user_activated != 0 : referrer == NULL && initiator == NULL;
    if (https_url) {
        resp = reload ? ns_engine_reload_blocking(https_url, referrer, &err)
                      : ns_engine_navigate_blocking(https_url, referrer,
                                                    initiator,
                                                    user_activated, &err);
        if (resp && !resp->error && resp->body) {
            fetch_url = https_url;
        } else {
            if (resp) ns_response_free(resp);
            resp = NULL;
            g_clear_error(&err);
        }
    }
    if (!resp && body)
        resp = ns_engine_navigate_post_blocking(fetch_url, referrer,
                                                initiator, body,
                                                body_len, content_type,
                                                user_activated, &err);
    else if (!resp && reload)
        resp = ns_engine_reload_blocking(fetch_url, referrer, &err);
    else if (!resp)
        resp = ns_engine_navigate_blocking(fetch_url, referrer, initiator,
                                           user_activated, &err);
    char *landed_host = resp && resp->final_url
        ? ns_url_host_from(resp->final_url) : NULL;
    if (landed_host && ns_safebrowsing_blocked(landed_host)) {
        char *landed = g_strdup(resp->final_url);
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(file_url);
        g_free(stripped_url);
        g_free(https_url);
        ns_browser *b = browser_open_blocked(landed, landed_host,
                                             viewport_width, viewport_height,
                                             settle_ms);
        g_free(landed);
        g_free(landed_host);
        return b;
    }
    g_free(landed_host);
    if (resp && !body && (resp->error || resp->status >= 400) &&
        (!resp->body || resp->body->len == 0)) {
        gboolean tls_failure = resp->error &&
            g_str_has_prefix(fetch_url, "https://");
        char *html = ns_build_error_page(fetch_url,
                                         resp->error ? 0 : resp->status,
                                         resp->error);
        if (html) {
            if (!resp->body)
                resp->body = g_byte_array_new();
            g_byte_array_set_size(resp->body, 0);
            g_byte_array_append(resp->body, (const guint8 *)html, strlen(html));
            g_free(html);
            g_free(resp->error);
            resp->error = NULL;
            g_free(resp->content_type);
            resp->content_type = g_strdup("text/html; charset=utf-8");
            g_free(resp->final_url);
            resp->final_url = g_strdup(fetch_url);
            if (tls_failure)
                resp->security = NS_SEC_INVALID;
        }
    }
    if (!resp || resp->error || !resp->body) {
        if (resp) ns_response_free(resp);
        g_clear_error(&err);
        g_free(file_url);
        g_free(stripped_url);
        g_free(https_url);
        return NULL;
    }
    g_clear_error(&err);

    char *base = g_strdup(resp->final_url ? resp->final_url : fetch_url);
    if (!body) {
        char *base_stripped = ns_url_strip_tracking_params(base);
        if (base_stripped) {
            g_free(base);
            base = base_stripped;
        }
    }
    gboolean bfcache_ok = !body &&
        (g_str_has_prefix(base, "http://") ||
         g_str_has_prefix(base, "https://")) &&
        resp->status >= 200 && resp->status < 400 &&
        !headers_have_no_store(resp->raw_headers);
    char *refresh_hdr = g_strdup(resp->refresh);
    char *doc_language = g_strdup(resp->content_language);
    char *csp_header = g_strdup(resp->csp_header);
    g_free(file_url);
    g_free(stripped_url);
    g_free(https_url);
    browser_prepare_document_response(resp);

    char *doc_charset = NULL;
    char *decoded = ns_html_decode_body_full((const char *)resp->body->data,
                                             resp->body->len,
                                             resp->content_type,
                                             &doc_charset);
    const ns_config *parse_cfg = ns_config_get();
    gboolean scripting_on = !parse_cfg || parse_cfg->javascript_enabled;
    ns_node *doc = ns_html_parse_with_scripting(
        decoded ? decoded : "", decoded ? (gssize)strlen(decoded) : 0,
        scripting_on);
    g_free(decoded);
    int sec = resp->security;
    if (sec == NS_SEC_NONE) {
        const char *u = resp->final_url ? resp->final_url : fetch_url;
        if (g_str_has_prefix(u, "https://"))
            sec = NS_SEC_SECURE;
        else if (g_str_has_prefix(u, "http://"))
            sec = NS_SEC_PLAIN;
    }
    char *ip = g_strdup(resp->remote_ip);
    ns_js_navigation_timing navigation_timing =
        ns_engine_navigation_timing(resp);
    ns_response_free(resp);

    ns_browser *b = browser_build_from_doc(doc, base, viewport_width,
                                           viewport_height, settle_ms,
                                           bfcache_ok, refresh_hdr, doc_language,
                                           csp_header, doc_charset, url,
                                           &navigation_timing);
    if (b) {
        b->security = sec;
        b->remote_ip = ip;
    } else {
        g_free(ip);
    }
    return b;
}

ns_browser *
ns_browser_open(const char *url, int viewport_width, int settle_ms)
{
    return browser_open_common(url, viewport_width, 0.0, settle_ms,
                               NULL, 0, NULL);
}

ns_browser *
ns_browser_open_viewport(const char *url, int viewport_width,
                         double viewport_height, int settle_ms)
{
    return browser_open_common(url, viewport_width, viewport_height, settle_ms,
                               NULL, 0, NULL);
}

ns_browser *
ns_browser_open_post(const char *url, int viewport_width, int settle_ms,
                     const void *body, size_t body_len,
                     const char *content_type)
{
    return browser_open_common(url, viewport_width, 0.0, settle_ms,
                               body, body_len, content_type);
}

ns_browser *
ns_browser_open_post_viewport(const char *url, int viewport_width,
                              double viewport_height, int settle_ms,
                              const void *body, size_t body_len,
                              const char *content_type)
{
    return browser_open_common(url, viewport_width, viewport_height, settle_ms,
                               body, body_len, content_type);
}

char *
ns_browser_render_text(ns_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    ns_engine_dump_text(browser->layout, out);
    char *text = malloc(out->len + 1);
    if (text) {
        memcpy(text, out->str, out->len);
        text[out->len] = '\0';
    }
    g_string_free(out, TRUE);
    return text;
}

char *
ns_browser_dump_dom(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    GString *out = ns_node_dump(browser->doc);
    return out ? g_string_free(out, FALSE) : NULL;
}

char *
ns_browser_dump_layout(ns_browser *browser)
{
    if (!browser || !browser->layout) return NULL;
    GString *out = g_string_new(NULL);
    ns_engine_dump_layout(browser->layout, 0, out);
    return g_string_free(out, FALSE);
}

static int
node_count_walk(const ns_node *n, int depth)
{
    if (depth > 1024) return 0;
    int c = 0;
    for (; n; n = n->next_sibling)
        c += 1 + node_count_walk(n->first_child, depth + 1);
    return c;
}

static int
box_count_walk(const ns_box *b)
{
    int c = 0;
    for (; b; b = b->next_sibling)
        c += 1 + box_count_walk(b->first_child);
    return c;
}

static void
perf_dump_threads(GString *out)
{
    g_string_append(out, "Threads\n");
#if defined(__linux__)
    GError *err = NULL;
    GDir *dir = g_dir_open("/proc/self/task", 0, &err);
    if (!dir) {
        g_string_append_printf(out, "  (unavailable: %s)\n",
                               err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    const char *tid;
    int n = 0;
    while ((tid = g_dir_read_name(dir))) {
        char *path = g_strdup_printf("/proc/self/task/%s/comm", tid);
        char *comm = NULL;
        if (g_file_get_contents(path, &comm, NULL, NULL) && comm) {
            g_string_append_printf(out, "  [%s] %s", tid, g_strchomp(comm));
            g_string_append_c(out, '\n');
            n++;
        }
        g_free(comm);
        g_free(path);
    }
    g_dir_close(dir);
    g_string_append_printf(out, "  %d thread%s total\n", n, n == 1 ? "" : "s");
#else
    g_string_append(out, "  (thread enumeration is Linux-only)\n");
#endif
}

char *
ns_browser_dump_performance(ns_browser *browser)
{
    if (!browser) return NULL;
    GString *out = g_string_new(NULL);

    int pw = 0, ph = 0;
    ns_browser_page_size(browser, &pw, &ph);

    g_string_append(out, "Document\n");
    g_string_append_printf(out, "  url         %s\n",
                           browser->base_url ? browser->base_url : "");
    g_string_append_printf(out, "  charset     %s\n",
                           browser->doc_charset ? browser->doc_charset : "");
    g_string_append_printf(out, "  DOM nodes   %d\n",
                           node_count_walk(browser->doc, 0));
    g_string_append_printf(out, "  layout boxes %d\n",
                           box_count_walk(browser->layout));
    g_string_append_printf(out, "  page size   %d x %d px\n", pw, ph);

    g_string_append(out, "\nLayout\n");
    g_string_append_printf(out, "  viewport    %d x %.0f px\n",
                           browser->vw, browser->vh);
    g_string_append_printf(out, "  last reflow %.2f ms\n",
                           browser->relayout_cost_us / 1000.0);
    g_string_append_printf(out, "  oscillation %d\n", browser->layout_osc);

    g_string_append_c(out, '\n');
    if (browser->js)
        ns_js_dump_stats(browser->js, out);

    g_string_append_c(out, '\n');
    perf_dump_threads(out);

    return g_string_free(out, FALSE);
}

int
ns_browser_render_image(ns_browser *browser, const char *path)
{
    if (!browser || !browser->layout || !path) return -1;

    browser_wait_images(browser);

    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);

    int rc;
    gsize len = strlen(path);
    if (len >= 4 && g_ascii_strcasecmp(path + len - 4, ".pdf") == 0)
        rc = ns_engine_write_pdf(browser->layout, path);
    else
        rc = ns_engine_write_png(browser->layout, path);

    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);
    return rc;
}

GPtrArray *
ns_browser_print_pages(ns_browser *browser, ns_print_setup *out_setup)
{
    if (!browser || !browser->doc || !out_setup) return NULL;

    browser_wait_images(browser);

    ns_print_setup setup;
    ns_print_setup_default(&setup);

    int saved_vw = browser->vw;
    double saved_vh = browser->vh;

    ns_css_set_print_media(TRUE);
    browser->vw = (int)(setup.width - setup.margin_left - setup.margin_right);
    browser->vh = setup.height - setup.margin_top - setup.margin_bottom;
    browser_relayout(browser);

    const ns_css_page_rule *rule = ns_render_page_rule();
    if (rule) {
        ns_print_setup_apply_page_rule(&setup, rule);
        int w = (int)(setup.width - setup.margin_left - setup.margin_right);
        if (w > 0 && w != browser->vw) {
            browser->vw = w;
            browser->vh = setup.height - setup.margin_top - setup.margin_bottom;
            browser_relayout(browser);
        }
    }

    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    GPtrArray *pages = ns_engine_print_recordings(browser->layout, &setup);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);

    ns_css_set_print_media(FALSE);
    browser->vw = saved_vw;
    browser->vh = saved_vh;
    browser_relayout(browser);

    *out_setup = setup;
    return pages;
}

int
ns_browser_tick(ns_browser *browser, int budget_ms)
{
    if (!browser) return -1;
    if (budget_ms < 0) budget_ms = 0;

    if (browser->refresh_due_us && !browser->pending_nav &&
        g_get_monotonic_time() >= browser->refresh_due_us) {
        browser->refresh_due_us = 0;
        browser->pending_nav = browser->refresh_url;
        browser->refresh_url = NULL;
    }

    gint64 deadline = g_get_monotonic_time() + (gint64)budget_ms * 1000;
    gboolean changed = FALSE;
    gboolean video_changed = FALSE;
    gboolean other_changed = FALSE;
    int guard = 0;
    if (browser->js &&
        !browser->pending_scroll &&
        (fabs(browser->cur_scroll_x - browser->js_scroll_x) > 0.5 ||
         fabs(browser->cur_scroll_y - browser->js_scroll_y) > 0.5)) {
        browser->js_scroll_x = browser->cur_scroll_x;
        browser->js_scroll_y = browser->cur_scroll_y;
        ns_js_note_viewport_scroll(browser->js, browser->cur_scroll_x,
                                   browser->cur_scroll_y);
    }
    if (browser->hover_restyle_pending) {
        gint64 now = g_get_monotonic_time();
        gint64 min_gap = browser->relayout_cost_us * 2;
        if (min_gap < 60000) min_gap = 60000;
        if (now - browser->hover_relayout_us >= min_gap) {
            browser->hover_restyle_pending = FALSE;
            browser->hover_relayout_us = now;
            browser_relayout(browser);
            browser->dirty = FALSE;
            changed = TRUE;
        }
    }
    changed |= browser_sync_scroll_timelines(browser);
    for (;;) {
        gint64 now = g_get_monotonic_time();
        if (browser->images && ns_image_cache_tick(browser->images, now)) {
            changed = TRUE;
            other_changed = TRUE;
        }
        if (browser->videos && browser->layout) {
            if (browser->videos_noted_seq != browser->layout_seq) {
                ns_video_cache_discover(browser->videos, browser->layout,
                                        browser->doc, now);
                ns_video_cache_note_layout(browser->videos, browser->layout);
                browser->videos_noted_seq = browser->layout_seq;
            }
            if (ns_video_cache_tick(browser->videos, now)) {
                changed = TRUE;
                video_changed = TRUE;
            }
        }
        if (browser->anim && ns_anim_tick(browser->anim, now)) {
            changed = TRUE;
            other_changed = TRUE;
            browser_note_anim_tick(browser);
            if (ns_anim_needs_layout(browser->anim)) browser->dirty = TRUE;
        }
        if (browser->anim && browser->js)
            ns_js_dispatch_anim_events(browser->js, browser->anim);
        if (browser->js && ns_js_run_animation_frame(browser->js)) {
            changed = TRUE;
            other_changed = TRUE;
        }

        gboolean did_iter = FALSE;
        int it = 0;
        while (g_main_context_pending(NULL) && it++ < 64 &&
               g_get_monotonic_time() < deadline) {
            g_main_context_iteration(NULL, FALSE);
            did_iter = TRUE;
        }

        if (!did_iter) break;
        if (++guard >= 4096) break;
        if (g_get_monotonic_time() >= deadline) break;
    }
    if (browser->svg_external_seen != ns_svg_external_generation()) {
        browser->svg_external_seen = ns_svg_external_generation();
        changed = TRUE;
        other_changed = TRUE;
    }
    changed |= browser_sync_scroll_timelines(browser);
    if (browser->js && ns_js_consume_mutated(browser->js))
        browser->dirty = TRUE;
    if (browser->dirty && browser_mutation_relayout_due(browser)) {
        if (browser_relayout_from_mutation(browser)) {
            changed = TRUE;
            other_changed = TRUE;
            browser->dirty = FALSE;
            if (browser->videos && browser->layout) {
                ns_video_cache_discover(browser->videos, browser->layout,
                                        browser->doc, g_get_monotonic_time());
                ns_video_cache_note_layout(browser->videos, browser->layout);
            }
        }
    }
    browser_follow_scroll_anchor(browser);
    if (browser->pending_scroll) changed = TRUE;
    if (browser->repaint_requested) {
        browser->repaint_requested = FALSE;
        changed = TRUE;
    }
    (void)video_changed;
    (void)other_changed;
    if (!changed && browser->videos &&
        ns_video_cache_waiting_growth(browser->videos))
        changed = TRUE;
    return changed ? 1 : 0;
}

int
ns_browser_animating(ns_browser *browser)
{
    if (!browser) return 0;
    if (browser->dirty) return 1;
    if (browser->hover_restyle_pending) return 1;
    if (browser->repaint_requested) return 1;
    if (browser->refresh_due_us || browser->refresh_url) return 1;
    if (browser_images_outstanding(browser) > 0) return 1;
    if (browser->js && ns_js_has_pending_animation_frame(browser->js))
        return 1;
    if (browser->js && ns_js_needs_tick(browser->js))
        return 1;
    if (browser->anim && ns_anim_has_active(browser->anim))
        return 1;
    if (browser->images && ns_image_cache_animating(browser->images))
        return 1;
    if (browser->videos && ns_video_cache_animating(browser->videos))
        return 1;
    if (ns_svg_external_pending() > 0)
        return 1;
    return 0;
}

static gboolean
browser_needs_frame_tick(ns_browser *browser)
{
    return browser->dirty || browser->hover_restyle_pending ||
           browser->repaint_requested ||
           (browser->refresh_url && !browser->refresh_due_us) ||
           browser_images_outstanding(browser) > 0 ||
           (browser->js && ns_js_needs_frame_tick(browser->js)) ||
           (browser->anim && ns_anim_has_active(browser->anim)) ||
           (browser->videos && ns_video_cache_animating(browser->videos)) ||
           ns_svg_external_pending() > 0;
}

static int
browser_main_context_timeout_ms(void)
{
    GMainContext *ctx = g_main_context_default();
    if (!g_main_context_acquire(ctx)) return 0;
    int max_priority = G_PRIORITY_DEFAULT;
    int timeout = 0;
    if (!g_main_context_prepare(ctx, &max_priority)) {
        GPollFD fds[32];
        g_main_context_query(ctx, max_priority, &timeout, fds,
                             G_N_ELEMENTS(fds));
    }
    g_main_context_release(ctx);
    return timeout;
}

static gint64
browser_earliest(gint64 a, gint64 b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    return a < b ? a : b;
}

int
ns_browser_wake_ms(ns_browser *browser)
{
    if (!browser || !ns_browser_animating(browser)) return -1;
    if (browser_needs_frame_tick(browser)) return 0;
    gint64 now = g_get_monotonic_time();
    gint64 due = browser->refresh_due_us > 0 ? browser->refresh_due_us : -1;
    if (browser->images)
        due = browser_earliest(due,
                               ns_image_cache_next_frame_us(browser->images,
                                                            now));
    int timeout = browser_main_context_timeout_ms();
    if (timeout >= 0)
        due = browser_earliest(due, now + (gint64)timeout * 1000);
    if (due <= now) return 0;
    gint64 ms = (due - now + 999) / 1000;
    return ms > G_MAXINT ? G_MAXINT : (int)ms;
}

int
ns_browser_set_viewport(ns_browser *browser, int css_width, double css_height)
{
    if (!browser || !browser->doc || css_width <= 0) return -1;
    if (css_height <= 0.0) css_height = (double)css_width * 0.75;
    if (css_width == browser->vw && css_height == browser->vh) return 0;
    browser->vw = css_width;
    browser->vh = css_height;
    ns_css_set_viewport((double)browser->vw, browser->vh);
    if (browser->js) {
        ns_js_sync_window_metrics(browser->js);
        ns_js_dispatch_resize(browser->js);
    }
    browser_damp_reset(browser);
    browser_relayout(browser);
    return 0;
}

int
ns_browser_set_device_pixel_ratio(ns_browser *browser, double dppx)
{
    if (!(dppx > 0)) return -1;
    ns_css_set_device_pixel_ratio(dppx);
    if (!browser || !browser->doc || browser->dppx == dppx) return 0;
    browser->dppx = dppx;
    if (browser->js) {
        ns_js_sync_window_metrics(browser->js);
        ns_js_reeval_media_queries(browser->js);
    }
    browser_relayout(browser);
    return 1;
}

int
ns_browser_set_viewport_width(ns_browser *browser, int css_width)
{
    return ns_browser_set_viewport(browser, css_width,
                                   (double)css_width * 0.75);
}

void
ns_browser_window_action_applied(ns_browser *browser)
{
    if (browser && browser->js)
        ns_js_window_action_applied(browser->js);
}

int
ns_browser_page_size(ns_browser *browser, int *out_width, int *out_height)
{
    if (!browser || !browser->layout) return -1;
    if (browser->page_size_seq == browser->layout_seq &&
        browser->page_size_vw == browser->vw &&
        browser->page_size_vh == browser->vh) {
        if (out_width)  *out_width  = browser->page_size_w;
        if (out_height) *out_height = browser->page_size_h;
        return 0;
    }
    gboolean hide_x = root_axis_overflow_hidden(browser->layout,
                                                NS_CSS_OVERFLOW_X);
    gboolean hide_y = root_axis_overflow_hidden(browser->layout,
                                                NS_CSS_OVERFLOW_Y);
    double w = hide_x ? browser->vw : browser->layout->content_width;
    if (!(w > 0)) w = browser->vw;
    double bottom = hide_y ? browser->vh : browser->layout->content_height;
    if (!hide_y)
        bottom = ns_box_max_bottom(browser->layout, bottom);
    if (!(bottom > 0)) bottom = 0;
    int ypad = (!hide_y && bottom > browser->vh + 0.5) ? 32 : 0;
    browser->page_size_seq = browser->layout_seq;
    browser->page_size_vw = browser->vw;
    browser->page_size_vh = browser->vh;
    browser->page_size_w = (int)w;
    browser->page_size_h = (int)bottom + ypad;
    if (out_width)  *out_width  = browser->page_size_w;
    if (out_height) *out_height = browser->page_size_h;
    return 0;
}

int
ns_browser_render_rgba(ns_browser *browser, int scroll_x, int scroll_y,
                       int width, int height, double scale,
                       unsigned char *out, int stride)
{
    if (!browser || !browser->layout || !out) return -1;
    if (width <= 0 || height <= 0 || stride < width * 4) return -1;
    if (!(scale > 0)) scale = 1.0;

    browser->cur_scroll_x = (double)scroll_x;
    browser->cur_scroll_y = (double)scroll_y;
    browser->cur_scale = scale;
    browser->cur_viewport_h = (double)height / scale;
    browser_ensure_images(browser);
    if (browser->videos && browser->layout) {
        gint64 now = g_get_monotonic_time();
        ns_video_cache_discover(browser->videos, browser->layout,
                                browser->doc, now);
        ns_video_cache_note_layout(browser->videos, browser->layout);
    }

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return -1;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_set_tolerance(cr, scale > 0 ? 0.5 / scale : 0.5);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);


    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    ns_paint_set_search(browser->search_case, browser->search_active);
    const char *highlight = browser->search_query;
    if (ns_selection_has_range(&browser->selection))
        ns_paint_with_selection(cr, browser->layout, highlight,
                                &browser->selection);
    else
        ns_paint(cr, browser->layout, highlight);
    ns_paint_set_search(FALSE, NULL);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);

    cairo_destroy(cr);
    cairo_surface_flush(surf);

    const unsigned char *src = cairo_image_surface_get_data(surf);
    int src_stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < height; y++) {
        const unsigned char *srow = src + (size_t)y * src_stride;
        unsigned char *drow = out + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            uint32_t px;
            memcpy(&px, srow + x * 4, sizeof px);
            drow[x * 4 + 0] = (unsigned char)((px >> 16) & 0xFF);
            drow[x * 4 + 1] = (unsigned char)((px >> 8) & 0xFF);
            drow[x * 4 + 2] = (unsigned char)(px & 0xFF);
            drow[x * 4 + 3] = (unsigned char)((px >> 24) & 0xFF);
        }
    }
    cairo_surface_destroy(surf);
    return 0;
}

static const ns_style *
viewport_snap_style(ns_browser *browser)
{
    static const char *const sources[] = { "html", "body" };
    if (!browser->doc || !browser->styles) return NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(sources); i++) {
        const ns_node *n = ns_node_find_first_element(browser->doc, sources[i]);
        const ns_style *s = n ? g_hash_table_lookup(browser->styles, n) : NULL;
        const char *type = s ? ns_style_keyword(s, NS_CSS_SCROLL_SNAP_TYPE)
                             : NULL;
        if (type && strcmp(type, "none") != 0) return s;
    }
    return NULL;
}

int
ns_browser_snap_document(ns_browser *browser, double viewport_w,
                         double viewport_h, int prev_x, int prev_y,
                         int *scroll_x, int *scroll_y)
{
    if (!browser || !browser->layout || !scroll_x || !scroll_y) return 0;
    const ns_style *s = viewport_snap_style(browser);
    if (!s) return 0;

    int page_w = 0, page_h = 0;
    if (ns_browser_page_size(browser, &page_w, &page_h) != 0) return 0;
    double max_x = page_w - viewport_w;
    double max_y = page_h - viewport_h;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    double x = *scroll_x, y = *scroll_y;
    if (!ns_box_scroll_snap_viewport(browser->layout, s, viewport_w, viewport_h,
                                     max_x, max_y, prev_x, prev_y, &x, &y))
        return 0;

    int nx = (int)(x + 0.5), ny = (int)(y + 0.5);
    if (nx == *scroll_x && ny == *scroll_y) return 0;
    *scroll_x = nx;
    *scroll_y = ny;
    return 1;
}

static void
browser_note_viewport(ns_browser *browser, int scroll_x, int scroll_y,
                      int height, double scale)
{
    browser->cur_scroll_x = (double)scroll_x;
    browser->cur_scroll_y = (double)scroll_y;
    browser->cur_scale = scale;
    browser->cur_viewport_h = (double)height / scale;
    ns_box_set_hit_viewport(browser->cur_scroll_x, browser->cur_scroll_y);
    browser_sync_scroll_timelines(browser);
    browser_ensure_images(browser);
    if (browser->videos && browser->layout) {
        gint64 now = g_get_monotonic_time();
        ns_video_cache_discover(browser->videos, browser->layout,
                                browser->doc, now);
        ns_video_cache_note_layout(browser->videos, browser->layout);
    }
}

static cairo_t *
browser_paint_context(unsigned char *out, int width, int height, int stride,
                      double scale)
{
    cairo_surface_t *surf =
        cairo_image_surface_create_for_data(out, CAIRO_FORMAT_ARGB32,
                                            width, height, stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_surface_destroy(surf);
    cairo_set_tolerance(cr, scale > 0 ? 0.5 / scale : 0.5);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_clip(cr);
    return cr;
}

static void
browser_paint_context_done(cairo_t *cr)
{
    cairo_surface_t *surf = cairo_surface_reference(cairo_get_target(cr));
    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
}

static void
browser_paint_begin(ns_browser *browser)
{
    ns_paint_set_js(browser->js);
    ns_paint_set_anim(browser->anim);
    ns_paint_set_search(browser->search_case, browser->search_active);
    ns_paint_set_caret_visible(browser->caret_paint_visible);
}

static void
browser_paint_end(void)
{
    ns_paint_set_caret_visible(TRUE);
    ns_paint_set_search(FALSE, NULL);
    ns_paint_set_anim(NULL);
    ns_paint_set_js(NULL);
}

static const ns_selection *
browser_paint_selection(ns_browser *browser)
{
    return ns_selection_has_range(&browser->selection) ? &browser->selection
                                                       : NULL;
}

static void
browser_dump_frame(cairo_surface_t *surf)
{
    const char *dump_dir = g_getenv("NS_FRAME_DUMP");
    if (!dump_dir) return;
    static int frame_no;
    if (frame_no % 30 == 0) {
        char *path = g_strdup_printf("%s/frame-%05d.png", dump_dir, frame_no);
        cairo_surface_write_to_png(surf, path);
        g_free(path);
    }
    frame_no++;
}

int
ns_browser_render_argb32(ns_browser *browser, int scroll_x, int scroll_y,
                         int width, int height, double scale,
                         unsigned char *out, int stride)
{
    if (!browser || !browser->layout || !out) return -1;
    if (width <= 0 || height <= 0 || stride < width * 4) return -1;
    if (!(scale > 0)) scale = 1.0;

    browser_note_viewport(browser, scroll_x, scroll_y, height, scale);
    cairo_t *cr = browser_paint_context(out, width, height, stride, scale);
    if (!cr) return -1;
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);

    browser_paint_begin(browser);
    gint64 paint_t0 = g_get_monotonic_time();
    const ns_selection *sel = browser_paint_selection(browser);
    if (sel)
        ns_paint_with_selection(cr, browser->layout, browser->search_query,
                                sel);
    else
        ns_paint(cr, browser->layout, browser->search_query);
    if (g_getenv("NS_PROFILE"))
        g_printerr("[profile] paint %6.1fms %dx%d\n",
                   (double)(g_get_monotonic_time() - paint_t0) / 1000.0,
                   width, height);
    browser_paint_end();

    cairo_surface_t *surf = cairo_surface_reference(cairo_get_target(cr));
    browser_paint_context_done(cr);
    browser_dump_frame(surf);
    cairo_surface_destroy(surf);
    return 0;
}

void
ns_browser_note_viewport(ns_browser *browser, int scroll_x, int scroll_y,
                         int height, double scale)
{
    if (!browser || !browser->layout) return;
    browser_note_viewport(browser, scroll_x, scroll_y, height,
                          scale > 0 ? scale : 1.0);
}

int
ns_browser_layers_prepare(ns_browser *browser, int scroll_x, int scroll_y,
                          int width, int height, double scale,
                          ns_paint_layer_plan *plan)
{
    if (!browser || !browser->layout || !plan || width <= 0 || height <= 0)
        return -1;
    if (!(scale > 0)) scale = 1.0;
    browser_note_viewport(browser, scroll_x, scroll_y, height, scale);
    unsigned char *row = g_malloc((size_t)width * 4u);
    cairo_t *cr = browser_paint_context(row, width, 1, width * 4, scale);
    if (!cr) {
        g_free(row);
        return -1;
    }
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);
    browser_paint_begin(browser);
    ns_paint_plan_layers(cr, browser->layout, plan);
    browser_paint_end();
    browser_paint_context_done(cr);
    g_free(row);
    return plan->dynamic ? -1 : 0;
}

typedef struct doc_tile_paint {
    unsigned char *const *bufs;
    cairo_t             **upper;
    gboolean             *used;
    int                   width, height, stride;
    int                   scroll_x, tile_y;
    double                scale;
} doc_tile_paint;

static void
doc_tile_transform(cairo_t *cr, const doc_tile_paint *p)
{
    cairo_translate(cr, 0, -(double)p->tile_y);
    cairo_scale(cr, p->scale, p->scale);
    cairo_translate(cr, -(double)p->scroll_x, 0);
}

static cairo_t *
doc_tile_upper(int index, gpointer data)
{
    doc_tile_paint *p = data;
    if (p->upper[index]) return p->upper[index];
    memset(p->bufs[index + 1], 0, (size_t)p->stride * (size_t)p->height);
    p->upper[index] = browser_paint_context(p->bufs[index + 1], p->width,
                                            p->height, p->stride, p->scale);
    if (p->upper[index]) doc_tile_transform(p->upper[index], p);
    p->used[index] = p->upper[index] != NULL;
    return p->upper[index];
}

static gboolean
layer_target_ok(const ns_browser *browser, const ns_paint_layer_plan *plan,
                int width, int height, int stride)
{
    return browser && browser->layout && plan && width > 0 && height > 0 &&
           stride >= width * 4;
}

int
ns_browser_render_doc_tile(ns_browser *browser,
                           const ns_paint_layer_plan *plan, int scroll_x,
                           int tile_y, int width, int height, double scale,
                           unsigned char *const *bufs, int stride,
                           gboolean *upper_used)
{
    if (!bufs || !layer_target_ok(browser, plan, width, height, stride))
        return -1;
    if (!(scale > 0)) scale = 1.0;
    int n_upper = (int)plan->vp->len;
    doc_tile_paint p = { bufs, g_new0(cairo_t *, MAX(n_upper, 1)),
                         upper_used, width, height, stride, scroll_x, tile_y,
                         scale };
    for (int i = 0; i < n_upper; i++) upper_used[i] = FALSE;
    cairo_t *cr = browser_paint_context(bufs[0], width, height, stride,
                                        scale);
    if (!cr) {
        g_free(p.upper);
        return -1;
    }
    doc_tile_transform(cr, &p);
    browser_paint_begin(browser);
    ns_paint_set_video_layer(ns_video_cache_layer_gen(browser->videos),
                             (double)tile_y);
    gboolean ok = ns_paint_doc_layers(cr, doc_tile_upper, &p, browser->layout,
                                      browser->search_query,
                                      browser_paint_selection(browser), plan);
    ns_paint_set_video_layer(0, 0);
    browser_paint_end();
    for (int i = 0; i < n_upper; i++)
        if (p.upper[i]) browser_paint_context_done(p.upper[i]);
    g_free(p.upper);
    browser_paint_context_done(cr);
    return ok ? 0 : -2;
}

void
ns_browser_set_video_layer_gen(ns_browser *browser, guint gen)
{
    if (browser) ns_video_cache_set_layer_gen(browser->videos, gen);
}

gboolean
ns_browser_video_layer_frames_pending(ns_browser *browser)
{
    return browser && ns_video_cache_layer_frames_pending(browser->videos);
}

typedef struct video_layer_visit {
    void   (*fn)(const ns_video_layer *layer, gpointer data);
    gpointer data;
} video_layer_visit;

static void
video_layer_visit_one(ns_video *v, gpointer data)
{
    video_layer_visit *visit = data;
    ns_video_layer layer = { .id = v->seq, .x = v->layer_x, .y = v->layer_y,
                             .w = v->layer_w, .h = v->layer_h,
                             .sent_serial = &v->layer_sent_serial };
    layer.n_frames = ns_video_layer_frames(v, layer.frames,
                                           NS_VIDEO_LAYER_FRAMES);
    if (layer.n_frames > 0) visit->fn(&layer, visit->data);
}

void
ns_browser_foreach_video_layer(ns_browser *browser,
                               void (*fn)(const ns_video_layer *layer,
                                          gpointer data),
                               gpointer data)
{
    if (!browser || !fn) return;
    video_layer_visit visit = { fn, data };
    ns_video_cache_foreach_layered(browser->videos, video_layer_visit_one,
                                   &visit);
}

static const ns_paint_vp_capture *
plan_capture(const ns_paint_layer_plan *plan, int index)
{
    if (!plan || index < 0 || index >= (int)plan->vp->len) return NULL;
    return &g_array_index(plan->vp, ns_paint_vp_capture, index);
}

static double
browser_viewport_css_h(const ns_browser *browser)
{
    return browser->cur_viewport_h > 0 ? browser->cur_viewport_h
                                       : browser->vh;
}

static int
fixed_layer_info(const ns_box *box, double vh, ns_vp_layer_info *out)
{
    gboolean exact = ns_box_subtree_extent_y(box, &out->top, &out->bottom);
    out->top = exact ? MAX(out->top - NS_VP_LAYER_PAD, 0) : 0;
    out->bottom = exact ? MIN(out->bottom + NS_VP_LAYER_PAD, vh) : vh;
    if (out->bottom <= out->top) out->top = out->bottom = 0;
    return 0;
}

static int
sticky_layer_info(const ns_browser *browser, const ns_box *box, double vh,
                  ns_vp_layer_info *out)
{
    gboolean exact = ns_box_subtree_extent_y(box, &out->top, &out->bottom);
    if (!ns_box_sticky_y_model(box, vh, &out->sticky)) return -1;
    double dx = 0, dy = 0;
    ns_box_sticky_offset(box, browser->cur_scroll_x, browser->cur_scroll_y,
                         browser->cur_scroll_x + browser->vw,
                         browser->cur_scroll_y + vh, &dx, &dy);
    out->x_offset = dx;
    double pad = exact ? NS_VP_LAYER_PAD : vh / 2;
    out->top -= pad;
    out->bottom += pad;
    return out->bottom - out->top <= vh * 4 ? 0 : -1;
}

int
ns_browser_vp_layer_info(ns_browser *browser, const ns_paint_layer_plan *plan,
                         int index, ns_vp_layer_info *out)
{
    memset(out, 0, sizeof *out);
    const ns_paint_vp_capture *cap = plan_capture(plan, index);
    if (!browser || !browser->layout || !cap) return -1;
    out->kind = cap->kind;
    double vh = browser_viewport_css_h(browser);
    return cap->kind == NS_PAINT_VP_STICKY
        ? sticky_layer_info(browser, cap->box, vh, out)
        : fixed_layer_info(cap->box, vh, out);
}

int
ns_browser_render_vp_layer(ns_browser *browser,
                           const ns_paint_layer_plan *plan, int index,
                           int scroll_x, int scroll_y, int origin_y,
                           int width, int height, double scale,
                           unsigned char *out, int stride)
{
    const ns_paint_vp_capture *cap = plan_capture(plan, index);
    if (!out || !cap || !layer_target_ok(browser, plan, width, height, stride))
        return -1;
    if (!(scale > 0)) scale = 1.0;
    memset(out, 0, (size_t)stride * (size_t)height);
    cairo_t *cr = browser_paint_context(out, width, height, stride, scale);
    if (!cr) return -1;
    cairo_translate(cr, 0, -(double)origin_y);
    cairo_scale(cr, scale, scale);
    if (cap->kind == NS_PAINT_VP_STICKY)
        cairo_translate(cr, -(double)scroll_x, 0);
    else
        cairo_translate(cr, -(double)scroll_x, -(double)scroll_y);
    browser_paint_begin(browser);
    ns_paint_vp_layer(cr, browser->layout, cap, scroll_x, scroll_y,
                      browser->search_query, browser_paint_selection(browser));
    browser_paint_end();
    browser_paint_context_done(cr);
    return 0;
}

static gboolean
box_under_fixed(const ns_box *b)
{
    for (const ns_box *a = b; a; a = a->parent)
        if (ns_box_is_fixed(a)) return TRUE;
    return FALSE;
}

typedef struct scroller_walk {
    double vx, vy;
    GString *out;
    int left;
    ns_js *js;
    int wheel_left;
} scroller_walk;

static void
scroller_rect_emit(const ns_box *b, double ox, double oy, scroller_walk *w)
{
    gboolean fixed = box_under_fixed(b);
    double x = b->x + b->margin.left + b->border.left + ox;
    double y = b->y + b->margin.top + b->border.top + oy;
    double bw = b->content_width + b->padding.left + b->padding.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom;
    if (fixed) {
        x -= w->vx;
        y -= w->vy;
    }
    if (!(bw > 0 && bh > 0 && isfinite(x) && isfinite(y))) return;
    int axes = (b->scroll_max_x > 0 ? 1 : 0) | (b->scroll_max_y > 0 ? 2 : 0);
    g_string_append_printf(w->out, "sr %d %d %d %d %d %d\n", (int)floor(x),
                           (int)floor(y), (int)ceil(bw), (int)ceil(bh), axes,
                           fixed ? 1 : 0);
    w->left--;
}

static void
wheel_rect_emit(const ns_box *b, double ox, double oy, scroller_walk *w)
{
    gboolean fixed = box_under_fixed(b);
    double x = b->x + b->margin.left + ox;
    double y = b->y + b->margin.top + oy;
    double bw = b->content_width + b->padding.left + b->padding.right +
                b->border.left + b->border.right;
    double bh = b->content_height + b->padding.top + b->padding.bottom +
                b->border.top + b->border.bottom;
    if (fixed) {
        x -= w->vx;
        y -= w->vy;
    }
    if (!(bw > 0 && bh > 0 && isfinite(x) && isfinite(y))) return;
    g_string_append_printf(w->out, "wr %d %d %d %d %d\n", (int)floor(x),
                           (int)floor(y), (int)ceil(bw), (int)ceil(bh),
                           fixed ? 1 : 0);
    w->wheel_left--;
}

static void
scroller_rects_walk(const ns_box *b, double ox, double oy, scroller_walk *w)
{
    if (!b || w->left <= 0) return;
    double hx = 0, hy = 0;
    ns_box_hit_offset(b, &hx, &hy);
    ox += hx;
    oy += hy;
    if (b->scrolls && (b->scroll_max_x > 0 || b->scroll_max_y > 0))
        scroller_rect_emit(b, ox, oy, w);
    if (w->wheel_left > 0 && b->dom && ns_js_node_blocks_wheel(w->js, b->dom))
        wheel_rect_emit(b, ox, oy, w);
    double cx = ox - b->scroll_x, cy = oy - b->scroll_y;
    for (const ns_box *c = b->first_child; c; c = c->next_sibling)
        scroller_rects_walk(c, cx, cy, w);
}

void
ns_browser_scroller_rects(ns_browser *browser, GString *out, int max_rects)
{
    if (!browser || !browser->layout || !out) return;
    gboolean wheel_all = ns_js_document_blocks_wheel(browser->js);
    scroller_walk w = { browser->cur_scroll_x, browser->cur_scroll_y, out,
                        max_rects, browser->js, wheel_all ? 0 : max_rects };
    scroller_rects_walk(browser->layout, 0, 0, &w);
    if (w.left <= 0)
        g_string_append(out, "sr-all\n");
    if (wheel_all || w.wheel_left <= 0)
        g_string_append(out, "wr-all\n");
}

int
ns_browser_wheel_event(ns_browser *browser, int x, int y, int dx, int dy,
                       int mods, int *out_changed)
{
    if (out_changed) *out_changed = 0;
    if (!browser || !browser->layout || !browser->js) return 0;
    const ns_node *node = ns_box_hit_node(browser->layout, (double)x, (double)y);
    if (!node) return 0;
    gboolean prevented = FALSE;
    ns_js_dispatch_wheel(browser->js, node,
                         (double)x - browser->cur_scroll_x,
                         (double)y - browser->cur_scroll_y,
                         (double)x, (double)y, (double)dx, (double)dy,
                         (mods & 1) != 0, (mods & 2) != 0, (mods & 4) != 0,
                         (mods & 8) != 0, &prevented);
    if (ns_js_consume_mutated(browser->js)) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        if (out_changed) *out_changed = 1;
    }
    return prevented ? 1 : 0;
}

gboolean
ns_browser_canvas_color(ns_browser *browser, double rgba_out[4])
{
    return browser && browser->layout &&
           ns_paint_canvas_color(browser->layout, rgba_out);
}

static const ns_node *browser_hit_node(ns_browser *browser, int x, int y);

static gboolean
browser_node_is_hyperlink(const ns_node *n)
{
    return ns_node_is_element_named(n, "a") ||
           ns_node_is_element_named(n, "area");
}

static char *
browser_link_near(ns_browser *browser, int x, int y, int probes)
{
    if (!browser || !browser->layout) return NULL;

    static const int kR = 6;
    static const int probe[][2] = {
        { 0, 0 },
        { 0, -kR }, { 0, kR }, { -kR, 0 }, { kR, 0 },
        { -kR, -kR }, { kR, -kR }, { -kR, kR }, { kR, kR },
    };
    for (int i = 0; i < probes; i++) {
        int px = x + probe[i][0], py = y + probe[i][1];
        const char *href = ns_box_hit_link(browser->layout,
                                           (double)px, (double)py);
        if (!href || !*href) {
            const ns_node *node = browser_hit_node(browser, px, py);
            for (const ns_node *a = node; a && (!href || !*href); a = a->parent)
                if (browser_node_is_hyperlink(a))
                    href = ns_element_get_attr(a, "href");
        }
        if (href && *href) return browser_resolve_navigation(browser, href);
    }
    return NULL;
}

char *
ns_browser_link_at(ns_browser *browser, int x, int y)
{
    return browser_link_near(browser, x, y, 9);
}

char *
ns_browser_link_under(ns_browser *browser, int x, int y)
{
    return browser_link_near(browser, x, y, 1);
}

char *
ns_browser_cursor_at(ns_browser *browser, int x, int y)
{
    static const char *const known[] = {
        "default", "none", "context-menu", "help", "pointer", "progress",
        "wait", "cell", "crosshair", "text", "vertical-text", "alias",
        "copy", "move", "no-drop", "not-allowed", "grab", "grabbing",
        "all-scroll", "col-resize", "row-resize", "n-resize", "e-resize",
        "s-resize", "w-resize", "ne-resize", "nw-resize", "se-resize",
        "sw-resize", "ew-resize", "ns-resize", "nesw-resize", "nwse-resize",
        "zoom-in", "zoom-out",
    };
    if (!browser || !browser->layout || !browser->styles) return NULL;

    const ns_node *node = browser_hit_node(browser, x, y);
    const ns_node *form_node =
        ns_box_hit_form_dom(browser->layout, (double)x, (double)y);

    const ns_style *style = NULL;
    for (const ns_node *n = node; n && !style; n = n->parent)
        style = g_hash_table_lookup(browser->styles, n);
    if (!style) return NULL;

    const ns_css_value *v = style->values[NS_CSS_CURSOR];
    char *match = NULL;
    if (v && v->kind == NS_CSS_V_KEYWORD && v->u.keyword) {
        char **tokens = g_strsplit_set(v->u.keyword, ", \t", -1);
        for (int i = 0; tokens && tokens[i]; i++) {
            if (!tokens[i][0]) continue;
            for (gsize k = 0; k < G_N_ELEMENTS(known); k++)
                if (g_ascii_strcasecmp(tokens[i], known[k]) == 0) {
                    g_free(match);
                    match = g_strdup(known[k]);
                }
        }
        g_strfreev(tokens);
    }
    if (match) return match;

    if (ns_box_hit_link(browser->layout, (double)x, (double)y)) return NULL;
    for (const ns_node *n = node; n; n = n->parent)
        if (browser_node_is_hyperlink(n) &&
            ns_element_get_attr(n, "href")) return NULL;
    if (form_node)
        return ns_node_is_text_input(form_node) ? g_strdup("text") : NULL;
    for (const ns_node *n = node; n; n = n->parent)
        if (ns_node_is_contenteditable_host(n)) return g_strdup("text");
    if (ns_selection_text_at(browser->layout, (double)x, (double)y))
        return g_strdup("text");
    return NULL;
}

static gsize
browser_utf8_boundary(const char *s, gsize off)
{
    gsize len = s ? strlen(s) : 0;
    if (off >= len)
        return len;
    while (off > 0 && (((unsigned char)s[off] & 0xc0) == 0x80))
        off--;
    return off;
}

static ns_node *
browser_focused_field(ns_browser *b)
{
    const ns_node *f = b->js ? ns_js_focused_node(b->js) : NULL;
    return ns_node_is_editable(f) ? (ns_node *)f : NULL;
}

static gboolean
browser_field_writable(const ns_node *field)
{
    if (!ns_node_is_editable(field)) return FALSE;
    if (ns_node_is_contenteditable_host(field)) return TRUE;
    return !ns_element_get_attr(field, "readonly") &&
           !ns_element_get_attr(field, "disabled");
}

static gboolean
browser_field_copyable(const ns_node *field)
{
    if (!ns_node_is_element_named(field, "input")) return TRUE;
    const char *type = ns_element_get_attr(field, "type");
    return !type || g_ascii_strcasecmp(type, "password") != 0;
}

static gboolean
browser_field_selection(ns_browser *b, const ns_node *field, gsize *lo,
                        gsize *hi)
{
    const char *cur = ns_node_editable_value(field);
    gsize caret = browser_utf8_boundary(cur, b->caret_byte);
    gsize anchor = browser_utf8_boundary(cur, b->sel_anchor_byte);
    *lo = MIN(caret, anchor);
    *hi = MAX(caret, anchor);
    return *lo < *hi;
}

static char *
browser_field_selected_text(ns_browser *b, const ns_node *field)
{
    gsize lo, hi;
    if (!browser_field_selection(b, field, &lo, &hi) ||
        !browser_field_copyable(field))
        return NULL;
    return g_strndup(ns_node_editable_value(field) + lo, hi - lo);
}

static int
browser_field_edit_state(ns_browser *b, const ns_node *field)
{
    gsize lo, hi;
    int state = NS_BROWSER_EDIT_FIELD;
    if (browser_field_writable(field))
        state |= NS_BROWSER_EDIT_WRITABLE;
    if (browser_field_selection(b, field, &lo, &hi) &&
        browser_field_copyable(field))
        state |= NS_BROWSER_EDIT_SELECTION;
    return state;
}

static void browser_input_replace(ns_browser *b, ns_node *node, gsize del_start,
                                  gsize del_end, const char *insert,
                                  const char *input_type);

static void
browser_sync_js_selection(ns_browser *browser)
{
    if (!browser || !browser->js) return;
    char *text = ns_selection_collect_text(browser->layout,
                                           &browser->selection);
    double x = 0, y = 0, w = 0, h = 0;
    if (ns_selection_bounds(browser->layout, &browser->selection,
                            &x, &y, &w, &h)) {
        x -= browser->cur_scroll_x;
        y -= browser->cur_scroll_y;
    }
    ns_js_set_selection(browser->js, text,
                        ns_selection_has_range(&browser->selection),
                        x, y, w, h);
    g_free(text);
}

static void
browser_field_select_all(ns_browser *b, const ns_node *field)
{
    b->sel_anchor_byte = 0;
    b->caret_byte = strlen(ns_node_editable_value(field));
    browser_relayout(b);
    b->dirty = FALSE;
}

static char *
browser_field_cut(ns_browser *b, ns_node *field)
{
    gsize lo, hi;
    char *text = browser_field_writable(field)
               ? browser_field_selected_text(b, field) : NULL;
    if (!text) return NULL;
    browser_field_selection(b, field, &lo, &hi);
    browser_input_replace(b, field, lo, hi, NULL, "deleteByCut");
    browser_relayout(b);
    b->dirty = FALSE;
    return text;
}

static char *
browser_copy_text(ns_browser *b)
{
    ns_node *field = browser_focused_field(b);
    gsize lo, hi;
    if (field && browser_field_selection(b, field, &lo, &hi))
        return browser_field_selected_text(b, field);
    return ns_selection_collect_text(b->layout, &b->selection);
}

static gboolean
browser_node_is_range(const ns_node *n)
{
    if (!ns_node_is_element_named(n, "input")) return FALSE;
    const char *type = ns_element_get_attr(n, "type");
    return type && g_ascii_strcasecmp(type, "range") == 0 &&
           !ns_element_get_attr(n, "disabled");
}

static double
browser_range_attr(const ns_node *n, const char *name, double fallback)
{
    const char *v = ns_element_get_attr(n, name);
    if (!v || !*v) return fallback;
    char *end = NULL;
    double d = g_ascii_strtod(v, &end);
    return end && end != v && isfinite(d) ? d : fallback;
}

static void
browser_range_set_from_x(ns_browser *b, ns_node *n, int x)
{
    double rx, ry, rw, rh;
    if (!ns_box_inline_rect_for_dom(b->layout, n, &rx, &ry, &rw, &rh))
        return;
    double lo = browser_range_attr(n, "min", 0);
    double hi = browser_range_attr(n, "max", 100);
    if (hi < lo) hi = lo;
    double knob = 16.0;
    double track = rw - 4.0 - knob;
    double frac = track > 0 ? ((double)x - (rx + 2.0 + knob / 2.0)) / track
                            : 0.5;
    frac = CLAMP(frac, 0.0, 1.0);
    double v = lo + frac * (hi - lo);
    const char *step_attr = ns_element_get_attr(n, "step");
    gboolean any = step_attr && g_ascii_strcasecmp(step_attr, "any") == 0;
    double step = any ? 0 : browser_range_attr(n, "step", 1);
    if (step > 0) {
        v = lo + round((v - lo) / step) * step;
        while (v > hi + 1e-9) v -= step;
        if (v < lo) v = lo;
    }
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof buf, "%.10g", v);
    if (g_strcmp0(ns_node_editable_value(n), buf) == 0) return;
    ns_node_set_editable_value(n, buf);
    if (b->js) {
        ns_js_dispatch_event(b->js, n, "input", NULL);
        (void)ns_js_consume_mutated(b->js);
    }
    b->dirty = TRUE;
}

static void
browser_range_drag_end(ns_browser *b)
{
    ns_node *n = b->range_drag;
    b->range_drag = NULL;
    if (n && b->js &&
        g_strcmp0(ns_node_editable_value(n), b->range_drag_start) != 0) {
        ns_js_dispatch_event(b->js, n, "change", NULL);
        if (ns_js_consume_mutated(b->js)) b->dirty = TRUE;
    }
    g_clear_pointer(&b->range_drag_start, g_free);
}

char *
ns_browser_select(ns_browser *browser, int kind, int x, int y)
{
    if (!browser || !browser->layout) return NULL;
    if (browser->range_drag && kind == 1) {
        browser_range_set_from_x(browser, browser->range_drag, x);
        if (browser->dirty) {
            browser_relayout(browser);
            browser->dirty = FALSE;
        }
        return NULL;
    }
    ns_node *field = browser_focused_field(browser);
    switch (kind) {
    case 0: ns_selection_anchor_at(&browser->selection, browser->layout,
                                   (double)x, (double)y); break;
    case 1: ns_selection_extend_to(&browser->selection, browser->layout,
                                   (double)x, (double)y);
            browser->selection_dragged = TRUE; break;
    case 2: ns_selection_clear(&browser->selection); break;
    case 3: if (field) {
                browser_field_select_all(browser, field);
                return NULL;
            }
            ns_selection_select_all(&browser->selection, browser->layout);
            break;
    case 4: return browser_copy_text(browser);
    case 7: return field ? browser_field_cut(browser, field) : NULL;
    case 5: ns_selection_select_word_at(&browser->selection, browser->layout,
                                        (double)x, (double)y);
            browser->selection_dragged = TRUE; break;
    case 6: ns_selection_select_block_at(&browser->selection, browser->layout,
                                         (double)x, (double)y);
            browser->selection_dragged = TRUE; break;
    default: break;
    }
    browser_sync_js_selection(browser);
    return NULL;
}

static int
browser_button_bit(int button)
{
    switch (button) {
    case 0:  return 1;
    case 1:  return 4;
    case 2:  return 2;
    case 3:  return 8;
    case 4:  return 16;
    default: return 0;
    }
}

static void
browser_mouse_dispatch(ns_browser *b, const ns_node *target, const char *type,
                       int x, int y, int button, int detail, int mods,
                       const ns_node *related, gboolean *prevented)
{
    ns_js_dispatch_mouse_event_detail(b->js, target, type,
                                      (double)x - b->cur_scroll_x,
                                      (double)y - b->cur_scroll_y,
                                      (double)x, (double)y, button,
                                      b->buttons_down, detail,
                                      (mods & 1) != 0, (mods & 2) != 0,
                                      (mods & 4) != 0, (mods & 8) != 0,
                                      related, prevented);
}

static void
browser_hover_dispatch(ns_browser *b, const ns_node *target, int x, int y,
                       const char *ptr_type, const char *mouse_type,
                       const ns_node *related, int mods)
{
    if (!b->js || !target) return;
    browser_mouse_dispatch(b, target, ptr_type, x, y, 0, -1, mods, related,
                           NULL);
    browser_mouse_dispatch(b, target, mouse_type, x, y, 0, -1, mods, related,
                           NULL);
}

int
ns_browser_hover(ns_browser *browser, int x, int y)
{
    return ns_browser_hover_full(browser, x, y, 0, -1);
}

int
ns_browser_hover_full(ns_browser *browser, int x, int y, int mods,
                      int buttons)
{
    if (!browser || !browser->layout) return -1;
    if (buttons >= 0) browser->buttons_down = buttons;

    const ns_node *node = browser_hit_node(browser, x, y);
    if (browser->js) node = ns_js_pointer_event_target(browser->js, node);

    browser_prune_cached_nodes(browser);
    if (browser->range_drag) {
        browser_range_set_from_x(browser, browser->range_drag, x);
        if (browser->dirty) {
            browser_relayout(browser);
            browser->dirty = FALSE;
            return 1;
        }
    }
    const ns_node *prev = browser->hover_node;
    gboolean changed = node != prev;
    browser->hover_node = node;

    gboolean dirty = FALSE;
    if (browser->js) {
        gboolean was_dirty = browser->dirty;
        if (changed) {
            browser_hover_dispatch(browser, prev, x, y, "pointerout",
                                   "mouseout", node, mods);
            browser_hover_dispatch(browser, prev, x, y, "pointerleave",
                                   "mouseleave", node, mods);
            browser_hover_dispatch(browser, node, x, y, "pointerover",
                                   "mouseover", prev, mods);
            browser_hover_dispatch(browser, node, x, y, "pointerenter",
                                   "mouseenter", prev, mods);
        }
        browser_hover_dispatch(browser, node, x, y, "pointermove",
                               "mousemove", NULL, mods);
        if (ns_js_consume_mutated(browser->js)) dirty = TRUE;
        if (browser->dirty && !was_dirty) dirty = TRUE;
    }

    gboolean hover_restyle = changed && ns_render_page_uses_hover() &&
                             !ns_selection_has_range(&browser->selection);
    if (dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        return 1;
    }
    if (hover_restyle) {
        gint64 now = g_get_monotonic_time();
        gint64 min_gap = browser->relayout_cost_us * 2;
        if (min_gap < 60000) min_gap = 60000;
        if (now - browser->hover_relayout_us >= min_gap) {
            browser->hover_relayout_us = now;
            browser->hover_restyle_pending = FALSE;
            browser_relayout(browser);
            browser->dirty = FALSE;
            return 1;
        }
        browser->hover_restyle_pending = TRUE;
    }
    return 0;
}

int
ns_browser_scroll_at(ns_browser *browser, int x, int y, int dx, int dy)
{
    return ns_browser_scroll_at_full(browser, x, y, dx, dy, NULL);
}

int
ns_browser_scroll_at_full(ns_browser *browser, int x, int y, int dx, int dy,
                          int *out_snapped)
{
    if (out_snapped) *out_snapped = 0;
    if (!browser || !browser->layout) return 0;

    ns_box *box = ns_box_hit_scrollable(browser->layout, (double)x, (double)y);
    if (!box) return 0;

    double prev_x = box->scroll_x, prev_y = box->scroll_y;
    int consumed = 0;
    if (dy != 0 && box->scroll_max_y > 0) {
        double ny = box->scroll_y + dy;
        if (ny < 0) ny = 0;
        if (ny > box->scroll_max_y) ny = box->scroll_max_y;
        if (ny != box->scroll_y) { box->scroll_y = ny; consumed = 1; }
    }
    if (dx != 0 && box->scroll_max_x > 0) {
        double nx = box->scroll_x + dx;
        if (nx < 0) nx = 0;
        if (nx > box->scroll_max_x) nx = box->scroll_max_x;
        if (nx != box->scroll_x) { box->scroll_x = nx; consumed = 1; }
    }

    if (consumed) {
        double moved_x = box->scroll_x, moved_y = box->scroll_y;
        ns_box_scroll_snap_from(box, prev_x, prev_y);
        if (out_snapped)
            *out_snapped = box->scroll_x != moved_x ||
                           box->scroll_y != moved_y;
        if (browser->js && box->dom)
            ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
        browser_scrolled(browser);
    }
    return consumed;
}

static gboolean
sb_vgeom(const ns_box *b, double *track_x, double *track_w,
         double *track_y, double *track_h, double *thumb_y, double *thumb_h)
{
    if (!b || b->scroll_max_y <= 0) return FALSE;
    double py = b->y + b->margin.top + b->border.top;
    double ph = b->content_height + b->padding.top + b->padding.bottom;
    if (ph <= 16.0) return FALSE;
    double px = b->x + b->margin.left + b->border.left;
    double pw = b->content_width + b->padding.left + b->padding.right;
    double tw = 8.0;
    double th = ph - 2.0;
    double total = ph + b->scroll_max_y;
    double thh = th * (ph / total);
    if (thh < 16.0) thh = 16.0;
    if (thh > th) thh = th;
    *track_x = px + pw - tw - 1.0;
    *track_w = tw;
    *track_y = py + 1.0;
    *track_h = th;
    *thumb_h = thh;
    *thumb_y = *track_y + (th - thh) * (b->scroll_y / b->scroll_max_y);
    return TRUE;
}

static ns_box *
box_find_scrollable_by_dom(ns_box *root, const ns_node *node)
{
    if (!root || !node) return NULL;
    if (root->dom == node && root->scrolls) return root;
    for (ns_box *c = root->first_child; c; c = c->next_sibling) {
        ns_box *m = box_find_scrollable_by_dom(c, node);
        if (m) return m;
    }
    return NULL;
}

int
ns_browser_scrollbar_press(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout) return 0;
    double lx = 0, ly = 0;
    ns_box *box = ns_box_hit_scrollbar(browser->layout, (double)x, (double)y,
                                       &lx, &ly);
    if (!box) return 0;

    double tx, tw, ty, th, thy, thh;
    if (!sb_vgeom(box, &tx, &tw, &ty, &th, &thy, &thh)) return 0;
    if (lx < tx - 3.0 || lx > tx + tw + 3.0 || ly < ty || ly > ty + th)
        return 0;

    const ns_node *box_dom = box->dom;
    double grab;
    if (ly >= thy && ly <= thy + thh) {
        grab = ly - thy;
    } else {
        grab = thh / 2.0;
        double ns = (th > thh)
            ? (ly - ty - grab) / (th - thh) * box->scroll_max_y : 0.0;
        if (ns < 0) ns = 0;
        if (ns > box->scroll_max_y) ns = box->scroll_max_y;
        box->scroll_y = ns;
        browser_scrolled(browser);
        if (box_dom && browser->js) {
            ns_js_dispatch_event(browser->js, box_dom, "scroll", NULL);
            box = box_find_scrollable_by_dom(browser->layout, box_dom);
        }
    }

    browser->sb_dragging = TRUE;
    browser->sb_box = box;
    browser->sb_node = box_dom;
    browser->sb_grab = grab;
    return 1;
}

int
ns_browser_scrollbar_drag(ns_browser *browser, int x, int y)
{
    (void)x;
    if (!browser || !browser->sb_dragging) return 0;
    ns_box *box = browser->sb_box;
    if (browser->sb_node)
        box = box_find_scrollable_by_dom(browser->layout, browser->sb_node);
    if (!box) {
        browser->sb_dragging = FALSE;
        browser->sb_box = NULL;
        return 0;
    }
    browser->sb_box = box;

    double tx, tw, ty, th, thy, thh;
    if (!sb_vgeom(box, &tx, &tw, &ty, &th, &thy, &thh) || th <= thh)
        return 0;
    double ns = ((double)y - ty - browser->sb_grab) / (th - thh)
              * box->scroll_max_y;
    if (ns < 0) ns = 0;
    if (ns > box->scroll_max_y) ns = box->scroll_max_y;
    if (ns == box->scroll_y) return 0;
    box->scroll_y = ns;
    browser_scrolled(browser);
    if (box->dom && browser->js)
        ns_js_dispatch_event(browser->js, box->dom, "scroll", NULL);
    return 1;
}

void
ns_browser_scrollbar_release(ns_browser *browser)
{
    if (!browser) return;
    browser->sb_dragging = FALSE;
    browser->sb_box = NULL;
    browser->sb_node = NULL;
}

int
ns_browser_drop_files(ns_browser *browser, int x, int y,
                      const char *const *paths, int n_paths)
{
    if (!browser || !browser->js || !browser->layout || !paths || n_paths <= 0)
        return 0;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_node *target = hit ? hit->dom : NULL;
    if (!target && browser->doc)
        target = ns_node_find_first_element(browser->doc, "body");
    if (!target) target = browser->doc;
    if (!target) return 0;

    ns_js_drag_session *session = ns_js_drag_session_new(browser->js);
    if (!session) return 0;
    for (int i = 0; i < n_paths; i++)
        if (paths[i]) ns_js_drag_session_add_file(session, paths[i]);

    gboolean accept = FALSE;
    gboolean prevented = FALSE;
    ns_js_dispatch_drag_event(browser->js, session, target, "dragenter",
                              x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                              NULL, &prevented);
    if (prevented) accept = TRUE;
    prevented = FALSE;
    ns_js_dispatch_drag_event(browser->js, session, target, "dragover",
                              x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                              NULL, &prevented);
    if (prevented) accept = TRUE;
    if (accept)
        ns_js_dispatch_drag_event(browser->js, session, target, "drop",
                                  x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                                  NULL, &prevented);
    else
        ns_js_dispatch_drag_event(browser->js, session, target, "dragleave",
                                  x, y, x, y, 0, 0, FALSE, FALSE, FALSE, FALSE,
                                  NULL, &prevented);
    ns_js_drag_session_free(session);

    if (ns_js_consume_mutated(browser->js)) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        return 1;
    }
    return 0;
}

char *
ns_browser_console_drain(ns_browser *browser)
{
    if (!browser || !browser->console_buf || browser->console_buf->len == 0)
        return NULL;
    char *out = g_strdup(browser->console_buf->str);
    g_string_truncate(browser->console_buf, 0);
    return out;
}

char *
ns_browser_eval(ns_browser *browser, const char *src)
{
    if (!browser || !browser->js || !src) return NULL;
    browser_damp_reset(browser);
    char *res = ns_js_eval_source(browser->js, src, "devtools-console");
    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    return res;
}

static int
browser_context_field(ns_browser *browser, int x, int y)
{
    if (!browser->layout) return 0;
    const ns_node *form = ns_box_hit_form_dom(browser->layout, (double)x,
                                              (double)y);
    const ns_node *field = ns_node_is_editable(form) ? form : NULL;
    for (const ns_node *n = browser_hit_node(browser, x, y); n && !field;
         n = n->parent)
        if (ns_node_is_contenteditable_host(n)) field = n;
    if (!field) return 0;
    if (ns_js_focused_node(browser->js) != field) {
        ns_js_set_focus(browser->js, field);
        browser->dirty = TRUE;
        if (ns_js_focused_node(browser->js) != field) return 0;
        browser->caret_byte = strlen(ns_node_editable_value(field));
        browser->sel_anchor_byte = browser->caret_byte;
    }
    return browser_field_edit_state(browser, field);
}

int
ns_browser_contextmenu_full(ns_browser *browser, int x, int y, int *out_edit)
{
    if (out_edit) *out_edit = 0;
    if (!browser || !browser->layout || !browser->js) return 0;
    const ns_node *node = browser_hit_node(browser, x, y);
    if (!node) return 0;
    gboolean prevented = FALSE;
    browser_mouse_dispatch(browser, node, "contextmenu", x, y, 2, -1,
                           browser->aux_press_mods, NULL, &prevented);
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    int edit = prevented ? 0 : browser_context_field(browser, x, y);
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }
    if (out_edit) *out_edit = edit;
    return prevented ? 1 : 0;
}

int
ns_browser_contextmenu(ns_browser *browser, int x, int y)
{
    return ns_browser_contextmenu_full(browser, x, y, NULL);
}

void
ns_browser_audio_feedback(ns_browser *browser, const char *line)
{
    if (!browser || !browser->js) return;
    ns_js_audio_feedback(browser->js, line);
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    browser->repaint_requested = TRUE;
}

char *
ns_browser_media_at(ns_browser *browser, int x, int y, int *out_is_video,
                    int *out_stream)
{
    if (out_is_video) *out_is_video = 0;
    if (out_stream) *out_stream = 0;
    if (!browser || !browser->layout) return NULL;

    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    const ns_box *media = NULL;
    for (const ns_box *b = hit; b; b = b->parent) {
        gboolean has_media_url =
            b->media && (b->media->video_src || b->media->video_audio_src);
        gboolean has_internal_video =
            b->dom && b->dom->kind == NS_NODE_ELEMENT &&
            (ns_element_get_attr(b->dom, NS_MEDIA_SRC_ATTR) != NULL ||
             ns_element_get_attr(b->dom, NS_MEDIA_STREAM_ATTR) != NULL);
        if (b->dom && (ns_node_is_element_named(b->dom, "video") ||
                       ns_node_is_element_named(b->dom, "audio") ||
                       has_media_url || has_internal_video)) {
            media = b;
            break;
        }
    }
    if (!media || !media->dom) return NULL;

    if (media->media && media->media->video) {
        ns_video *iv = media->media->video;
        if (iv->is_camera)
            return NULL;
        if (iv->player) {
            ns_video_cache_toggle(browser->videos, iv, g_get_monotonic_time());
            return NULL;
        }
    }
    if (media->dom->kind == NS_NODE_ELEMENT) {
        const char *stream_kind = ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR);
        if (stream_kind && g_strcmp0(stream_kind, "camera") == 0)
            return NULL;
    }

    gboolean is_video =
        ns_node_is_element_named(media->dom, "video") ||
        (media->media && media->media->video_src) ||
        (media->dom->kind == NS_NODE_ELEMENT &&
         (ns_element_get_attr(media->dom, NS_MEDIA_SRC_ATTR) != NULL ||
          ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR) != NULL));
    gboolean force_stream =
        media->dom->kind == NS_NODE_ELEMENT &&
        ns_element_get_attr(media->dom, NS_MEDIA_STREAM_ATTR) != NULL;
    const char *msrc = NULL;
    if (media->media) {
        if (is_video) {
            msrc = media->media->video_src;
            if (!msrc) msrc = media->media->video_audio_src;
        } else {
            msrc = media->media->video_audio_src;
        }
    }
    if ((!msrc || !*msrc) && media->dom->kind == NS_NODE_ELEMENT)
        msrc = ns_element_get_attr(media->dom, NS_MEDIA_SRC_ATTR);
    g_autofree char *base = browser_document_base(browser);
    char *abs = (!force_stream && msrc)
        ? ns_url_resolve(base ? base : browser->base_url, msrc) : NULL;
    gboolean stream = force_stream || !abs || g_str_has_prefix(abs, "blob:") ||
                      g_str_has_prefix(abs, "data:");
    if (!stream && is_video && abs && ns_video_url_is_inline(abs)) {
        g_free(abs);
        return NULL;
    }
    if (stream) {
        g_free(abs);
        abs = browser->base_url ? g_strdup(browser->base_url) : NULL;
    }
    if (!abs) return NULL;
    if (g_str_has_prefix(abs, "file://") &&
        (!browser->base_url || !g_str_has_prefix(browser->base_url, "file://"))) {
        g_free(abs);
        return NULL;
    }
    if (out_is_video) *out_is_video = is_video ? 1 : 0;
    if (out_stream) *out_stream = stream ? 1 : 0;
    return abs;
}

int
ns_browser_find(ns_browser *browser, const char *query, int case_sensitive,
                int direction, int from_y, int *out_total, int *out_current,
                int *out_y)
{
    if (out_total) *out_total = 0;
    if (out_current) *out_current = 0;
    if (out_y) *out_y = 0;
    if (!browser || !browser->layout) return -1;

    gboolean cs = case_sensitive != 0;
    if (!query || !*query) {
        g_clear_pointer(&browser->search_query, g_free);
        browser->search_active = NULL;
        browser->search_case = cs;
        return 0;
    }

    if (!browser->search_query || strcmp(browser->search_query, query) != 0 ||
        browser->search_case != cs) {
        g_free(browser->search_query);
        browser->search_query = g_strdup(query);
        browser->search_active = NULL;
    }
    browser->search_case = cs;

    guint total = ns_box_count_matches(browser->layout, query, cs);
    if (out_total) *out_total = (int)total;
    if (total == 0) {
        browser->search_active = NULL;
        return 0;
    }

    double cur_y = browser->search_active ? browser->search_active->y
                                          : (double)from_y;
    const ns_box *target = NULL;
    if (direction == 2) {
        target = ns_box_first_match_above(browser->layout, query, cur_y, cs);
        if (!target)
            target = ns_box_first_match_above(browser->layout, query,
                                              G_MAXDOUBLE, cs);
    } else if (direction == 1) {
        target = ns_box_first_match_below(browser->layout, query, cur_y + 2,
                                          cs);
        if (!target)
            target = ns_box_first_match_below(browser->layout, query, -1, cs);
    } else {
        target = ns_box_first_match_below(browser->layout, query,
                                          (double)from_y - 1, cs);
        if (!target)
            target = ns_box_first_match_below(browser->layout, query, -1, cs);
    }

    browser->search_active = target;
    if (target) {
        if (out_y) *out_y = (int)target->y;
        if (out_current)
            *out_current = (int)ns_box_match_ordinal(browser->layout, query,
                                                     target, cs);
    }
    return 0;
}

char *
ns_browser_take_post(ns_browser *browser, size_t *out_len, char **out_ct)
{
    if (out_len) *out_len = 0;
    if (out_ct) *out_ct = NULL;
    if (!browser || !browser->pending_post_body) return NULL;
    char *body = browser->pending_post_body;
    browser->pending_post_body = NULL;
    if (out_len) *out_len = browser->pending_post_len;
    browser->pending_post_len = 0;
    if (out_ct) *out_ct = browser->pending_post_ct;
    else        g_free(browser->pending_post_ct);
    browser->pending_post_ct = NULL;
    return body;
}

static void
browser_perform_form_navigation(ns_browser *b, const ns_node *form,
                                const ns_node *clicked)
{
    if (!b || !form || !b->doc) return;
    gboolean from_text = clicked && ns_node_is_text_input(clicked);

    const char *method = ns_element_get_attr(form, "method");
    const char *formmethod = (clicked && !from_text)
                             ? ns_element_get_attr(clicked, "formmethod") : NULL;
    if (formmethod && *formmethod) method = formmethod;
    gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;

    const char *action = ns_element_get_attr(form, "action");
    const char *formaction = (clicked && !from_text)
                             ? ns_element_get_attr(clicked, "formaction") : NULL;
    if (formaction && *formaction) action = formaction;
    g_autofree char *base = browser_document_base(b);
    char *abs_action = (action && *action)
        ? ns_url_resolve(base ? base : b->base_url, action)
        : g_strdup(b->base_url);
    if (!abs_action) return;
    if (!browser_allows_navigation_url(b, abs_action)) {
        g_free(abs_action);
        return;
    }
    if (b->js && !ns_js_csp_form_action_allowed(b->js, abs_action)) {
        g_free(abs_action);
        return;
    }

    const char *accept_charset = ns_element_get_attr(form, "accept-charset");
    ns_form_set_submission_charset(
        (accept_charset && *accept_charset) ? accept_charset
                                            : b->doc_charset);

    if (is_post) {
        GString *body = g_string_new(NULL);
        gboolean first = TRUE;
        ns_form_collect_inputs(form, b->doc, b->doc, body, &first, clicked);
        ns_form_set_submission_charset(NULL);
        g_free(b->pending_post_body);
        g_free(b->pending_post_ct);
        b->pending_post_len = body->len;
        b->pending_post_body = g_string_free(body, FALSE);
        b->pending_post_ct = g_strdup("application/x-www-form-urlencoded");
        g_free(b->pending_nav);
        b->pending_nav = abs_action;
        return;
    }

    GString *query = g_string_new(NULL);
    gboolean first = TRUE;
    ns_form_collect_inputs(form, b->doc, b->doc, query, &first, clicked);
    ns_form_set_submission_charset(NULL);

    char *frag = strchr(abs_action, '#');
    if (frag) *frag = '\0';
    char *full;
    if (query->len == 0) {
        full = g_strdup(abs_action);
    } else {
        full = strchr(abs_action, '?')
            ? g_strdup_printf("%s&%s", abs_action, query->str)
            : g_strdup_printf("%s?%s", abs_action, query->str);
    }
    g_string_free(query, TRUE);
    g_free(abs_action);
    g_free(b->pending_nav);
    b->pending_nav = full;
}

static void
browser_js_form_submit(const ns_node *form, const ns_node *submitter,
                       gpointer user_data)
{
    ns_browser *b = user_data;
    if (!b || !form) return;
    browser_perform_form_navigation(b, form, submitter ? submitter : form);
}

static void
browser_submit_form(ns_browser *b, const ns_node *clicked)
{
    if (!clicked || !b->doc) return;
    gboolean from_text = ns_node_is_text_input(clicked);
    gboolean from_form = ns_node_is_element_named(clicked, "form");
    if (!from_text && !from_form && !ns_form_is_submit_trigger(clicked))
        return;
    const ns_node *form = from_form ? clicked : ns_form_owner(clicked, b->doc);
    if (!form) return;

    if (!ns_element_get_attr(form, "novalidate") &&
        !ns_element_get_attr(clicked, "formnovalidate")) {
        const ns_node *bad = ns_form_first_invalid(form, b->doc, b->doc);
        if (bad) {
            if (b->js) ns_js_dispatch_event(b->js, bad, "invalid", NULL);
            return;
        }
    }

    if (b->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_submit_event(b->js, form, clicked, &prevented);
        if (ns_js_consume_mutated(b->js)) b->dirty = TRUE;
        if (prevented) return;
    }

    browser_perform_form_navigation(b, form, clicked);
}

static const ns_node *
browser_hit_node(ns_browser *browser, int x, int y)
{
    return ns_box_hit_node(browser->layout, (double)x, (double)y);
}

static char *
browser_finish_pointer_input(ns_browser *browser, int *out_changed)
{
    if (browser->js) {
        if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
        if (out_changed) *out_changed = 1;
    }
    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

static char *
browser_press_aux(ns_browser *browser, int x, int y, int mods, int button,
                  int clicks)
{
    ns_js_note_pointer_input(browser->js, TRUE);
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);
    const ns_node *node = browser_hit_node(browser, x, y);
    if (browser->js) node = ns_js_pointer_event_target(browser->js, node);
    gboolean first = browser->buttons_down == 0;
    browser->buttons_down |= browser_button_bit(button);
    browser->aux_press_node = node;
    browser->aux_press_button = button;
    browser->aux_press_mods = mods;
    if (browser->js && node) {
        if (first)
            browser_mouse_dispatch(browser, node, "pointerdown", x, y, button,
                                   -1, mods, NULL, NULL);
        browser_mouse_dispatch(browser, node, "mousedown", x, y, button,
                               clicks, mods, NULL, NULL);
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }
    return browser_finish_pointer_input(browser, NULL);
}

char *
ns_browser_press(ns_browser *browser, int x, int y, int mods)
{
    return ns_browser_press_full(browser, x, y, mods, 0, 1);
}

char *
ns_browser_press_full(ns_browser *browser, int x, int y, int mods, int button,
                      int clicks)
{
    if (!browser || !browser->layout) return NULL;
    if (button != 0)
        return browser_press_aux(browser, x, y, mods, button, clicks);
    ns_js_note_pointer_input(browser->js, TRUE);
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);
    gboolean extending = (mods & 1) != 0 &&
                         ns_selection_has_range(&browser->selection);
    if (!extending) {
        ns_selection_clear(&browser->selection);
        browser_sync_js_selection(browser);
    }
    browser->selection_dragged = FALSE;

    const ns_node *node = browser_hit_node(browser, x, y);
    browser->press_node = node;
    browser->press_x = x;
    browser->press_y = y;
    browser->press_mods = mods;
    browser->press_active = node != NULL;

    ns_css_set_active_node(node);
    if (node && ns_render_page_uses_active()) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    gboolean first = browser->buttons_down == 0;
    browser->buttons_down |= browser_button_bit(0);
    const ns_node *target = browser->js
        ? ns_js_pointer_event_target(browser->js, node) : node;
    if (browser->js && target) {
        if (first)
            browser_mouse_dispatch(browser, target, "pointerdown", x, y, 0,
                                   -1, mods, NULL, NULL);
        browser_mouse_dispatch(browser, target, "mousedown", x, y, 0,
                               clicks, mods, NULL, NULL);
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }

    gboolean in_datalist = FALSE;
    for (const ns_node *a = node; a; a = a->parent)
        if (ns_node_is_element_named(a, "datalist")) { in_datalist = TRUE; break; }
    if (browser->js && !in_datalist) {
        const ns_node *focus = NULL;
        for (const ns_node *a = node; a; a = a->parent)
            if (ns_node_is_focusable(a)) { focus = a; break; }
        ns_js_focus_from_pointer(browser->js, node);
        const char *val = focus ? ns_node_editable_value(focus) : NULL;
        browser->caret_byte = val ? strlen(val) : 0;
        browser->sel_anchor_byte = browser->caret_byte;
        browser->datalist_suppressed =
            !(focus && ns_node_is_element_named(focus, "input") &&
              ns_element_get_attr(focus, "list") != NULL);
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }

    browser_range_drag_end(browser);
    const ns_node *range = ns_box_hit_form_dom(browser->layout, (double)x,
                                               (double)y);
    if (browser_node_is_range(range)) {
        browser->range_drag = (ns_node *)range;
        browser->range_drag_start =
            g_strdup(ns_node_editable_value(browser->range_drag));
        browser_range_set_from_x(browser, browser->range_drag, x);
    }

    if (browser->js) {
        if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

static gboolean
ns_is_dropdown_select(const ns_node *n)
{
    if (!ns_node_is_element_named(n, "select")) return FALSE;
    if (ns_element_get_attr(n, "multiple")) return FALSE;
    const char *sz = ns_element_get_attr(n, "size");
    if (sz && atoi(sz) > 1) return FALSE;
    return TRUE;
}

static gboolean
browser_datalist_click(ns_browser *browser, const ns_node *node)
{
    const ns_node *option = NULL, *dl = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!option && ns_node_is_element_named(a, "option")) option = a;
        if (ns_node_is_element_named(a, "datalist")) { dl = a; break; }
        if (ns_node_is_element_named(a, "select")) return FALSE;
    }
    if (!option || !dl || !browser->js) return FALSE;
    ns_node *inp = (ns_node *)ns_js_focused_node(browser->js);
    if (!inp || !ns_node_is_element_named(inp, "input")) return FALSE;
    const char *ov = ns_element_get_attr(option, "value");
    char *val = (ov && *ov) ? g_strdup(ov) : ns_option_label_dup(option);
    const char *cur = ns_node_editable_value(inp);
    browser_input_replace(browser, inp, 0, cur ? strlen(cur) : 0,
                          val ? val : "", "insertReplacementText");
    ns_js_commit_change(browser->js, inp);
    ns_js_consume_mutated(browser->js);
    g_free(val);
    browser->datalist_suppressed = TRUE;
    browser->dirty = TRUE;
    return TRUE;
}

static gboolean
browser_dropdown_click(ns_browser *browser, const ns_node *node)
{
    if (browser_datalist_click(browser, node)) return TRUE;
    const ns_node *option = NULL, *select = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!option && ns_node_is_element_named(a, "option")) option = a;
        if (ns_node_is_element_named(a, "select")) { select = a; break; }
    }
    if (select && option && !ns_is_dropdown_select(select) &&
        !ns_element_get_attr(select, "disabled")) {
        gboolean multiple = ns_element_get_attr(select, "multiple") != NULL;
        gboolean toggle = multiple && (browser->press_mods & 2) != 0;
        if (browser->js) {
            if (toggle)
                ns_js_select_toggle_option(browser->js, (ns_node *)option);
            else
                ns_js_select_choose_option(browser->js, (ns_node *)option);
            ns_js_consume_mutated(browser->js);
        }
        browser->dirty = TRUE;
        return TRUE;
    }
    if (browser->open_select && option && select == browser->open_select) {
        if (browser->js &&
            ns_js_select_choose_option(browser->js, (ns_node *)option)) {
            ns_js_consume_mutated(browser->js);
            browser->open_select = NULL;
        }
        browser->dirty = TRUE;
        return TRUE;
    }
    if (select && ns_is_dropdown_select(select) &&
        !ns_element_get_attr(select, "disabled")) {
        browser->open_select =
            (browser->open_select == select) ? NULL : select;
        browser->dirty = TRUE;
        return TRUE;
    }
    if (browser->open_select) {
        browser->open_select = NULL;
        browser->dirty = TRUE;
    }
    return FALSE;
}

static const ns_node *
browser_common_ancestor(const ns_node *a, const ns_node *b)
{
    for (const ns_node *x = a; x; x = x->parent)
        for (const ns_node *y = b; y; y = y->parent)
            if (x == y) return x;
    return NULL;
}

char *
ns_browser_release_click(ns_browser *browser, int *out_changed)
{
    if (!browser) {
        if (out_changed) *out_changed = -1;
        return NULL;
    }
    return ns_browser_release_click_at(browser, browser->press_x,
                                       browser->press_y, out_changed);
}

char *
ns_browser_release_click_at(ns_browser *browser, int x, int y,
                            int *out_changed)
{
    return ns_browser_release_full(browser, x, y, 0, 1, out_changed, NULL);
}

static const char *
browser_link_href(ns_browser *browser, const ns_node *node, int x, int y,
                  const char **out_download)
{
    *out_download = NULL;
    for (const ns_node *a = node; a; a = a->parent) {
        if (!browser_node_is_hyperlink(a)) continue;
        const char *h = ns_element_get_attr(a, "href");
        if (h && *h) {
            *out_download = ns_element_get_attr(a, "download");
            return h;
        }
    }
    return browser->layout
        ? ns_box_hit_link(browser->layout, (double)x, (double)y) : NULL;
}

static char *
browser_release_aux(ns_browser *browser, int x, int y, int button, int clicks,
                    int *out_changed, int *out_newtab)
{
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);
    browser_prune_cached_nodes(browser);
    int bit = browser_button_bit(button);
    gboolean held = (browser->buttons_down & bit) != 0;
    browser->buttons_down &= ~bit;
    const ns_node *hit = browser->layout ? browser_hit_node(browser, x, y)
                                         : NULL;
    const ns_node *up = browser->js
        ? ns_js_pointer_event_target(browser->js, hit) : hit;
    const ns_node *press = browser->aux_press_button == button
        ? browser->aux_press_node : NULL;
    if (press) browser->aux_press_node = NULL;
    int mods = browser->aux_press_mods;
    gboolean prevented = FALSE;
    if (browser->js && up) {
        if (!browser->buttons_down)
            browser_mouse_dispatch(browser, up, "pointerup", x, y, button, -1,
                                   mods, NULL, NULL);
        browser_mouse_dispatch(browser, up, "mouseup", x, y, button, clicks,
                               mods, NULL, NULL);
    }
    const ns_node *node = held && press && up
        ? browser_common_ancestor(press, up) : NULL;
    if (browser->js && node)
        browser_mouse_dispatch(browser, node, "auxclick", x, y, button, clicks,
                               mods, NULL, &prevented);
    if (browser->js && !browser->buttons_down)
        ns_js_release_pointer_capture(browser->js);
    if (browser->js && ns_js_consume_mutated(browser->js))
        browser->dirty = TRUE;
    if (!prevented && node && button == 1 && !browser->pending_nav) {
        const char *download = NULL;
        const char *href = browser_link_href(browser, node, x, y, &download);
        if (href && *href && !download) {
            browser->pending_nav = browser_resolve_navigation(browser, href);
            if (out_newtab && browser->pending_nav) *out_newtab = 1;
        }
    }
    return browser_finish_pointer_input(browser, out_changed);
}

char *
ns_browser_release_full(ns_browser *browser, int x, int y, int button,
                        int clicks, int *out_changed, int *out_newtab)
{
    if (out_changed) *out_changed = 0;
    if (out_newtab) *out_newtab = 0;
    if (!browser) {
        if (out_changed) *out_changed = -1;
        return NULL;
    }
    if (button != 0)
        return browser_release_aux(browser, x, y, button, clicks, out_changed,
                                   out_newtab);
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);

    browser_prune_cached_nodes(browser);
    const ns_node *press = browser->press_active ? browser->press_node : NULL;
    const ns_node *up = press;
    if (press && browser->layout &&
        (x != browser->press_x || y != browser->press_y)) {
        const ns_node *hit = browser_hit_node(browser, x, y);
        if (hit) up = hit;
    }
    if (browser->js)
        up = ns_js_pointer_event_target(browser->js, up);
    const ns_node *node = up == press ? press
                                      : browser_common_ancestor(press, up);
    int mods = browser->press_mods;
    browser->press_node = NULL;
    browser->press_active = FALSE;
    browser->buttons_down &= ~browser_button_bit(0);

    gboolean selected = browser->selection_dragged &&
                        ns_selection_has_range(&browser->selection);
    gboolean drag_selected = selected && clicks < 2;
    browser->selection_dragged = FALSE;
    browser_range_drag_end(browser);

    gboolean prevented = FALSE;
    gboolean newtab = FALSE;
    if (browser->js && up) {
        if (!browser->buttons_down)
            browser_mouse_dispatch(browser, up, "pointerup", x, y, 0, -1,
                                   mods, NULL, NULL);
        browser_mouse_dispatch(browser, up, "mouseup", x, y, 0, clicks, mods,
                               NULL, NULL);
    }
    if (browser->js && node) {
        if (!drag_selected) {
            browser_mouse_dispatch(browser, node, "click", x, y, 0, clicks,
                                   mods, NULL, &prevented);
            if (clicks == 2)
                browser_mouse_dispatch(browser, node, "dblclick", x, y, 0,
                                       clicks, mods, NULL, NULL);
        }
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    }
    if (browser->js && !browser->buttons_down)
        ns_js_release_pointer_capture(browser->js);
    if (selected) prevented = TRUE;

    gboolean select_consumed =
        !prevented && node && browser_dropdown_click(browser, node);

    if (!select_consumed) {
    if (!prevented && browser->js && node &&
        ns_js_click_activate(browser->js, node))
        browser->dirty = TRUE;

    if (!prevented && node && browser->js &&
        ns_js_activate_summary(browser->js, node)) {
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    } else if (!prevented && !browser->pending_nav && node &&
        ns_form_is_submit_trigger(node)) {
        browser_submit_form(browser, node);
    } else if (!prevented && node && browser->js && browser->doc &&
               ns_form_is_reset_trigger(node)) {
        ns_node *form = (ns_node *)ns_form_owner(node, browser->doc);
        if (form) {
            ns_js_form_reset(browser->js, form);
            if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
        }
    } else if (!prevented && !browser->pending_nav) {
        const char *download = NULL;
        const char *href = browser_link_href(browser, node, x, y, &download);
        if (href && *href && download)
            browser_js_download(href, download, browser);
        else if (href && *href) {
            browser->pending_nav = browser_resolve_navigation(browser, href);
            newtab = (mods & (2 | 8)) != 0;
        }
    }
    }

    const ns_node *prev = ns_css_set_active_node(NULL);
    if (prev && ns_render_page_uses_active())
        browser->dirty = TRUE;

    char *nav = browser_finish_pointer_input(browser, out_changed);
    if (out_newtab && newtab && nav) *out_newtab = 1;
    return nav;
}

int
ns_browser_release(ns_browser *browser)
{
    int changed = 0;
    char *nav = ns_browser_release_click(browser, &changed);
    free(nav);
    return changed;
}

static void
browser_video_click_toggle(ns_browser *browser, int x, int y)
{
    if (!browser || !browser->layout || !browser->videos) return;
    const ns_box *hit = ns_box_hit_test(browser->layout, (double)x, (double)y);
    for (const ns_box *b = hit; b; b = b->parent) {
        if (!b->media || !b->media->video) continue;
        ns_video *iv = b->media->video;
        if (iv->is_camera || !iv->player) return;
        if (browser->js && b->dom &&
            ns_js_node_has_click_handler(browser->js, b->dom))
            return;
        ns_video_cache_toggle(browser->videos, iv, g_get_monotonic_time());
        browser->dirty = TRUE;
        return;
    }
}

char *
ns_browser_click(ns_browser *browser, int x, int y, int mods)
{
    char *nav = ns_browser_press(browser, x, y, mods);
    if (nav && *nav) {
        if (browser) {
            browser->press_node = NULL;
            browser->press_active = FALSE;
            ns_css_set_active_node(NULL);
        }
        return nav;
    }
    free(nav);
    char *out = ns_browser_release_click(browser, NULL);
    if (!out || !*out)
        browser_video_click_toggle(browser, x, y);
    return out;
}

static void
browser_input_replace(ns_browser *b, ns_node *node, gsize del_start,
                      gsize del_end, const char *insert,
                      const char *input_type)
{
    const char *cur = ns_node_editable_value(node);
    if (!cur) return;
    gsize cur_len = strlen(cur);
    if (del_start > cur_len) del_start = cur_len;
    if (del_end > cur_len) del_end = cur_len;
    if (del_end < del_start) del_end = del_start;
    gsize ins_len = insert ? strlen(insert) : 0;

    GString *s = g_string_sized_new(cur_len - (del_end - del_start) + ins_len);
    g_string_append_len(s, cur, (gssize)del_start);
    if (ins_len) g_string_append_len(s, insert, (gssize)ins_len);
    g_string_append_len(s, cur + del_end, (gssize)(cur_len - del_end));

    const char *data = strcmp(input_type, "insertLineBreak") == 0 ? NULL
                                                                   : insert;
    if (b->js) {
        gboolean prevented = FALSE;
        ns_js_dispatch_input_event(b->js, node, "beforeinput", input_type,
                                   data, &prevented);
        if (prevented || ns_js_focused_node(b->js) != node) {
            g_string_free(s, TRUE);
            return;
        }
        ns_js_note_user_edit(b->js, node, ns_node_editable_value(node));
    }
    ns_node_set_editable_value(node, s->str);
    b->caret_byte = del_start + ins_len;
    b->sel_anchor_byte = b->caret_byte;
    g_string_free(s, TRUE);
    if (b->js) {
        ns_js_dispatch_input_event(b->js, node, "input", input_type, data,
                                   NULL);
        (void)ns_js_consume_mutated(b->js);
    }
}

static gboolean
browser_edit_key(ns_browser *b, ns_node *node, const char *key, int mods)
{
    gboolean shift = (mods & 1) != 0, ctrl = (mods & 2) != 0;
    if (mods & (4 | 8)) return FALSE;
    const char *cur = ns_node_editable_value(node);
    if (!cur || !key || !*key) return FALSE;
    gsize cur_len = strlen(cur);
    if (b->caret_byte > cur_len) b->caret_byte = cur_len;
    if (b->sel_anchor_byte > cur_len) b->sel_anchor_byte = cur_len;
    gsize sel_lo = b->sel_anchor_byte < b->caret_byte ? b->sel_anchor_byte
                                                      : b->caret_byte;
    gsize sel_hi = b->sel_anchor_byte < b->caret_byte ? b->caret_byte
                                                      : b->sel_anchor_byte;
    gboolean has_sel = sel_lo != sel_hi;
    gboolean multiline = (node->name && strcmp(node->name, "textarea") == 0) ||
                         ns_node_is_contenteditable_host(node);

    if (ctrl) {
        if ((key[0] == 'a' || key[0] == 'A') && !key[1]) {
            b->sel_anchor_byte = 0;
            b->caret_byte = cur_len;
            return TRUE;
        }
        return FALSE;
    }
    if (strcmp(key, "Backspace") == 0) {
        if (has_sel)
            browser_input_replace(b, node, sel_lo, sel_hi, NULL,
                                  "deleteContentBackward");
        else if (b->caret_byte > 0) {
            const char *prev = g_utf8_prev_char(cur + b->caret_byte);
            browser_input_replace(b, node, (gsize)(prev - cur), b->caret_byte,
                                  NULL, "deleteContentBackward");
        }
        return TRUE;
    }
    if (strcmp(key, "Delete") == 0) {
        if (has_sel)
            browser_input_replace(b, node, sel_lo, sel_hi, NULL,
                                  "deleteContentForward");
        else if (b->caret_byte < cur_len) {
            const char *nxt = g_utf8_next_char(cur + b->caret_byte);
            browser_input_replace(b, node, b->caret_byte, (gsize)(nxt - cur),
                                  NULL, "deleteContentForward");
        }
        return TRUE;
    }
    if (strcmp(key, "ArrowLeft") == 0) {
        if (has_sel && !shift) b->caret_byte = sel_lo;
        else if (b->caret_byte > 0)
            b->caret_byte = (gsize)(g_utf8_prev_char(cur + b->caret_byte) - cur);
        if (!shift) b->sel_anchor_byte = b->caret_byte;
        return TRUE;
    }
    if (strcmp(key, "ArrowRight") == 0) {
        if (has_sel && !shift) b->caret_byte = sel_hi;
        else if (b->caret_byte < cur_len)
            b->caret_byte = (gsize)(g_utf8_next_char(cur + b->caret_byte) - cur);
        if (!shift) b->sel_anchor_byte = b->caret_byte;
        return TRUE;
    }
    if (strcmp(key, "Home") == 0) {
        b->caret_byte = 0;
        if (!shift) b->sel_anchor_byte = 0;
        return TRUE;
    }
    if (strcmp(key, "End") == 0) {
        b->caret_byte = cur_len;
        if (!shift) b->sel_anchor_byte = cur_len;
        return TRUE;
    }
    if (strcmp(key, "Enter") == 0) {
        if (multiline) {
            browser_input_replace(b, node, sel_lo, sel_hi, "\n",
                                  "insertLineBreak");
            return TRUE;
        }
        if (b->js) {
            ns_js_commit_change(b->js, node);
            if (ns_js_focused_node(b->js) != node) return TRUE;
        }
        browser_submit_form(b, node);
        return TRUE;
    }
    if (g_utf8_strlen(key, -1) == 1 &&
        !g_unichar_iscntrl(g_utf8_get_char(key))) {
        browser_input_replace(b, node, sel_lo, sel_hi, key, "insertText");
        return TRUE;
    }
    return FALSE;
}

static int
browser_key_char_code(const char *key)
{
    if (!key || !*key || !g_utf8_validate(key, -1, NULL))
        return 0;
    const char *next = g_utf8_next_char(key);
    if (!next || *next)
        return 0;
    gunichar ch = g_utf8_get_char(key);
    return g_unichar_iscntrl(ch) ? 0 : (int)ch;
}

static gboolean
browser_accesskey_matches(const ns_node *el, const char *key)
{
    const char *ak = ns_element_get_attr(el, "accesskey");
    if (!ak || !*ak || !key || !*key) return FALSE;
    gsize klen = strlen(key);
    for (const char *p = ak; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        gsize len = (gsize)(p - s);
        if (len == klen && g_ascii_strncasecmp(s, key, len) == 0)
            return TRUE;
    }
    return FALSE;
}

static const ns_node *
browser_find_accesskey(const ns_node *n, const char *key, int depth)
{
    if (!n || depth > 1024) return NULL;
    if (n->kind == NS_NODE_ELEMENT && n->name &&
        g_ascii_strcasecmp(n->name, "template") == 0)
        return NULL;
    if (n->kind == NS_NODE_ELEMENT && browser_accesskey_matches(n, key) &&
        !ns_element_effectively_inert(n))
        return n;
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        const ns_node *m = browser_find_accesskey(c, key, depth + 1);
        if (m) return m;
    }
    return NULL;
}

static gboolean
browser_select_key(ns_browser *browser, ns_node *select, const char *key,
                   int mods)
{
    if (!key || !*key || !browser->js) return FALSE;
    gboolean dropdown = ns_is_dropdown_select(select);
    if (strcmp(key, "ArrowDown") == 0)
        return ns_js_select_step(browser->js, select, +1);
    if (strcmp(key, "ArrowUp") == 0)
        return ns_js_select_step(browser->js, select, -1);
    if (strcmp(key, "Home") == 0)
        return ns_js_select_edge(browser->js, select, FALSE);
    if (strcmp(key, "End") == 0)
        return ns_js_select_edge(browser->js, select, TRUE);
    if (dropdown && (strcmp(key, "Enter") == 0 || strcmp(key, " ") == 0)) {
        browser->open_select = (browser->open_select == select) ? NULL : select;
        return TRUE;
    }
    if (dropdown && strcmp(key, "Escape") == 0 &&
        browser->open_select == select) {
        browser->open_select = NULL;
        return TRUE;
    }
    if ((mods & (2 | 4 | 8)) == 0 && g_utf8_validate(key, -1, NULL) &&
        g_utf8_strlen(key, -1) == 1 && key[0] != ' ' &&
        g_unichar_isprint(g_utf8_get_char(key)))
        return ns_js_select_typeahead(browser->js, select, key);
    return FALSE;
}

static char *
browser_paste_text_for(const ns_node *field, const char *text, gsize lo,
                       gsize hi)
{
    gboolean multiline = ns_node_is_element_named(field, "textarea") ||
                         ns_node_is_contenteditable_host(field);
    gsize len = strlen(text);
    if (!multiline)
        while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r'))
            len--;
    GString *out = g_string_sized_new(len);
    for (gsize i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < len && text[i + 1] == '\n') i++;
            c = '\n';
        }
        g_string_append_c(out, c == '\n' && !multiline ? ' ' : c);
    }
    const char *maxlength = ns_node_is_contenteditable_host(field)
                          ? NULL : ns_element_get_attr(field, "maxlength");
    char *end = NULL;
    long max = maxlength ? strtol(maxlength, &end, 10) : -1;
    if (maxlength && end != maxlength && max >= 0) {
        const char *cur = ns_node_editable_value(field);
        glong kept = g_utf8_strlen(cur, (gssize)lo) +
                     g_utf8_strlen(cur + hi, -1);
        glong room = max > kept ? max - kept : 0;
        if (g_utf8_strlen(out->str, -1) > room)
            g_string_truncate(out, (gsize)(g_utf8_offset_to_pointer(out->str,
                                                                    room) -
                                           out->str));
    }
    return g_string_free(out, FALSE);
}

static void
browser_paste(ns_browser *browser, const ns_node *target, const char *text)
{
    gboolean prevented = FALSE;
    ns_js_dispatch_clipboard_event(browser->js, target, "paste", text,
                                   &prevented);
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    ns_node *field = browser_focused_field(browser);
    if (prevented || !browser_field_writable(field)) return;
    gsize lo, hi;
    browser_field_selection(browser, field, &lo, &hi);
    char *insert = browser_paste_text_for(field, text, lo, hi);
    if (*insert) {
        browser_input_replace(browser, field, lo, hi, insert,
                              "insertFromPaste");
        browser->datalist_suppressed = FALSE;
        browser->dirty = TRUE;
    }
    g_free(insert);
}

char *
ns_browser_key_full(ns_browser *browser, int kind, const char *key,
                    const char *code, int keycode, int mods,
                    int *out_prevented)
{
    if (out_prevented) *out_prevented = 0;
    if (!browser || !browser->js) return NULL;
    ns_js_note_pointer_input(browser->js, FALSE);
    browser_damp_reset(browser);
    g_clear_pointer(&browser->pending_nav, g_free);

    const ns_node *target = ns_js_focused_node(browser->js);
    if (!target && browser->doc)
        target = ns_node_find_first_element(browser->doc, "body");
    if (!target) return NULL;

    if (kind == 2) {
        const ns_node *f = ns_js_focused_node(browser->js);
        gboolean suppressed = browser->key_text_suppressed;
        browser->key_text_suppressed = FALSE;
        if (!suppressed && f && ns_node_editable_value(f) && key && *key &&
            g_utf8_validate(key, -1, NULL)) {
            const char *cur = ns_node_editable_value(f);
            gsize cur_len = cur ? strlen(cur) : 0;
            if (browser->caret_byte > cur_len)
                browser->caret_byte = cur_len;
            if (browser->sel_anchor_byte > cur_len)
                browser->sel_anchor_byte = cur_len;
            gsize lo = browser->sel_anchor_byte < browser->caret_byte
                       ? browser->sel_anchor_byte : browser->caret_byte;
            gsize hi = browser->sel_anchor_byte < browser->caret_byte
                       ? browser->caret_byte : browser->sel_anchor_byte;
            browser_input_replace(browser, (ns_node *)f, lo, hi, key,
                                  "insertText");
            browser->datalist_suppressed = FALSE;
            browser->dirty = TRUE;
        }
    } else if (kind == 4) {
        if (key && *key && g_utf8_validate(key, -1, NULL))
            browser_paste(browser, target, key);
    } else if (kind == 3) {
        int char_code = browser_key_char_code(key);
        if (!browser->keydown_prevented && char_code > 0 &&
            (mods & (2 | 4 | 8)) == 0) {
            gboolean press_prevented = FALSE;
            ns_js_dispatch_key_event_full(browser->js, target, "keypress",
                                          key ? key : "", code ? code : "",
                                          keycode, char_code,
                                          (mods & 1) != 0, FALSE, FALSE, FALSE,
                                          &press_prevented);
            if (press_prevented) {
                browser->key_text_suppressed = TRUE;
                if (out_prevented) *out_prevented = 1;
            }
            if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
        }
        browser->keydown_prevented = FALSE;
    } else {
        gboolean prevented = FALSE;
        ns_js_dispatch_key_event_full(browser->js, target,
                                      kind == 1 ? "keyup" : "keydown",
                                      key ? key : "", code ? code : "",
                                      keycode, 0,
                                      (mods & 1) != 0, (mods & 2) != 0,
                                      (mods & 4) != 0, (mods & 8) != 0,
                                      &prevented);
        if (out_prevented && prevented)
            *out_prevented = 1;
        browser->keydown_prevented = kind == 0 && prevented;
        browser->key_text_suppressed = kind == 0 && prevented;
        if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;

        if (kind == 0 && !prevented && key && strcmp(key, "Enter") == 0 &&
            (mods & (2 | 4 | 8)) == 0) {
            gboolean press_prevented = FALSE;
            ns_js_dispatch_key_event_full(browser->js, target, "keypress",
                                          "Enter", code ? code : "Enter",
                                          13, 13, (mods & 1) != 0,
                                          FALSE, FALSE, FALSE,
                                          &press_prevented);
            if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
            if (press_prevented) {
                prevented = TRUE;
                if (out_prevented) *out_prevented = 1;
            }
        }

        if (kind == 0 && !prevented && (mods & 4) && !(mods & (2 | 8)) &&
            key && g_utf8_validate(key, -1, NULL) &&
            g_utf8_strlen(key, -1) == 1 && browser->doc) {
            const ns_node *ak = browser_find_accesskey(browser->doc, key, 0);
            if (ak) {
                ns_js_set_focus(browser->js, ak);
                ns_js_activate_element(browser->js, ak);
                if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            }
        }

        if (!prevented && kind == 0 && key && strcmp(key, "Tab") == 0) {
            gboolean backward = (mods & 1) != 0;
            const ns_node *next =
                ns_js_sequential_focus_target(browser->js, backward);
            if (next) {
                ns_js_set_focus(browser->js, (ns_node *)next);
                const char *val = ns_node_editable_value(next);
                browser->caret_byte = val ? strlen(val) : 0;
                browser->sel_anchor_byte = browser->caret_byte;
                browser->dirty = TRUE;
            }
        } else if (!prevented && kind == 0) {
            const ns_node *f = ns_js_focused_node(browser->js);
            if (f && ns_node_is_element_named(f, "input") &&
                !browser->datalist_suppressed && key &&
                strcmp(key, "Escape") == 0 &&
                ns_element_get_attr(f, "list") != NULL) {
                browser->datalist_suppressed = TRUE;
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_is_element_named(f, "select") &&
                !ns_element_get_attr(f, "disabled") &&
                browser_select_key(browser, (ns_node *)f, key, mods)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && !(mods & (2 | 4 | 8)) &&
                       ns_js_keyboard_activates(f, key)) {
                if (out_prevented) *out_prevented = 1;
            } else if (f && !(mods & (2 | 4 | 8)) &&
                       ns_js_keyboard_activate(browser->js, f, key, FALSE)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_editable_value(f) &&
                       (mods & NS_KEY_MOD_TEXT_SEPARATE) &&
                       !(mods & (2 | 4 | 8)) &&
                       browser_key_char_code(key) > 0) {
                if (out_prevented) *out_prevented = 1;
            } else if (f && ns_node_editable_value(f) &&
                browser_edit_key(browser, (ns_node *)f, key, mods)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            } else if (key && strcmp(key, "Escape") == 0 &&
                       ns_js_process_close_request(browser->js)) {
                browser->dirty = TRUE;
                if (out_prevented) *out_prevented = 1;
            }
        } else if (!prevented && kind == 1) {
            const ns_node *f = ns_js_focused_node(browser->js);
            if (f && !(mods & (2 | 4 | 8)) &&
                ns_js_keyboard_activate(browser->js, f, key, TRUE))
                browser->dirty = TRUE;
        }
    }

    if (ns_js_run_animation_frame(browser->js)) browser->dirty = TRUE;
    if (ns_js_consume_mutated(browser->js)) browser->dirty = TRUE;
    if (browser->dirty) {
        browser_relayout(browser);
        browser->dirty = FALSE;
    }

    char *nav = browser->pending_nav;
    browser->pending_nav = NULL;
    return nav;
}

char *
ns_browser_key(ns_browser *browser, int kind, const char *key,
               const char *code, int keycode, int mods)
{
    return ns_browser_key_full(browser, kind, key, code, keycode, mods, NULL);
}

int
ns_browser_focused_editable(ns_browser *browser)
{
    if (!browser || !browser->js)
        return 0;
    const ns_node *f = ns_js_focused_node(browser->js);
    return f && ns_node_editable_value(f) ? 1 : 0;
}

int
ns_browser_set_caret_blink_active(ns_browser *browser, int active)
{
    if (!browser) return 0;
    const ns_node *focused = active && ns_browser_focused_editable(browser)
                           ? ns_js_focused_node(browser->js) : NULL;
    gsize caret = focused ? browser->caret_byte : 0;
    gsize anchor = focused ? browser->sel_anchor_byte : 0;
    gint64 now = g_get_monotonic_time();
    if (focused != browser->caret_blink_node ||
        caret != browser->caret_blink_byte ||
        anchor != browser->caret_blink_anchor) {
        browser->caret_blink_epoch_us = now;
        browser->caret_blink_node = focused;
        browser->caret_blink_byte = caret;
        browser->caret_blink_anchor = anchor;
    }
    gint64 since = now - browser->caret_blink_epoch_us;
    gboolean visible = focused &&
        (since >= NS_CARET_BLINK_TIMEOUT_US ||
         (since / NS_CARET_BLINK_US) % 2 == 0);
    gboolean changed = browser->caret_blink_active != (focused != NULL) ||
                       browser->caret_paint_visible != visible;
    browser->caret_blink_active = focused != NULL;
    browser->caret_paint_visible = visible;
    if (!focused) browser->caret_blink_epoch_us = 0;
    return changed ? 1 : 0;
}

int
ns_browser_caret_blinking(ns_browser *browser)
{
    return browser && browser->caret_blink_active &&
           g_get_monotonic_time() - browser->caret_blink_epoch_us <
               NS_CARET_BLINK_TIMEOUT_US ? 1 : 0;
}

char *
ns_browser_focused_editable_value(ns_browser *browser, size_t *out_caret,
                                  size_t *out_anchor)
{
    if (out_caret)
        *out_caret = 0;
    if (out_anchor)
        *out_anchor = 0;
    if (!browser || !browser->js)
        return NULL;
    const ns_node *f = ns_js_focused_node(browser->js);
    const char *cur = f ? ns_node_editable_value(f) : NULL;
    if (!cur)
        return NULL;
    gsize len = strlen(cur);
    gsize caret = browser->caret_byte > len ? len : browser->caret_byte;
    gsize anchor = browser->sel_anchor_byte > len ? len
                                                  : browser->sel_anchor_byte;
    caret = browser_utf8_boundary(cur, caret);
    anchor = browser_utf8_boundary(cur, anchor);
    if (out_caret)
        *out_caret = caret;
    if (out_anchor)
        *out_anchor = anchor;
    return strdup(cur);
}

int
ns_browser_set_focused_editable_selection(ns_browser *browser, size_t caret,
                                          size_t anchor)
{
    if (!browser || !browser->js)
        return 0;
    const ns_node *f = ns_js_focused_node(browser->js);
    const char *cur = f ? ns_node_editable_value(f) : NULL;
    if (!cur)
        return 0;
    browser->caret_byte = browser_utf8_boundary(cur, (gsize)caret);
    browser->sel_anchor_byte = browser_utf8_boundary(cur, (gsize)anchor);
    browser->dirty = TRUE;
    browser_relayout(browser);
    browser->dirty = FALSE;
    return 1;
}

char *
ns_browser_title(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    ns_node *title = ns_node_find_first_element(browser->doc, "title");
    if (!title) return NULL;
    char *raw = ns_node_collect_text(title);
    if (!raw) return NULL;

    GString *out = g_string_new(NULL);
    gboolean prev_ws = TRUE;
    for (const char *p = raw; *p; p++) {
        gboolean ws = (*p == ' ' || *p == '\t' || *p == '\n' ||
                       *p == '\r' || *p == '\f');
        if (ws) {
            if (!prev_ws) g_string_append_c(out, ' ');
            prev_ws = TRUE;
        } else {
            g_string_append_c(out, *p);
            prev_ws = FALSE;
        }
    }
    if (out->len > 0 && out->str[out->len - 1] == ' ')
        g_string_set_size(out, out->len - 1);
    g_free(raw);

    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    char *result = strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

char *
ns_browser_url(ns_browser *browser)
{
    if (!browser || !browser->base_url) return NULL;
    return strdup(browser->base_url);
}

int
ns_browser_security(ns_browser *browser, const char **out_ip)
{
    if (out_ip)
        *out_ip = browser ? browser->remote_ip : NULL;
    return browser ? browser->security : NS_SEC_NONE;
}

char *
ns_browser_take_pending_nav(ns_browser *browser)
{
    if (!browser || !browser->pending_nav) return NULL;
    char *out = strdup(browser->pending_nav);
    g_clear_pointer(&browser->pending_nav, g_free);
    return out;
}

int
ns_browser_take_soft_nav_pushed(ns_browser *browser)
{
    if (!browser || !browser->soft_nav_pushed) return 0;
    browser->soft_nav_pushed = FALSE;
    return 1;
}

int
ns_browser_take_pending_scroll(ns_browser *browser, int *out_scroll_x,
                               int *out_scroll_y)
{
    if (!browser || !browser->pending_scroll) return 0;
    if (out_scroll_x) *out_scroll_x = browser->pending_scroll_x;
    if (out_scroll_y) *out_scroll_y = browser->pending_scroll_y;
    browser->pending_scroll_x = -1;
    browser->pending_scroll = FALSE;
    return 1;
}

int
ns_browser_take_pending_scroll_y(ns_browser *browser, int *out_scroll_y)
{
    return ns_browser_take_pending_scroll(browser, NULL, out_scroll_y);
}

char *
ns_browser_take_pending_webgl(ns_browser *browser)
{
    (void)browser;
    return ns_webgl_take_pending_origin();
}

int
ns_browser_has_pending_clipboard(ns_browser *browser)
{
    return browser && browser->pending_clipboard ? 1 : 0;
}

char *
ns_browser_take_pending_clipboard(ns_browser *browser)
{
    if (!browser || !browser->pending_clipboard) return NULL;
    char *out = browser->pending_clipboard;
    browser->pending_clipboard = NULL;
    return out;
}

char *
ns_browser_take_pending_download(ns_browser *browser)
{
    if (!browser || !browser->pending_download) return NULL;
    char *out = browser->pending_download;
    browser->pending_download = NULL;
    return out;
}

char *
ns_browser_take_pending_window_action(ns_browser *browser)
{
    if (!browser || !browser->pending_window_action) return NULL;
    char *out = browser->pending_window_action;
    browser->pending_window_action = NULL;
    return out;
}

void
ns_browser_resolve_webgl(ns_browser *browser, const char *origin, int allow)
{
    (void)browser;
    ns_webgl_set_decision(origin, allow);
}

char *
ns_browser_take_pending_camera(ns_browser *browser)
{
    (void)browser;
    return ns_camera_take_pending_origin();
}

void
ns_browser_resolve_camera(ns_browser *browser, const char *origin, int allow)
{
    ns_camera_set_decision(origin, allow);
    if (browser && browser->js) {
        char *r = ns_js_eval_source(browser->js,
            allow ? "__nd_camera_resolve_pending(true)"
                  : "__nd_camera_resolve_pending(false)",
            "camera-resolve");
        free(r);
    }
}

static void
collect_links(const ns_node *node, const char *base, GString *out,
              GHashTable *seen, int depth)
{
    if (!node || depth > 1024) return;
    for (const ns_node *c = node->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "a")) {
            const char *href = ns_element_get_attr(c, "href");
            if (href && *href && href[0] != '#' &&
                !g_str_has_prefix(href, "javascript:")) {
                char *abs = ns_url_resolve(base, href);
                if (abs && *abs && !g_hash_table_contains(seen, abs)) {
                    g_hash_table_add(seen, g_strdup(abs));
                    if (out->len) g_string_append_c(out, '\n');
                    g_string_append(out, abs);
                }
                g_free(abs);
            }
        }
        collect_links(c, base, out, seen, depth + 1);
    }
}

char *
ns_browser_links(ns_browser *browser)
{
    if (!browser || !browser->doc) return NULL;
    GString *out = g_string_new(NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    collect_links(browser->doc, browser->base_url, out, seen, 0);
    g_hash_table_destroy(seen);
    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    char *result = strdup(out->str);
    g_string_free(out, TRUE);
    return result;
}

static gboolean
rel_token_is_icon(const char *rel)
{
    if (!rel)
        return FALSE;
    for (const char *p = rel; *p;) {
        while (*p && g_ascii_isspace(*p))
            p++;
        const char *start = p;
        while (*p && !g_ascii_isspace(*p))
            p++;
        if ((size_t)(p - start) == 4 &&
            g_ascii_strncasecmp(start, "icon", 4) == 0)
            return TRUE;
    }
    return FALSE;
}

static const char *
find_icon_href(const ns_node *node, int depth)
{
    if (!node || depth > 1024) return NULL;
    for (const ns_node *c = node->first_child; c; c = c->next_sibling) {
        if (ns_node_is_element_named(c, "link")) {
            const char *href = ns_element_get_attr(c, "href");
            if (href && *href && rel_token_is_icon(ns_element_get_attr(c, "rel")))
                return href;
        }
        const char *found = find_icon_href(c, depth + 1);
        if (found)
            return found;
    }
    return NULL;
}

char *
ns_browser_favicon_url(ns_browser *browser)
{
    if (!browser || !browser->doc)
        return NULL;
    const char *href = find_icon_href(browser->doc, 0);
    g_autofree char *base = browser_document_base(browser);
    char *abs = (href && *href)
        ? ns_url_resolve(base ? base : browser->base_url, href) : NULL;
    if (abs && *abs) {
        char *out = strdup(abs);
        g_free(abs);
        return out;
    }
    g_free(abs);
    char *origin = ns_url_origin_from(browser->base_url);
    char *out = (origin && *origin) ? g_strconcat(origin, "/favicon.ico", NULL)
                                    : NULL;
    g_free(origin);
    if (!out)
        return NULL;
    char *dup = strdup(out);
    g_free(out);
    return dup;
}

int
ns_browser_bfcache_eligible(ns_browser *browser)
{
    return browser && browser->bfcache_ok;
}

void
ns_browser_bfcache_park(ns_browser *browser)
{
    if (!browser) return;
    if (browser->js)
        ns_js_fire_page_transition(browser->js, "pagehide", TRUE);
}

void
ns_browser_bfcache_restore(ns_browser *browser, int viewport_width,
                           double viewport_height)
{
    if (!browser) return;
    if (viewport_width > 0 && viewport_height > 0.0)
        ns_browser_set_viewport(browser, viewport_width, viewport_height);
    if (browser->js)
        ns_js_fire_page_transition(browser->js, "pageshow", TRUE);
}

int
ns_browser_busy(const ns_browser *browser)
{
    if (!browser) return 0;
    if (ns_engine_in_blocking_fetch()) return 1;
    return browser->js && ns_js_in_pump(browser->js);
}

void
ns_browser_close(ns_browser *browser)
{
    if (!browser) return;
    ns_net_csp_unregister(browser->doc);
    if (browser->media_events_source)
        g_source_remove(browser->media_events_source);
    if (browser->img_sessions) {
        for (guint i = 0; i < browser->img_sessions->len; i++)
            ns_engine_img_session_close(
                g_ptr_array_index(browser->img_sessions, i));
        g_ptr_array_free(browser->img_sessions, TRUE);
    }
    if (browser->img_requested) g_hash_table_destroy(browser->img_requested);
    ns_css_set_active_node(NULL);
    ns_paint_set_anim(NULL);
    if (browser->js) {
        ns_js_set_layout_root(browser->js, NULL);
        ns_js_set_style_table(browser->js, NULL);
    }
    if (browser->anim) ns_anim_free(browser->anim);
    if (browser->layout) { ns_paint_3d_invalidate(); ns_box_free(browser->layout); }
    if (browser->styles) g_hash_table_destroy(browser->styles);
    g_clear_pointer(&browser->retired_styles, g_hash_table_destroy);
    if (browser->css_cache) g_hash_table_destroy(browser->css_cache);
    if (browser->js) ns_js_free(browser->js);
    if (browser->doc) ns_node_free(browser->doc);
    if (browser->videos) ns_video_cache_free(browser->videos);
    if (browser->images) ns_image_cache_free(browser->images);
    g_free(browser->base_url);
    g_free(browser->doc_charset);
    g_free(browser->doc_language);
    g_free(browser->pending_nav);
    g_free(browser->pending_download);
    g_free(browser->pending_clipboard);
    g_free(browser->pending_window_action);
    if (browser->pending_audio) g_string_free(browser->pending_audio, TRUE);
    g_free(browser->refresh_url);
    g_free(browser->pending_post_body);
    g_free(browser->pending_post_ct);
    g_free(browser->search_query);
    g_free(browser->range_drag_start);
    g_free(browser->remote_ip);
    if (browser->console_buf) g_string_free(browser->console_buf, TRUE);
    g_free(browser);
}
