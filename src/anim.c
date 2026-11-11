/* Nordstjernen — CSS transitions and @keyframes animation engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "anim.h"

#include <math.h>
#include <string.h>

#define NS_ANIM_MAX_ACTIVE 256
#define NS_ANIM_KEYFRAME_PROP -1

typedef struct ns_anim_chan {
    int           prop;
    ns_css_value *last;
    ns_css_value *from;
    ns_css_value *to;
    ns_css_value *current;
    gboolean      has_last;
    const void     *starting_seen;
    gboolean      last_currentcolor;
    gboolean      active;
    gboolean      cancelled;
    gboolean      discrete;
    gboolean      pending;
    int           phase;
    gboolean      paused;
    gboolean      finished;
    double        start_us;
    double        duration_ms;
    double        delay_ms;
    double        paused_elapsed_ms;
    ns_css_timing timing;
    guint         generation;
} ns_anim_chan;

typedef struct ns_anim_kf_stop {
    double  pct;
    GArray *decls;
} ns_anim_kf_stop;

#define NS_ANIM_SCRIPT_BASE 1000

typedef struct ns_anim_run {
    int      index;
    gboolean is_script;
    char    *name;
    char    *cancelled_name;
    gboolean active;
    gboolean paused;
    gboolean css_paused;
    gboolean api_override;
    gboolean finished;
    gboolean pending;
    int      phase;
    int      iters_emitted;
    double   elapsed_base_ms;
    ns_css_keyframes *kf;
    GArray  *stops;
    double   start_us;
    double   duration_ms;
    double   delay_ms;
    double   iterations;
    ns_css_anim_direction direction;
    ns_css_anim_fill      fill;
    ns_css_timing timing;
    int      composite;
    guint    generation;
    GHashTable *values;
    GHashTable *partials;
    ns_css_timeline timeline;
    gboolean       duration_auto;
    gboolean       scroll_driven;
    gboolean       tl_root;
    gboolean       tl_inactive;
    const ns_node *tl_source;
    int            tl_axis;
    double         tl_progress;
} ns_anim_run;

enum { NS_ANIM_COMPOSITE_REPLACE, NS_ANIM_COMPOSITE_ADD,
       NS_ANIM_COMPOSITE_ACCUMULATE };

typedef struct ns_anim_partial {
    ns_css_value *from;
    ns_css_value *to;
    double        t;
    int           from_composite;
    int           to_composite;
} ns_anim_partial;

enum { NS_ANIM_PHASE_IDLE, NS_ANIM_PHASE_BEFORE, NS_ANIM_PHASE_ACTIVE,
       NS_ANIM_PHASE_AFTER };

typedef struct ns_anim_state {
    const ns_node *node;
    GPtrArray     *chans;
    GPtrArray     *runs;
    GPtrArray     *scripts;
    GHashTable    *base_values;
    ns_style      *prev_style;
    guint          run_generation;
} ns_anim_state;

typedef struct {
    const ns_node *node;
    const char    *type;
    char          *name;
    double         elapsed_ms;
} ns_anim_event;

typedef struct {
    double pos[2];
    double max[2];
} ns_anim_scroll_pos;

struct ns_anim {
    GHashTable *states;
    GHashTable *active;
    GHashTable *keyframes;
    int         active_count;
    GArray     *events;
    gint64      now_us;
    GHashTable *scroll_sources;
    GHashTable *scroll_known;
    ns_anim_scroll_pos viewport;
    gboolean    viewport_known;
    gboolean    scroll_sync;
    int         scroll_runs;
    int         scroll_root_runs;
    guint       scroll_epoch;
    gboolean    scroll_fresh;
    ns_anim_scroll_lookup_cb scroll_lookup;
    gpointer    scroll_lookup_user;
};

static gint64
anim_now(ns_anim *a)
{
    if (a->now_us == 0) a->now_us = g_get_monotonic_time();
    return a->now_us;
}

static void
anim_emit(ns_anim *a, const ns_node *node, const char *type,
          const char *name, double elapsed_ms)
{
    if (!a || !node) return;
    if (!a->events)
        a->events = g_array_new(FALSE, FALSE, sizeof(ns_anim_event));
    if (elapsed_ms == 0) elapsed_ms = 0.0;
    ns_anim_event e = { node, type, g_strdup(name ? name : ""), elapsed_ms };
    g_array_append_val(a->events, e);
}

void
ns_anim_drain_events(ns_anim *a, ns_anim_event_cb cb, gpointer user)
{
    if (!a || !a->events || a->events->len == 0) return;
    GArray *evs = a->events;
    a->events = NULL;
    for (guint i = 0; i < evs->len; i++) {
        ns_anim_event *e = &g_array_index(evs, ns_anim_event, i);
        if (cb) cb(e->node, e->type, e->name, e->elapsed_ms, user);
        g_free(e->name);
    }
    g_array_free(evs, TRUE);
}

static void
chan_free(gpointer data)
{
    ns_anim_chan *ch = data;
    ns_css_value_free(ch->last);
    ns_css_value_free(ch->from);
    ns_css_value_free(ch->to);
    ns_css_value_free(ch->current);
    g_free(ch);
}

static void
anim_stops_free(GArray *stops)
{
    if (!stops) return;
    for (guint i = 0; i < stops->len; i++)
        ns_css_declarations_free(g_array_index(stops, ns_anim_kf_stop, i).decls);
    g_array_free(stops, TRUE);
}

static void
run_free(gpointer data)
{
    ns_anim_run *r = data;
    if (!r) return;
    ns_css_keyframes_resolved_free(r->kf);
    anim_stops_free(r->stops);
    g_free(r->name);
    g_free(r->cancelled_name);
    if (r->values) g_hash_table_destroy(r->values);
    if (r->partials) g_hash_table_destroy(r->partials);
    g_free(r);
}

static void
ns_anim_state_free(gpointer data)
{
    ns_anim_state *s = data;
    if (!s) return;
    if (s->chans) g_ptr_array_free(s->chans, TRUE);
    if (s->runs) g_ptr_array_free(s->runs, TRUE);
    if (s->scripts) g_ptr_array_free(s->scripts, TRUE);
    if (s->base_values) g_hash_table_destroy(s->base_values);
    ns_style_free(s->prev_style);
    g_free(s);
}

static void
ns_anim_keyframes_free(gpointer data)
{
    ns_css_keyframes *kf = data;
    if (!kf) return;
    g_free(kf->name);
    for (int i = 0; i < kf->n_stops; i++)
        g_free(kf->stops[i].raw_props);
    g_free(kf->stops);
    g_free(kf);
}

static gboolean advance_run(ns_anim *a, ns_anim_run *r, gint64 now_us);
static void partial_free(gpointer data);
static void chan_cancel(ns_anim *a, ns_anim_state *s, ns_anim_chan *ch, gint64 now_us);
static double run_active_ms(const ns_anim_run *r);
static gboolean advance_chan(ns_anim *a, ns_anim_state *s, ns_anim_chan *ch, gint64 now_us);
static void run_emit_progress(ns_anim *a, ns_anim_state *s, ns_anim_run *r, gint64 now_us);
static void run_emit_script(ns_anim *a, ns_anim_state *s, ns_anim_run *r,
                            const char *type);

static GPtrArray *
state_runs(const ns_anim_state *s, int which)
{
    return which == 0 ? s->runs : s->scripts;
}

static gboolean
state_is_active(const ns_anim_state *s)
{
    for (int w = 0; w < 2; w++) {
        GPtrArray *runs = state_runs(s, w);
        if (!runs) continue;
        for (guint i = 0; i < runs->len; i++) {
            const ns_anim_run *r = runs->pdata[i];
            if (r->active && !r->paused) return TRUE;
        }
    }
    if (s->chans)
        for (guint i = 0; i < s->chans->len; i++) {
            const ns_anim_chan *ch = s->chans->pdata[i];
            if (ch->active && !ch->paused) return TRUE;
        }
    return FALSE;
}

static void
anim_track(ns_anim *a, ns_anim_state *s)
{
    if (state_is_active(s)) g_hash_table_add(a->active, s);
    else                    g_hash_table_remove(a->active, s);
}

ns_anim *
ns_anim_new(void)
{
    ns_anim *a = g_new0(ns_anim, 1);
    a->states    = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                         NULL, ns_anim_state_free);
    a->active    = g_hash_table_new(g_direct_hash, g_direct_equal);
    a->keyframes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, ns_anim_keyframes_free);
    a->scroll_sources = g_hash_table_new(g_direct_hash, g_direct_equal);
    a->scroll_known = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, g_free);
    ns_anim_load_from_stylesheet(a, ns_css_ua_stylesheet());
    return a;
}

void
ns_anim_free(ns_anim *a)
{
    if (!a) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val))
        ns_css_incremental_exclude(key, FALSE);
    g_hash_table_destroy(a->active);
    g_hash_table_destroy(a->states);
    g_hash_table_destroy(a->keyframes);
    g_hash_table_destroy(a->scroll_sources);
    g_hash_table_destroy(a->scroll_known);
    if (a->events) {
        for (guint i = 0; i < a->events->len; i++)
            g_free(g_array_index(a->events, ns_anim_event, i).name);
        g_array_free(a->events, TRUE);
    }
    g_free(a);
}

static int
state_active_count(const ns_anim_state *s)
{
    int n = 0;
    for (int w = 0; w < 2; w++) {
        GPtrArray *runs = state_runs(s, w);
        if (!runs) continue;
        for (guint i = 0; i < runs->len; i++)
            if (((ns_anim_run *)runs->pdata[i])->active) n++;
    }
    if (s->chans)
        for (guint i = 0; i < s->chans->len; i++)
            if (((ns_anim_chan *)s->chans->pdata[i])->active) n++;
    return n;
}

void
ns_anim_prune(ns_anim *a, GHashTable *live)
{
    if (!a || !live) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (g_hash_table_contains(live, key)) continue;
        ns_anim_state *s = val;
        a->active_count -= state_active_count(s);
        if (a->active_count < 0) a->active_count = 0;
        g_hash_table_remove(a->active, s);
        ns_css_incremental_exclude(key, FALSE);
        g_hash_table_iter_remove(&it);
    }
}

void
ns_anim_rebase(ns_anim *a, gint64 base_us)
{
    if (!a) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = val;
        for (int w = 0; w < 2; w++) {
            GPtrArray *runs = state_runs(s, w);
            if (!runs) continue;
            for (guint i = 0; i < runs->len; i++)
                ((ns_anim_run *)runs->pdata[i])->start_us = base_us;
        }
        if (s->chans)
            for (guint i = 0; i < s->chans->len; i++)
                ((ns_anim_chan *)s->chans->pdata[i])->start_us = base_us;
    }
}

static void
ns_anim_register_keyframes(ns_anim *a, const ns_css_keyframes *src)
{
    if (!a || !src || !src->name) return;
    ns_css_keyframes *copy = g_new0(ns_css_keyframes, 1);
    copy->name = g_strdup(src->name);
    copy->n_stops = src->n_stops;
    if (src->n_stops > 0) {
        copy->stops = g_new(ns_css_keyframe_stop, src->n_stops);
        memcpy(copy->stops, src->stops,
               src->n_stops * sizeof(ns_css_keyframe_stop));
        for (int i = 0; i < copy->n_stops; i++)
            copy->stops[i].raw_props = g_strdup(src->stops[i].raw_props);
    }
    g_hash_table_replace(a->keyframes, g_strdup(src->name), copy);
}

void
ns_anim_load_from_stylesheet(ns_anim *a, const ns_css_stylesheet *sh)
{
    if (!a || !sh || !sh->keyframes) return;
    for (guint i = 0; i < sh->keyframes->len; i++) {
        const ns_css_keyframes *kf =
            &g_array_index(sh->keyframes, ns_css_keyframes, i);
        ns_anim_register_keyframes(a, kf);
    }
}

static double
steps_apply(int n, ns_css_step_pos pos, double x)
{
    if (n < 1) n = 1;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    int step = (int)floor(x * n + 1e-9);
    if (pos == NS_CSS_STEP_JUMP_START || pos == NS_CSS_STEP_JUMP_BOTH)
        step += 1;
    int jumps = n;
    if (pos == NS_CSS_STEP_JUMP_NONE) jumps = n > 1 ? n - 1 : 1;
    else if (pos == NS_CSS_STEP_JUMP_BOTH) jumps = n + 1;
    if (step < 0) step = 0;
    if (step > jumps) step = jumps;
    return (double)step / jumps;
}

static double
cubic_bezier_axis(double t, double p1, double p2)
{
    double mt = 1.0 - t;
    return 3.0 * mt * mt * t * p1 + 3.0 * mt * t * t * p2 + t * t * t;
}

static double
cubic_bezier_apply(const double cb[4], double x)
{
    double t = x;
    for (int i = 0; i < 8; i++) {
        double xt = cubic_bezier_axis(t, cb[0], cb[2]) - x;
        if (fabs(xt) < 1e-6) break;
        double mt = 1.0 - t;
        double d = 3.0 * mt * mt * cb[0]
                 + 6.0 * mt * t * (cb[2] - cb[0])
                 + 3.0 * t * t * (1.0 - cb[2]);
        if (fabs(d) < 1e-6) break;
        t -= xt / d;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    return cubic_bezier_axis(t, cb[1], cb[3]);
}

static double
timing_apply(ns_css_timing t, double x)
{
    if (t.kind == NS_CSS_TIMING_STEPS)
        return steps_apply(t.steps, t.step_pos, x);
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    switch (t.kind) {
    case NS_CSS_TIMING_LINEAR:      return x;
    case NS_CSS_TIMING_EASE_IN: {
        static const double cb[4] = { 0.42, 0.0, 1.0, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    case NS_CSS_TIMING_EASE_OUT: {
        static const double cb[4] = { 0.0, 0.0, 0.58, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    case NS_CSS_TIMING_EASE_IN_OUT: {
        static const double cb[4] = { 0.42, 0.0, 0.58, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    case NS_CSS_TIMING_CUBIC:       return cubic_bezier_apply(t.cb, x);
    case NS_CSS_TIMING_EASE:
    default: {
        static const double cb[4] = { 0.25, 0.1, 0.25, 1.0 };
        return cubic_bezier_apply(cb, x);
    }
    }
}

static ns_anim_state *
state_for(ns_anim *a, const ns_node *dom)
{
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (s) return s;
    s = g_new0(ns_anim_state, 1);
    s->node = dom;
    g_hash_table_insert(a->states, (gpointer)dom, s);
    return s;
}

static gboolean
should_skip_motion(void)
{
    return ns_css_get_reduced_motion() == NS_CSS_REDUCED_MOTION_REDUCE;
}

static ns_anim_chan *
chan_find(const ns_anim_state *s, int prop)
{
    if (!s->chans) return NULL;
    for (guint i = 0; i < s->chans->len; i++) {
        ns_anim_chan *ch = s->chans->pdata[i];
        if (ch->prop == prop) return ch;
    }
    return NULL;
}

static ns_anim_chan *
chan_ensure(ns_anim_state *s, int prop)
{
    ns_anim_chan *ch = chan_find(s, prop);
    if (ch) return ch;
    if (!s->chans) s->chans = g_ptr_array_new_with_free_func(chan_free);
    ch = g_new0(ns_anim_chan, 1);
    ch->prop = prop;
    g_ptr_array_add(s->chans, ch);
    return ch;
}

static int
entry_prop(const ns_css_anim_entry *e)
{
    switch (e->target) {
    case NS_CSS_ANIM_TARGET_OPACITY:   return NS_CSS_OPACITY;
    case NS_CSS_ANIM_TARGET_TRANSFORM: return NS_CSS_TRANSFORM;
    case NS_CSS_ANIM_TARGET_COLOR:     return NS_CSS_COLOR;
    case NS_CSS_ANIM_TARGET_BG_COLOR:  return NS_CSS_BACKGROUND_COLOR;
    case NS_CSS_ANIM_TARGET_OTHER:
        return e->name ? ns_css_prop_id(e->name) : -1;
    default:
        return -1;
    }
}

static gboolean
prop_animatable(int prop)
{
    switch (prop) {
    case NS_CSS_TRANSITION:
    case NS_CSS_TRANSITION_PROPERTY:
    case NS_CSS_TRANSITION_DURATION:
    case NS_CSS_TRANSITION_DELAY:
    case NS_CSS_TRANSITION_TIMING_FUNCTION:
    case NS_CSS_TRANSITION_BEHAVIOR:
    case NS_CSS_ANIMATION:
    case NS_CSS_ANIMATION_NAME:
    case NS_CSS_ANIMATION_DURATION:
    case NS_CSS_ANIMATION_DELAY:
    case NS_CSS_ANIMATION_TIMING_FUNCTION:
    case NS_CSS_ANIMATION_ITERATION_COUNT:
    case NS_CSS_ANIMATION_DIRECTION:
    case NS_CSS_ANIMATION_FILL_MODE:
    case NS_CSS_ANIMATION_PLAY_STATE:
    case NS_CSS_ANIMATION_COMPOSITION:
    case NS_CSS_ANIMATION_TIMELINE:
    case NS_CSS_ANIMATION_RANGE_START:
    case NS_CSS_ANIMATION_RANGE_END:
    case NS_CSS_CONTAINER_TYPE:
    case NS_CSS_CONTAINER_NAME:
    case NS_CSS_DIRECTION:
    case NS_CSS_UNICODE_BIDI:
        return FALSE;
    default:
        return prop >= 0 && prop < NS_CSS_PROP_COUNT;
    }
}

static gboolean
prop_transitionable(int prop)
{
    if (!prop_animatable(prop)) return FALSE;
    switch (prop) {
    case NS_CSS_TRANSITION:
    case NS_CSS_TRANSITION_PROPERTY:
    case NS_CSS_TRANSITION_DURATION:
    case NS_CSS_TRANSITION_DELAY:
    case NS_CSS_TRANSITION_TIMING_FUNCTION:
    case NS_CSS_ANIMATION:
    case NS_CSS_ANIMATION_NAME:
    case NS_CSS_ANIMATION_DURATION:
    case NS_CSS_ANIMATION_DELAY:
    case NS_CSS_ANIMATION_TIMING_FUNCTION:
    case NS_CSS_ANIMATION_ITERATION_COUNT:
    case NS_CSS_ANIMATION_DIRECTION:
    case NS_CSS_ANIMATION_FILL_MODE:
    case NS_CSS_ANIMATION_PLAY_STATE:
    case NS_CSS_DISPLAY:
    case NS_CSS_CONTENT:
        return FALSE;
    default:
        return prop >= 0 && prop < NS_CSS_PROP_COUNT;
    }
}

static gboolean
prop_discretely_animatable(int prop)
{
    return prop == NS_CSS_VISIBILITY;
}

static double
chan_elapsed_ms(const ns_anim_chan *ch, gint64 now_us)
{
    if (ch->paused) return ch->paused_elapsed_ms;
    if (ch->pending) return -ch->delay_ms;
    return (now_us - ch->start_us) / 1000.0 - ch->delay_ms;
}

static void
chan_start(ns_anim *a, ns_anim_state *s, ns_anim_chan *ch,
           const ns_css_value *from, const ns_css_value *to,
           const ns_css_anim_entry *e, gint64 now_us, gboolean discrete)
{
    double duration_ms = e->duration_ms;
    double delay_ms = e->delay_ms;
    if (ch->active && ch->to && ns_css_value_equal(to, ch->from)) {
        double el = chan_elapsed_ms(ch, now_us);
        double p = ch->duration_ms > 0 ? CLAMP(el / ch->duration_ms, 0.0, 1.0) : 1.0;
        double factor = timing_apply(ch->timing, p);
        if (el < 0) factor = 0;
        duration_ms *= factor;
        if (delay_ms < 0) delay_ms *= factor;
    }
    if (ch->active) chan_cancel(a, s, ch, now_us);
    ns_css_value *new_from = ns_css_value_dup(from);
    ns_css_value *new_to = ns_css_value_dup(to);
    ns_css_value *new_current = ns_css_value_dup(from);
    ns_css_value_free(ch->from);
    ns_css_value_free(ch->to);
    ns_css_value_free(ch->current);
    ch->from = new_from;
    ch->to = new_to;
    ch->current = new_current;
    ch->active = TRUE;
    ch->paused = FALSE;
    ch->finished = FALSE;
    ch->cancelled = FALSE;
    ch->pending = TRUE;
    ch->phase = NS_ANIM_PHASE_IDLE;
    ch->discrete = discrete;
    ch->start_us = now_us;
    ch->duration_ms = duration_ms;
    ch->delay_ms = delay_ms;
    ch->timing = e->timing;
    ch->generation++;
    a->active_count++;
    anim_emit(a, s->node, "transitionrun", ns_css_prop_name(ch->prop), 0.0);
    advance_chan(a, s, ch, now_us);
}

static void
chan_cancel(ns_anim *a, ns_anim_state *s, ns_anim_chan *ch, gint64 now_us)
{
    double el = CLAMP(chan_elapsed_ms(ch, now_us), 0.0, ch->duration_ms);
    anim_emit(a, s->node, "transitioncancel", ns_css_prop_name(ch->prop), el);
    ch->active = FALSE;
    ch->phase = NS_ANIM_PHASE_IDLE;
    if (a->active_count > 0) a->active_count--;
}

static gboolean
ancestor_transitions_to(ns_anim *a, const ns_node *node, int prop,
                        const ns_css_value *cur)
{
    for (const ns_node *p = node ? node->parent : NULL; p; p = p->parent) {
        const ns_anim_state *ps = g_hash_table_lookup(a->states, p);
        if (!ps) continue;
        const ns_anim_chan *pc = chan_find(ps, prop);
        if (pc && pc->active &&
            (ns_css_value_equal(pc->to, cur) || ns_css_value_equal(pc->current, cur)))
            return TRUE;
    }
    return FALSE;
}

static ns_css_value *
initial_value_for(int prop)
{
    const char *name = ns_css_prop_name(prop);
    const char *text = name ? ns_css_initial_value_text(name) : NULL;
    if (!text) return NULL;
    char *decl = g_strdup_printf("%s: %s", name, text);
    GArray *decls = ns_css_parse_declarations(decl);
    g_free(decl);
    ns_css_value *v = NULL;
    if (decls && decls->len > 0) {
        ns_css_decl *d = &g_array_index(decls, ns_css_decl, 0);
        if ((int)d->prop == prop) {
            v = d->value;
            d->value = NULL;
        }
    }
    ns_css_declarations_free(decls);
    return v;
}

static ns_css_value *
currentcolor_used(int prop, ns_css_value *v, const ns_style *st)
{
    if (prop == NS_CSS_COLOR || !st || !st->values[NS_CSS_COLOR] ||
        !ns_css_keyword_is(v, "currentcolor"))
        return v;
    ns_css_value_free(v);
    return ns_css_value_dup(st->values[NS_CSS_COLOR]);
}

static ns_css_value *
value_or_initial_used(int prop, const ns_css_value *v, const ns_style *st)
{
    ns_css_value *out = v ? ns_css_value_dup((ns_css_value *)v) : initial_value_for(prop);
    return currentcolor_used(prop, out, st);
}

static void
observe_transition_prop(ns_anim *a, ns_anim_state *s, const ns_style *style,
                        int prop, const ns_css_anim_entry *e, gint64 now_us)
{
    const ns_css_value *cur = style->values[prop];
    ns_anim_chan *ch = chan_ensure(s, prop);
    if (cur && ch->current && cur == ch->current) return;
    const ns_style *starting = ns_css_style_starting(s->node);
    if (!starting) ch->starting_seen = NULL;
    if (starting && starting != style && ch->starting_seen != starting) {
        ch->starting_seen = starting;
        ns_css_value_free(ch->last);
        ch->last = ns_css_value_dup(starting->values[prop]);
        ch->last_currentcolor = ns_style_prop_from_currentcolor(starting, prop);
        ch->has_last = TRUE;
    }
    if (!ch->has_last && s->prev_style && s->prev_style != style) {
        ch->last = ns_css_value_dup(s->prev_style->values[prop]);
        ch->last_currentcolor = ns_style_prop_from_currentcolor(s->prev_style, prop);
        ch->has_last = TRUE;
    }
    if (!ch->has_last) {
        const ns_style *before = ns_css_style_before_change(s->node);
        if (before && before != style) {
            ch->last = ns_css_value_dup(before->values[prop]);
            ch->last_currentcolor = ns_style_prop_from_currentcolor(before, prop);
            ch->has_last = TRUE;
        }
    }
    if (!ch->has_last) {
        ch->last = ns_css_value_dup(cur);
        ch->last_currentcolor = ns_style_prop_from_currentcolor(style, prop);
        ch->has_last = TRUE;
        return;
    }
    gboolean cur_currentcolor = ns_style_prop_from_currentcolor(style, prop);
    if (ns_css_value_equal(cur, ch->last) ||
        (cur_currentcolor && ch->last_currentcolor)) {
        ch->last_currentcolor = cur_currentcolor;
        if (!ns_css_value_equal(cur, ch->last)) {
            ns_css_value_free(ch->last);
            ch->last = ns_css_value_dup(cur);
        }
        return;
    }
    ns_css_value *cur_eff = value_or_initial_used(prop, cur, style);
    ns_css_value *last_eff = value_or_initial_used(prop, ch->last, style);
    gboolean can_run = e && cur_eff && last_eff &&
                       e->duration_ms + e->delay_ms > 0 &&
                       e->duration_ms >= 0 && !should_skip_motion() &&
                       (ch->active || a->active_count < NS_ANIM_MAX_ACTIVE);
    if (can_run) {
        const ns_css_value *from = ch->active ? ch->current : last_eff;
        ns_css_value *probe = ns_css_value_interpolate_prop(prop, from, cur_eff, 0.0);
        gboolean interpolable = probe != NULL;
        ns_css_value_free(probe);
        if (interpolable && ancestor_transitions_to(a, s->node, prop, cur_eff)) {
            if (ch->active) chan_cancel(a, s, ch, now_us);
        } else if (interpolable) {
            chan_start(a, s, ch, from, cur_eff, e, now_us, FALSE);
        } else if (prop_discretely_animatable(prop) || e->allow_discrete) {
            chan_start(a, s, ch, from, cur_eff, e, now_us, TRUE);
        } else if (ch->active) {
            chan_cancel(a, s, ch, now_us);
        }
    } else if (ch->active) {
        chan_cancel(a, s, ch, now_us);
    }
    ns_css_value_free(cur_eff);
    ns_css_value_free(last_eff);
    ns_css_value_free(ch->last);
    ch->last = ns_css_value_dup(cur);
    ch->last_currentcolor = cur_currentcolor;
}

static void
observe_transition(ns_anim *a, ns_anim_state *s, const ns_style *style,
                   const ns_css_anim_list *tv, gint64 now_us)
{
    const ns_css_anim_entry *by_prop[NS_CSS_PROP_COUNT];
    memset(by_prop, 0, sizeof by_prop);
    gboolean touched[NS_CSS_PROP_COUNT];
    memset(touched, 0, sizeof touched);
    GArray *order = g_array_new(FALSE, FALSE, sizeof(int));
    for (int i = 0; i < tv->n; i++) {
        const ns_css_anim_entry *e = &tv->entries[i];
        if (e->target == NS_CSS_ANIM_TARGET_ALL) {
            for (int p = 0; p < NS_CSS_PROP_COUNT; p++) {
                if (!prop_transitionable(p)) continue;
                by_prop[p] = e;
                if (!touched[p]) { touched[p] = TRUE; g_array_append_val(order, p); }
            }
            continue;
        }
        int p = entry_prop(e);
        if (!prop_transitionable(p)) continue;
        by_prop[p] = e;
        if (!touched[p]) { touched[p] = TRUE; g_array_append_val(order, p); }
    }
    for (guint i = 0; i < order->len; i++) {
        int p = g_array_index(order, int, i);
        observe_transition_prop(a, s, style, p, by_prop[p], now_us);
    }
    if (s->chans) {
        for (guint i = 0; i < s->chans->len; i++) {
            ns_anim_chan *ch = s->chans->pdata[i];
            if (ch->prop < 0 || ch->prop >= NS_CSS_PROP_COUNT || touched[ch->prop])
                continue;
            if (ch->active) {
                chan_cancel(a, s, ch, now_us);
                ch->cancelled = TRUE;
            }
            observe_transition_prop(a, s, style, ch->prop, NULL, now_us);
        }
    }
    g_array_free(order, TRUE);
}

static GArray *
anim_stops_build(const ns_css_keyframes *kf)
{
    GArray *stops = g_array_new(FALSE, TRUE, sizeof(ns_anim_kf_stop));
    if (!kf) return stops;
    for (int i = 0; i < kf->n_stops; i++) {
        ns_anim_kf_stop st = { kf->stops[i].pct,
                               ns_css_parse_declarations(kf->stops[i].raw_props) };
        if (st.decls)
            for (guint d = st.decls->len; d > 0; d--) {
                ns_css_decl *decl = &g_array_index(st.decls, ns_css_decl, d - 1);
                if (!decl->important) continue;
                ns_css_value_free(decl->value);
                g_array_remove_index(st.decls, d - 1);
            }
        g_array_append_val(stops, st);
    }
    return stops;
}

static double
run_elapsed_ms(const ns_anim_run *r, gint64 now_us)
{
    if (r->paused) return r->elapsed_base_ms;
    if (r->pending) return -r->delay_ms;
    return (now_us - r->start_us) / 1000.0 - r->delay_ms;
}

static void
run_set_paused(ns_anim_run *r, gboolean paused, gint64 now_us)
{
    if (paused && !r->paused) {
        r->elapsed_base_ms = run_elapsed_ms(r, now_us);
        r->paused = TRUE;
    } else if (!paused && r->paused) {
        r->start_us = now_us - (r->elapsed_base_ms + r->delay_ms) * 1000.0;
        r->paused = FALSE;
    }
}

static double
run_active_time(const ns_anim_run *r, double el)
{
    double active = run_active_ms(r);
    if (el < 0) return 0;
    if (isfinite(active) && el > active) return active;
    return el;
}

static void
run_cancel(ns_anim *a, ns_anim_state *s, ns_anim_run *r)
{
    double at = run_active_time(r, run_elapsed_ms(r, anim_now(a)));
    if (r->active) {
        r->active = FALSE;
        if (a->active_count > 0) a->active_count--;
    }
    if (r->name) {
        if (r->is_script) run_emit_script(a, s, r, "__nscancel");
        else if (r->phase != NS_ANIM_PHASE_IDLE)
            anim_emit(a, s->node, "animationcancel", r->name, at);
        g_free(r->name);
        r->name = NULL;
    }
    r->finished = FALSE;
    r->pending = FALSE;
    r->phase = NS_ANIM_PHASE_IDLE;
    r->scroll_driven = FALSE;
    r->tl_progress = -1;
    if (r->values) g_hash_table_remove_all(r->values);
    if (r->partials) g_hash_table_remove_all(r->partials);
}

static void
run_configure(ns_anim_run *r, const ns_css_anim_entry *e)
{
    r->duration_ms = e->duration_ms;
    r->delay_ms = e->delay_ms;
    r->iterations = e->iterations;
    r->direction = e->direction;
    r->fill = e->fill;
    r->timing = e->timing;
    r->timeline = e->timeline;
    r->duration_auto = e->duration_auto;
}

static void
run_load_keyframes(ns_anim *a, ns_anim_run *r, const ns_css_anim_entry *e,
                   const ns_style *style)
{
    ns_css_keyframes_resolved_free(r->kf);
    anim_stops_free(r->stops);
    const ns_css_keyframes *gkf = g_hash_table_lookup(a->keyframes, e->name);
    r->kf = gkf ? ns_css_keyframes_resolve(gkf, style->vars) : NULL;
    r->stops = anim_stops_build(r->kf ? r->kf : gkf);
    if (r->values) g_hash_table_remove_all(r->values);
    if (r->partials) g_hash_table_remove_all(r->partials);
}

static void
run_start_progress(ns_anim_state *s, ns_anim_run *r)
{
    r->active = FALSE;
    r->pending = FALSE;
    r->scroll_driven = TRUE;
    r->tl_progress = -1;
    r->generation = ++s->run_generation;
}

static gboolean
entry_progress_based(const ns_css_anim_entry *e)
{
    return e->timeline.kind == NS_CSS_TIMELINE_SCROLL ||
           e->timeline.kind == NS_CSS_TIMELINE_NAMED;
}

static gboolean
run_matches_entry(const ns_anim_run *r, const ns_css_anim_entry *e)
{
    gboolean progress = entry_progress_based(e);
    return r->name && strcmp(r->name, e->name) == 0 &&
           progress == r->scroll_driven;
}

static void
run_start(ns_anim *a, ns_anim_state *s, ns_anim_run *r,
          const ns_css_anim_entry *e, const ns_style *style, gint64 now_us)
{
    gboolean progress = entry_progress_based(e);
    if (!r->active && !progress) {
        if (a->active_count >= NS_ANIM_MAX_ACTIVE) return;
        a->active_count++;
    }
    g_free(r->name);
    r->name = g_strdup(e->name);
    g_free(r->cancelled_name);
    r->cancelled_name = NULL;
    r->start_us = now_us;
    run_configure(r, e);
    r->active = TRUE;
    r->paused = e->paused;
    r->css_paused = e->paused;
    r->api_override = FALSE;
    r->pending = !e->paused;
    r->finished = FALSE;
    r->phase = NS_ANIM_PHASE_IDLE;
    r->iters_emitted = 0;
    r->elapsed_base_ms = -e->delay_ms;
    r->scroll_driven = FALSE;
    r->generation = ++s->run_generation;
    run_load_keyframes(a, r, e, style);
    if (progress) {
        run_start_progress(s, r);
        return;
    }
    advance_run(a, r, now_us);
    run_emit_progress(a, s, r, now_us);
}

static int composite_from_name(const char *name, int fallback);

static int
composition_for(const ns_style *style, int index)
{
    const ns_css_value *v = style ? style->values[NS_CSS_ANIMATION_COMPOSITION] : NULL;
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return NS_ANIM_COMPOSITE_REPLACE;
    char **items = g_strsplit(v->u.keyword, ",", -1);
    int n = (int)g_strv_length(items);
    int c = n > 0 ? composite_from_name(items[index % n], NS_ANIM_COMPOSITE_REPLACE)
                  : NS_ANIM_COMPOSITE_REPLACE;
    g_strfreev(items);
    return c;
}

static void
observe_animation(ns_anim *a, ns_anim_state *s, const ns_style *style,
                  const ns_css_anim_list *av, gint64 now_us)
{
    int n = should_skip_motion() ? 0 : av->n;
    if (!s->runs) {
        if (n == 0) return;
        s->runs = g_ptr_array_new_with_free_func(run_free);
    }
    for (int i = 0; i < n; i++) {
        const ns_css_anim_entry *e = &av->entries[i];
        while ((int)s->runs->len <= i) {
            ns_anim_run *nr = g_new0(ns_anim_run, 1);
            nr->index = (int)s->runs->len;
            g_ptr_array_add(s->runs, nr);
        }
        ns_anim_run *r = s->runs->pdata[i];
        if (!e->name || e->duration_ms < 0 ||
            !g_hash_table_contains(a->keyframes, e->name)) {
            if (r->name) run_cancel(a, s, r);
            g_free(r->cancelled_name);
            r->cancelled_name = NULL;
            continue;
        }
        if (r->cancelled_name) {
            if (strcmp(r->cancelled_name, e->name) == 0) continue;
            g_free(r->cancelled_name);
            r->cancelled_name = NULL;
        }
        r->composite = composition_for(style, i);
        if (run_matches_entry(r, e)) {
            run_configure(r, e);
            if (e->paused != r->css_paused && !r->api_override)
                run_set_paused(r, e->paused, now_us);
            r->css_paused = e->paused;
            continue;
        }
        if (r->name) run_cancel(a, s, r);
        run_start(a, s, r, e, style, now_us);
    }
    for (guint i = n; i < s->runs->len; i++) {
        ns_anim_run *r = s->runs->pdata[i];
        if (r->name) run_cancel(a, s, r);
        g_free(r->cancelled_name);
        r->cancelled_name = NULL;
    }
    while (s->runs->len > 0) {
        ns_anim_run *last = s->runs->pdata[s->runs->len - 1];
        if (last->name || last->cancelled_name) break;
        g_ptr_array_remove_index(s->runs, s->runs->len - 1);
    }
}

static gboolean
ancestor_hidden(GHashTable *styles, const ns_node *dom)
{
    if (!styles) return FALSE;
    for (const ns_node *p = dom->parent; p; p = p->parent) {
        const ns_style *ps = g_hash_table_lookup(styles, p);
        if (ps && ns_display_is_none(ns_css_display_of(ps))) return TRUE;
    }
    return FALSE;
}

static void anim_resolve_timelines(ns_css_anim_list *av, GHashTable *styles,
                                   const ns_node *dom, const ns_style *style);

static void
anim_observe_one(ns_anim *a, const ns_node *dom, const ns_style *style,
                 gint64 now_us, GHashTable *styles)
{
    if (!a || !dom || !style) return;
    if (a->now_us == 0 || g_hash_table_size(a->active) == 0) a->now_us = now_us;
    now_us = a->now_us;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (s && s->prev_style == style && !state_is_active(s)) return;
    ns_css_anim_list tv, av;
    ns_css_anim_effective(style, FALSE, &tv);
    ns_css_anim_effective(style, TRUE, &av);
    anim_resolve_timelines(&av, styles, dom, style);
    if (!s) {
        if (tv.n == 0 && av.n == 0) {
            ns_css_anim_list_clear(&tv);
            ns_css_anim_list_clear(&av);
            return;
        }
        s = state_for(a, dom);
    }
    if (ns_display_is_none(ns_css_display_of(style)) || ancestor_hidden(styles, dom)) {
        ns_css_anim_list_clear(&tv);
        ns_css_anim_list_clear(&av);
        tv.n = 0;
        av.n = 0;
        if (s->chans)
            for (guint i = 0; i < s->chans->len; i++) {
                ns_anim_chan *ch = s->chans->pdata[i];
                if (ch->active) chan_cancel(a, s, ch, now_us);
                ch->finished = FALSE;
                ch->cancelled = TRUE;
            }
    }
    observe_transition(a, s, style, &tv, now_us);
    observe_animation(a, s, style, &av, now_us);
    anim_track(a, s);
    ns_css_anim_list_clear(&tv);
    ns_css_anim_list_clear(&av);
    if (s->prev_style != style) {
        ns_style_free(s->prev_style);
        s->prev_style = (ns_style *)style;
        s->prev_style->ref++;
    }
}

static int
node_depth(const ns_node *n)
{
    int d = 0;
    for (const ns_node *p = n ? n->parent : NULL; p; p = p->parent) d++;
    return d;
}

typedef struct {
    const ns_node  *node;
    const ns_style *style;
    int             depth;
} ns_anim_observe_item;

static int
observe_item_cmp(gconstpointer x, gconstpointer y)
{
    const ns_anim_observe_item *p = x, *q = y;
    return p->depth - q->depth;
}

static void anim_scroll_bind(ns_anim *a, GHashTable *styles);

void
ns_anim_observe_all(ns_anim *a, GHashTable *styles, gint64 now_us)
{
    if (!a || !styles) return;
    if (a->now_us == 0 || g_hash_table_size(a->active) == 0) a->now_us = now_us;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(ns_anim_observe_item));
    GHashTableIter it;
    gpointer key, val;
    gboolean have_states = g_hash_table_size(a->states) > 0;
    g_hash_table_iter_init(&it, styles);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const ns_anim_state *s = have_states
            ? g_hash_table_lookup(a->states, key) : NULL;
        if (s ? s->prev_style == val && !state_is_active(s)
              : !ns_css_style_may_animate(val))
            continue;
        ns_anim_observe_item item = { key, val, node_depth(key) };
        g_array_append_val(items, item);
    }
    g_array_sort(items, observe_item_cmp);
    for (guint i = 0; i < items->len; i++) {
        const ns_anim_observe_item *item = &g_array_index(items, ns_anim_observe_item, i);
        anim_observe_one(a, item->node, item->style, now_us, styles);
    }
    g_array_free(items, TRUE);
    ns_anim_prune(a, styles);
    anim_scroll_bind(a, styles);
    ns_anim_apply(a, styles);
}

static void
style_set_value(ns_style *st, int prop, ns_css_value *v)
{
    if (!st || prop < 0 || prop >= NS_CSS_PROP_COUNT || !v) return;
    if (st->values[prop] == v) return;
    ns_css_value_free(st->values[prop]);
    st->values[prop] = ns_css_value_dup(v);
}

static void
apply_propagate(GHashTable *styles, const ns_node *node, int prop,
                const ns_css_value *base, ns_css_value *current)
{
    const ns_node *c = node->first_child;
    while (c) {
        ns_style *st = g_hash_table_lookup(styles, c);
        gboolean inherits = st && st->values[prop] == base;
        if (inherits) style_set_value(st, prop, current);
        c = ns_node_next_in_subtree(c, node, inherits);
    }
}

static gboolean
apply_animated_value(GHashTable *styles, ns_anim_state *s, ns_style *st,
                     int prop, ns_css_value *current)
{
    if (prop < 0 || prop >= NS_CSS_PROP_COUNT || !current) return FALSE;
    ns_css_value *base = st->values[prop];
    if (base == current) return FALSE;
    if (!s->base_values)
        s->base_values = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                               (GDestroyNotify)ns_css_value_free);
    if (!g_hash_table_contains(s->base_values, GINT_TO_POINTER(prop)))
        g_hash_table_insert(s->base_values, GINT_TO_POINTER(prop),
                            base ? ns_css_value_dup(base) : g_new0(ns_css_value, 1));
    ns_css_value *resolved = ns_css_value_dup(current);
    ns_css_value_absolutize(resolved, prop, st);
    st->values[prop] = resolved;
    if (base && ns_css_prop_inherits(prop))
        apply_propagate(styles, s->node, prop, base, resolved);
    ns_css_value_free(base);
    return TRUE;
}

const ns_css_value *
ns_anim_base_value(ns_anim *a, const ns_node *node, int prop)
{
    if (!a || !node) return NULL;
    ns_anim_state *s = g_hash_table_lookup(a->states, node);
    if (!s || !s->base_values) return NULL;
    const ns_css_value *v = g_hash_table_lookup(s->base_values, GINT_TO_POINTER(prop));
    return v && v->kind == 0 && !v->u.keyword ? NULL : v;
}

static gboolean value_is_wide_keyword(const ns_css_value *v);

static ns_css_value *
resolve_wide_keyword(const ns_css_value *v, int prop, GHashTable *styles,
                     const ns_node *node, const ns_css_value *under)
{
    if (!value_is_wide_keyword(v)) return ns_css_value_dup((ns_css_value *)v);
    const char *k = v->u.keyword;
    gboolean unset = strcmp(k, "unset") == 0;
    if (strcmp(k, "inherit") == 0 || (unset && ns_css_prop_inherits(prop))) {
        const ns_style *ps = node && node->parent
            ? g_hash_table_lookup(styles, node->parent) : NULL;
        if (ps && ps->values[prop]) return ns_css_value_dup(ps->values[prop]);
    }
    if (strcmp(k, "inherit") == 0 || strcmp(k, "initial") == 0 || unset) {
        ns_css_value *init = initial_value_for(prop);
        return init ? init : ns_css_value_dup((ns_css_value *)v);
    }
    return under ? ns_css_value_dup((ns_css_value *)under) : NULL;
}

static ns_css_value *
resolve_stop_value(const ns_css_value *v, int prop, GHashTable *styles,
                   const ns_node *node, const ns_css_value *under, const ns_style *st)
{
    return currentcolor_used(prop, resolve_wide_keyword(v, prop, styles, node, under), st);
}

static gboolean
value_needs_element(const ns_css_value *v)
{
    return value_is_wide_keyword(v) || ns_css_keyword_is(v, "currentcolor");
}

static ns_css_value *
composite_onto(int prop, const ns_css_value *under, const ns_css_value *v, int composite)
{
    if (!v) return NULL;
    if (composite == NS_ANIM_COMPOSITE_REPLACE || !under) return ns_css_value_dup(v);
    ns_css_value *out = ns_css_value_composite(prop, under, v,
                                               composite == NS_ANIM_COMPOSITE_ACCUMULATE);
    return out ? out : ns_css_value_dup(v);
}

void
ns_anim_apply(ns_anim *a, GHashTable *styles)
{
    if (!a || !styles) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = val;
        ns_style *st = g_hash_table_lookup(styles, key);
        if (!st) continue;
        gboolean mutated = FALSE;
        if (s->base_values) g_hash_table_remove_all(s->base_values);
        if (s->chans)
            for (guint i = 0; i < s->chans->len; i++) {
                ns_anim_chan *ch = s->chans->pdata[i];
                if (ch->active && ch->current)
                    mutated |= apply_animated_value(styles, s, st, ch->prop,
                                                    ch->current);
            }
        for (int w = 0; w < 2; w++) {
            GPtrArray *runs = state_runs(s, w);
            if (!runs) continue;
            for (guint i = 0; i < runs->len; i++) {
                ns_anim_run *r = runs->pdata[i];
                if (r->partials && g_hash_table_size(r->partials) > 0) {
                    GHashTableIter pit;
                    gpointer pk, pv;
                    g_hash_table_iter_init(&pit, r->partials);
                    while (g_hash_table_iter_next(&pit, &pk, &pv)) {
                        int prop = GPOINTER_TO_INT(pk);
                        ns_anim_partial *pa = pv;
                        const ns_css_value *base = s->base_values
                            ? g_hash_table_lookup(s->base_values, pk) : NULL;
                        if (base && base->kind == 0 && !base->u.keyword) base = NULL;
                        if (!base) base = st->values[prop];
                        ns_css_value *init = value_or_initial_used(prop, base, st);
                        const ns_css_value *under = init;
                        ns_css_value *rfrom = pa->from
                            ? resolve_stop_value(pa->from, prop, styles, s->node, under, st) : NULL;
                        ns_css_value *rto = pa->to
                            ? resolve_stop_value(pa->to, prop, styles, s->node, under, st) : NULL;
                        ns_css_value *cfrom = composite_onto(prop, under, rfrom,
                                                             pa->from_composite);
                        ns_css_value *cto = composite_onto(prop, under, rto, pa->to_composite);
                        ns_css_value_free(rfrom);
                        ns_css_value_free(rto);
                        const ns_css_value *from = cfrom ? cfrom : under;
                        const ns_css_value *to = cto ? cto : under;
                        ns_css_value *out = from && to
                            ? ns_css_value_interpolate_prop(prop, from, to, pa->t) : NULL;
                        ns_css_value_clamp_animated(prop, out);
                        if (!out && from && to)
                            out = ns_css_value_dup(pa->t < 0.5 ? from : to);
                        ns_css_value_free(cfrom);
                        ns_css_value_free(cto);
                        ns_css_value_free(init);
                        if (out) g_hash_table_insert(r->values, pk, out);
                    }
                }
                if (!r->values) continue;
                GHashTableIter vit;
                gpointer vk, vv;
                g_hash_table_iter_init(&vit, r->values);
                while (g_hash_table_iter_next(&vit, &vk, &vv))
                    mutated |= apply_animated_value(styles, s, st,
                                                    GPOINTER_TO_INT(vk), vv);
            }
        }
        ns_css_incremental_exclude(key, mutated);
    }
}

static int
chan_phase_at(const ns_anim_chan *ch, double elapsed)
{
    if (elapsed < 0) return NS_ANIM_PHASE_BEFORE;
    if (elapsed >= ch->duration_ms) return NS_ANIM_PHASE_AFTER;
    return NS_ANIM_PHASE_ACTIVE;
}

static void
chan_emit_phase(ns_anim *a, ns_anim_state *s, ns_anim_chan *ch, int cur)
{
    int old = ch->phase;
    if (old == cur) return;
    const char *name = ns_css_prop_name(ch->prop);
    double start_el = MAX(MIN(-ch->delay_ms, ch->duration_ms), 0.0);
    if (cur == NS_ANIM_PHASE_ACTIVE) {
        anim_emit(a, s->node, "transitionstart", name,
                  old == NS_ANIM_PHASE_AFTER ? ch->duration_ms : start_el);
    } else if (cur == NS_ANIM_PHASE_AFTER) {
        if (old != NS_ANIM_PHASE_ACTIVE)
            anim_emit(a, s->node, "transitionstart", name, start_el);
        anim_emit(a, s->node, "transitionend", name, ch->duration_ms);
    } else if (cur == NS_ANIM_PHASE_BEFORE) {
        if (old == NS_ANIM_PHASE_AFTER)
            anim_emit(a, s->node, "transitionstart", name, ch->duration_ms);
        if (old != NS_ANIM_PHASE_IDLE)
            anim_emit(a, s->node, "transitionend", name, 0.0);
    }
    ch->phase = cur;
}

static gboolean
advance_chan(ns_anim *a, ns_anim_state *s, ns_anim_chan *ch, gint64 now_us)
{
    double elapsed = chan_elapsed_ms(ch, now_us);
    int cur = chan_phase_at(ch, elapsed);
    chan_emit_phase(a, s, ch, cur);
    if (cur == NS_ANIM_PHASE_BEFORE) {
        if (ch->current == ch->from) return FALSE;
        gboolean moved = !ns_css_value_equal(ch->current, ch->from);
        ns_css_value_free(ch->current);
        ch->current = ns_css_value_dup(ch->from);
        return moved;
    }
    if (cur == NS_ANIM_PHASE_AFTER) {
        ns_css_value_free(ch->current);
        ch->current = ns_css_value_dup(ch->to);
        if (ch->active) {
            ch->active = FALSE;
            ch->finished = TRUE;
            if (a->active_count > 0) a->active_count--;
        }
        return TRUE;
    }
    double t = ch->duration_ms > 0 ? elapsed / ch->duration_ms : 1.0;
    double eased = timing_apply(ch->timing, t);
    ns_css_value *next = ch->discrete
        ? ns_css_value_dup(eased < 0.5 ? ch->from : ch->to)
        : ns_css_value_interpolate_prop(ch->prop, ch->from, ch->to, eased);
    if (!next) next = ns_css_value_dup(eased < 0.5 ? ch->from : ch->to);
    gboolean moved = !ch->current || !ns_css_value_equal(ch->current, next);
    ns_css_value_free(ch->current);
    ch->current = next;
    return moved;
}

static const ns_css_value *
stop_value(const ns_anim_kf_stop *st, int prop)
{
    if (!st->decls) return NULL;
    const ns_css_value *found = NULL;
    for (guint i = 0; i < st->decls->len; i++) {
        const ns_css_decl *d = &g_array_index(st->decls, ns_css_decl, i);
        if ((int)d->prop == prop) found = d->value;
    }
    return found;
}

static int
composite_from_name(const char *name, int fallback)
{
    if (!name) return fallback;
    while (*name == ' ') name++;
    if (g_ascii_strncasecmp(name, "add", 3) == 0) return NS_ANIM_COMPOSITE_ADD;
    if (g_ascii_strncasecmp(name, "accumulate", 10) == 0)
        return NS_ANIM_COMPOSITE_ACCUMULATE;
    if (g_ascii_strncasecmp(name, "replace", 7) == 0)
        return NS_ANIM_COMPOSITE_REPLACE;
    return fallback;
}

static gboolean
value_is_wide_keyword(const ns_css_value *v)
{
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    const char *k = v->u.keyword;
    return strcmp(k, "initial") == 0 || strcmp(k, "inherit") == 0 ||
           strcmp(k, "unset") == 0 || strcmp(k, "revert") == 0 ||
           strcmp(k, "revert-layer") == 0;
}

static int
stop_composite(const ns_anim_kf_stop *st, int fallback)
{
    const ns_css_value *v = stop_value(st, NS_CSS_ANIMATION_COMPOSITION);
    if (!v || v->kind != NS_CSS_V_KEYWORD) return fallback;
    return composite_from_name(v->u.keyword, fallback);
}

static gboolean
stop_timing(const ns_anim_kf_stop *st, ns_css_timing *out)
{
    const ns_css_value *v = stop_value(st, NS_CSS_ANIMATION_TIMING_FUNCTION);
    if (!v || v->kind != NS_CSS_V_KEYWORD || !v->u.keyword) return FALSE;
    return ns_css_timing_parse(v->u.keyword, out);
}

static void
partial_free(gpointer data)
{
    ns_anim_partial *p = data;
    ns_css_value_free(p->from);
    ns_css_value_free(p->to);
    g_free(p);
}

static void
run_sample_at(ns_anim_run *r, double progress)
{
    double pct = (r->is_script ? timing_apply(r->timing, progress) : progress) * 100.0;
    if (!r->values)
        r->values = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, (GDestroyNotify)ns_css_value_free);
    if (!r->partials)
        r->partials = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, partial_free);
    g_hash_table_remove_all(r->values);
    g_hash_table_remove_all(r->partials);
    if (!r->stops) return;
    gboolean seen[NS_CSS_PROP_COUNT];
    memset(seen, 0, sizeof seen);
    for (guint i = 0; i < r->stops->len; i++) {
        const ns_anim_kf_stop *st = &g_array_index(r->stops, ns_anim_kf_stop, i);
        if (!st->decls) continue;
        for (guint d = 0; d < st->decls->len; d++) {
            int prop = (int)g_array_index(st->decls, ns_css_decl, d).prop;
            if (prop < 0 || prop >= NS_CSS_PROP_COUNT || seen[prop]) continue;
            seen[prop] = TRUE;
            if (!prop_animatable(prop)) continue;
            const ns_anim_kf_stop *prev = NULL, *next = NULL;
            const ns_anim_kf_stop *first = NULL, *second = NULL;
            const ns_anim_kf_stop *penult = NULL, *last = NULL;
            for (guint k = 0; k < r->stops->len; k++) {
                const ns_anim_kf_stop *c = &g_array_index(r->stops, ns_anim_kf_stop, k);
                if (!stop_value(c, prop)) continue;
                if (c->pct <= pct) prev = c;
                if (c->pct > pct && !next) next = c;
                if (!first) first = c;
                else if (!second) second = c;
                penult = last;
                last = c;
            }
            if (r->is_script && pct < 0 && first && second &&
                first->pct <= 1e-9) {
                prev = first;
                next = second;
            } else if (r->is_script && pct > 100.0 && last && penult &&
                       last->pct >= 100.0 - 1e-9) {
                prev = penult;
                next = last;
            }
            ns_css_value *out = NULL;
            if (prev && next && prev != next) {
                double range = next->pct - prev->pct;
                double t = range > 0 ? (pct - prev->pct) / range : 0;
                if (!r->is_script) {
                    ns_css_timing tm;
                    if (!stop_timing(prev, &tm)) tm = r->timing;
                    t = timing_apply(tm, t);
                }
                int from_comp = stop_composite(prev, r->composite);
                int to_comp = stop_composite(next, r->composite);
                if (from_comp != NS_ANIM_COMPOSITE_REPLACE ||
                    to_comp != NS_ANIM_COMPOSITE_REPLACE ||
                    value_needs_element(stop_value(prev, prop)) ||
                    value_needs_element(stop_value(next, prop))) {
                    ns_anim_partial *pa = g_new0(ns_anim_partial, 1);
                    pa->from = ns_css_value_dup(stop_value(prev, prop));
                    pa->to = ns_css_value_dup(stop_value(next, prop));
                    pa->t = t;
                    pa->from_composite = from_comp;
                    pa->to_composite = to_comp;
                    g_hash_table_insert(r->partials, GINT_TO_POINTER(prop), pa);
                    continue;
                }
                out = ns_css_value_interpolate_prop(prop, stop_value(prev, prop),
                                                    stop_value(next, prop), t);
                ns_css_value_clamp_animated(prop, out);
                if (!out)
                    out = ns_css_value_dup(t < 0.5 ? stop_value(prev, prop)
                                                   : stop_value(next, prop));
            } else if (prev && prev->pct >= 100.0 - 1e-9 &&
                       (stop_composite(prev, r->composite) != NS_ANIM_COMPOSITE_REPLACE ||
                        value_needs_element(stop_value(prev, prop)))) {
                ns_anim_partial *pa = g_new0(ns_anim_partial, 1);
                pa->from = ns_css_value_dup(stop_value(prev, prop));
                pa->to = ns_css_value_dup(stop_value(prev, prop));
                pa->from_composite = pa->to_composite =
                    stop_composite(prev, r->composite);
                g_hash_table_insert(r->partials, GINT_TO_POINTER(prop), pa);
            } else if (next && next->pct <= 1e-9 && !prev &&
                       (stop_composite(next, r->composite) != NS_ANIM_COMPOSITE_REPLACE ||
                        value_needs_element(stop_value(next, prop)))) {
                ns_anim_partial *pa = g_new0(ns_anim_partial, 1);
                pa->from = ns_css_value_dup(stop_value(next, prop));
                pa->to = ns_css_value_dup(stop_value(next, prop));
                pa->from_composite = pa->to_composite =
                    stop_composite(next, r->composite);
                g_hash_table_insert(r->partials, GINT_TO_POINTER(prop), pa);
            } else if (prev && prev->pct >= 100.0 - 1e-9) {
                out = ns_css_value_dup(stop_value(prev, prop));
            } else if (next && next->pct <= 1e-9) {
                out = ns_css_value_dup(stop_value(next, prop));
            } else if (prev || next) {
                double lo = prev ? prev->pct : 0.0;
                double hi = next ? next->pct : 100.0;
                double t = hi > lo ? (pct - lo) / (hi - lo) : 0;
                if (!r->is_script && prev) {
                    ns_css_timing tm;
                    if (!stop_timing(prev, &tm)) tm = r->timing;
                    t = timing_apply(tm, t);
                } else if (!r->is_script) {
                    t = timing_apply(r->timing, t);
                }
                ns_anim_partial *pa = g_new0(ns_anim_partial, 1);
                pa->from = prev ? ns_css_value_dup(stop_value(prev, prop)) : NULL;
                pa->to = next ? ns_css_value_dup(stop_value(next, prop)) : NULL;
                pa->t = t;
                pa->from_composite = prev ? stop_composite(prev, r->composite)
                                          : NS_ANIM_COMPOSITE_REPLACE;
                pa->to_composite = next ? stop_composite(next, r->composite)
                                        : NS_ANIM_COMPOSITE_REPLACE;
                g_hash_table_insert(r->partials, GINT_TO_POINTER(prop), pa);
            }
            if (out) g_hash_table_insert(r->values, GINT_TO_POINTER(prop), out);
        }
    }
}

static double
directed_progress(int iter, double raw, ns_css_anim_direction dir)
{
    gboolean rev;
    switch (dir) {
    case NS_CSS_ANIM_DIR_REVERSE:           rev = TRUE; break;
    case NS_CSS_ANIM_DIR_ALTERNATE:         rev = (iter & 1); break;
    case NS_CSS_ANIM_DIR_ALTERNATE_REVERSE: rev = !(iter & 1); break;
    case NS_CSS_ANIM_DIR_NORMAL:
    default:                                rev = FALSE; break;
    }
    return rev ? 1.0 - raw : raw;
}

static int
run_last_iteration(const ns_anim_run *r)
{
    if (!isfinite(r->iterations)) return 0;
    int last = (int)ceil(r->iterations) - 1;
    return last < 0 ? 0 : last;
}

static double
run_end_progress(const ns_anim_run *r)
{
    int last = run_last_iteration(r);
    double raw_end = isfinite(r->iterations) ? r->iterations - last : 1.0;
    return directed_progress(last, CLAMP(raw_end, 0.0, 1.0), r->direction);
}

static double
run_active_ms(const ns_anim_run *r)
{
    if (!isfinite(r->iterations)) return INFINITY;
    return r->duration_ms * r->iterations;
}

static gboolean
values_tables_equal(GHashTable *a, GHashTable *b)
{
    guint na = a ? g_hash_table_size(a) : 0;
    guint nb = b ? g_hash_table_size(b) : 0;
    if (na != nb) return FALSE;
    if (na == 0) return TRUE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const ns_css_value *other = g_hash_table_lookup(b, key);
        if (!other || !ns_css_value_equal(val, other)) return FALSE;
    }
    return TRUE;
}

static gboolean
partials_tables_equal(GHashTable *a, GHashTable *b)
{
    guint na = a ? g_hash_table_size(a) : 0;
    guint nb = b ? g_hash_table_size(b) : 0;
    if (na != nb) return FALSE;
    if (na == 0) return TRUE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const ns_anim_partial *p = val;
        const ns_anim_partial *q = g_hash_table_lookup(b, key);
        if (!q || p->t != q->t ||
            !ns_css_value_equal(p->from, q->from) ||
            !ns_css_value_equal(p->to, q->to))
            return FALSE;
    }
    return TRUE;
}

static gboolean
run_resample(ns_anim_run *r, double progress)
{
    GHashTable *old_values = r->values;
    GHashTable *old_partials = r->partials;
    r->values = NULL;
    r->partials = NULL;
    run_sample_at(r, progress);
    gboolean moved = !values_tables_equal(old_values, r->values) ||
                     !partials_tables_equal(old_partials, r->partials);
    if (old_values) g_hash_table_destroy(old_values);
    if (old_partials) g_hash_table_destroy(old_partials);
    return moved;
}

static gboolean
advance_run(ns_anim *a, ns_anim_run *r, gint64 now_us)
{
    if (!r->name) return FALSE;
    double elapsed = run_elapsed_ms(r, now_us);
    if (elapsed < 0) {
        gboolean fill_back = r->fill == NS_CSS_ANIM_FILL_BACKWARDS ||
                             r->fill == NS_CSS_ANIM_FILL_BOTH;
        if (!fill_back) {
            if (r->values) g_hash_table_remove_all(r->values);
            if (r->partials) g_hash_table_remove_all(r->partials);
            return FALSE;
        }
        return run_resample(r, directed_progress(0, 0.0, r->direction));
    }
    double active = run_active_ms(r);
    if (r->duration_ms <= 0 || (isfinite(active) && elapsed >= active)) {
        if (r->active) {
            r->active = FALSE;
            r->finished = TRUE;
            if (a->active_count > 0) a->active_count--;
        }
        gboolean fill_fwd = r->fill == NS_CSS_ANIM_FILL_FORWARDS ||
                            r->fill == NS_CSS_ANIM_FILL_BOTH;
        if (!fill_fwd) {
            if (r->values) g_hash_table_remove_all(r->values);
            if (r->partials) g_hash_table_remove_all(r->partials);
            return TRUE;
        }
        run_sample_at(r, run_end_progress(r));
        return TRUE;
    }
    double iter_d = elapsed / r->duration_ms;
    if (iter_d > 1e9) iter_d = 1e9;
    int iter = (int)iter_d;
    double raw = fmod(elapsed, r->duration_ms) / r->duration_ms;
    return run_resample(r, directed_progress(iter, raw, r->direction));
}

static void
run_emit_script(ns_anim *a, ns_anim_state *s, ns_anim_run *r, const char *type)
{
    char idx[16];
    g_snprintf(idx, sizeof idx, "%d", r->index);
    anim_emit(a, s->node, type, idx, 0.0);
}

static int
run_phase_at(const ns_anim_run *r, double el)
{
    double active = run_active_ms(r);
    if (el < 0) return NS_ANIM_PHASE_BEFORE;
    if (isfinite(active) && el >= active) return NS_ANIM_PHASE_AFTER;
    return NS_ANIM_PHASE_ACTIVE;
}

static void
run_emit_progress(ns_anim *a, ns_anim_state *s, ns_anim_run *r, gint64 now_us)
{
    if (r->is_script) {
        if (!r->active) run_emit_script(a, s, r, "__nsfinish");
        return;
    }
    if (!r->name) return;
    double el = run_elapsed_ms(r, now_us);
    int old = r->phase;
    int cur = run_phase_at(r, el);
    double active = run_active_ms(r);
    double start_el = MAX(MIN(-r->delay_ms, isfinite(active) ? active : -r->delay_ms), 0.0);
    double end_el = isfinite(active) ? active : el;
    if (old != cur) {
        if (cur == NS_ANIM_PHASE_ACTIVE) {
            anim_emit(a, s->node, "animationstart", r->name,
                      old == NS_ANIM_PHASE_AFTER ? end_el : start_el);
            r->iters_emitted = (int)MIN(el / MAX(r->duration_ms, 1e-9), 1e9);
            if (old == NS_ANIM_PHASE_AFTER) r->iters_emitted = run_last_iteration(r);
        } else if (cur == NS_ANIM_PHASE_AFTER) {
            if (old != NS_ANIM_PHASE_ACTIVE)
                anim_emit(a, s->node, "animationstart", r->name, start_el);
            anim_emit(a, s->node, "animationend", r->name, end_el);
        } else if (cur == NS_ANIM_PHASE_BEFORE) {
            if (old == NS_ANIM_PHASE_AFTER)
                anim_emit(a, s->node, "animationstart", r->name, end_el);
            if (old != NS_ANIM_PHASE_IDLE)
                anim_emit(a, s->node, "animationend", r->name, 0.0);
        }
        r->phase = cur;
        return;
    }
    if (cur == NS_ANIM_PHASE_ACTIVE && r->duration_ms > 0) {
        int reached = (int)MIN(el / r->duration_ms, 1e9);
        int last = run_last_iteration(r);
        if (isfinite(r->iterations) && reached > last) reached = last;
        if (r->iters_emitted < reached) {
            r->iters_emitted = reached;
            anim_emit(a, s->node, "animationiteration", r->name,
                      reached * r->duration_ms);
        }
    }
}

gboolean
ns_anim_tick(ns_anim *a, gint64 now_us)
{
    if (!a) return FALSE;
    a->now_us = now_us;
    if (g_hash_table_size(a->active) == 0) return FALSE;
    gboolean any = FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->active);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = key;
        if (s->chans)
            for (guint i = 0; i < s->chans->len; i++) {
                ns_anim_chan *ch = s->chans->pdata[i];
                if (!ch->active || ch->paused) continue;
                if (ch->pending) {
                    ch->pending = FALSE;
                    ch->start_us = now_us;
                }
                if (advance_chan(a, s, ch, now_us)) any = TRUE;
            }
        for (int w = 0; w < 2; w++) {
            GPtrArray *runs = state_runs(s, w);
            if (!runs) continue;
            for (guint i = 0; i < runs->len; i++) {
                ns_anim_run *r = runs->pdata[i];
                if (!r->active) continue;
                if (r->pending) {
                    r->pending = FALSE;
                    r->start_us = now_us;
                }
                if (advance_run(a, r, now_us)) any = TRUE;
                run_emit_progress(a, s, r, now_us);
            }
        }
        if (!state_is_active(s)) g_hash_table_iter_remove(&it);
    }
    return any;
}

static gboolean
anim_has_time_runs(const ns_anim *a)
{
    return a && a->active && g_hash_table_size(a->active) > 0;
}

gboolean
ns_anim_has_active(const ns_anim *a)
{
    return anim_has_time_runs(a) || (a && a->scroll_sync);
}

static gboolean
anim_prop_needs_relayout(int prop)
{
    return prop != NS_CSS_OPACITY && prop != NS_CSS_TRANSFORM &&
           prop != NS_CSS_COLOR && prop != NS_CSS_BACKGROUND_COLOR;
}

static gboolean
anim_prop_paint_only(int prop)
{
    return prop == NS_CSS_OPACITY || prop == NS_CSS_COLOR ||
           prop == NS_CSS_BACKGROUND_COLOR;
}

static gboolean
anim_prop_moves_boxes(int prop)
{
    return !anim_prop_paint_only(prop);
}

static gboolean
anim_any_active_prop(const ns_anim *a, gboolean (*match)(int prop))
{
    if (!a || !a->active) return FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->active);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        const ns_anim_state *s = key;
        if (s->chans)
            for (guint i = 0; i < s->chans->len; i++) {
                const ns_anim_chan *ch = s->chans->pdata[i];
                if (ch->active && match(ch->prop))
                    return TRUE;
            }
        for (int w = 0; w < 2; w++) {
            GPtrArray *runs = state_runs(s, w);
            if (!runs) continue;
            for (guint i = 0; i < runs->len; i++) {
                const ns_anim_run *r = runs->pdata[i];
                GHashTableIter vit;
                gpointer vk, vv;
                if (r->partials) {
                    g_hash_table_iter_init(&vit, r->partials);
                    while (g_hash_table_iter_next(&vit, &vk, &vv))
                        if (match(GPOINTER_TO_INT(vk))) return TRUE;
                }
                if (!r->values) continue;
                g_hash_table_iter_init(&vit, r->values);
                while (g_hash_table_iter_next(&vit, &vk, &vv))
                    if (match(GPOINTER_TO_INT(vk))) return TRUE;
            }
        }
    }
    return FALSE;
}

gboolean
ns_anim_needs_layout(const ns_anim *a)
{
    return anim_any_active_prop(a, anim_prop_needs_relayout);
}

gboolean
ns_anim_paint_only(const ns_anim *a)
{
    return anim_has_time_runs(a) &&
           !anim_any_active_prop(a, anim_prop_moves_boxes);
}

static gboolean
overflow_kw_container(const char *kw)
{
    return kw && g_ascii_strcasecmp(kw, "visible") != 0 &&
           g_ascii_strcasecmp(kw, "clip") != 0;
}

static gboolean
display_can_scroll(const ns_style *st)
{
    ns_display d = ns_css_display_of(st);
    gboolean plain_inline = d.internal == NS_DISPLAY_INTERNAL_NONE &&
                            d.outer == NS_DISPLAY_OUTER_INLINE &&
                            d.inner == NS_DISPLAY_INNER_FLOW;
    return d.box == NS_DISPLAY_BOX_NORMAL && !plain_inline;
}

static gboolean
style_is_scroll_container(const ns_style *st)
{
    return st && display_can_scroll(st) &&
           (overflow_kw_container(ns_style_overflow_keyword(st, NS_CSS_OVERFLOW_X)) ||
            overflow_kw_container(ns_style_overflow_keyword(st, NS_CSS_OVERFLOW_Y)));
}

static gboolean
node_is_root_element(const ns_node *n)
{
    return !n->parent || n->parent->kind != NS_NODE_ELEMENT;
}

static gboolean
node_overflow_to_viewport(GHashTable *styles, const ns_node *n)
{
    const ns_node *root = n->parent;
    return n->kind == NS_NODE_ELEMENT && n->name &&
           g_ascii_strcasecmp(n->name, "body") == 0 && root &&
           node_is_root_element(root) &&
           !style_is_scroll_container(g_hash_table_lookup(styles, root));
}

static gboolean
node_is_scroll_container(GHashTable *styles, const ns_node *n)
{
    return !node_overflow_to_viewport(styles, n) &&
           style_is_scroll_container(g_hash_table_lookup(styles, n));
}

static int
style_containing_kind(const ns_style *st)
{
    const char *kw = ns_style_keyword(st, NS_CSS_POSITION);
    if (!kw) return 0;
    if (g_ascii_strcasecmp(kw, "fixed") == 0) return 2;
    return g_ascii_strcasecmp(kw, "absolute") == 0 ? 1 : 0;
}

static gboolean
style_has_transform(const ns_style *st)
{
    static const ns_css_prop props[4] = {
        NS_CSS_TRANSFORM, NS_CSS_TRANSLATE, NS_CSS_ROTATE, NS_CSS_SCALE,
    };
    for (int i = 0; i < 4; i++) {
        const ns_css_value *v = st->values[props[i]];
        if (v && v->kind == NS_CSS_V_TRANSFORM && v->u.transform.n_ops > 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
style_makes_fixed_cb(const ns_style *st)
{
    const ns_css_value *pv = st ? st->values[NS_CSS_PERSPECTIVE] : NULL;
    const char *filter = ns_style_keyword(st, NS_CSS_FILTER);
    if (!st) return FALSE;
    return style_has_transform(st) ||
           (pv && pv->kind == NS_CSS_V_LENGTH && pv->u.length.v > 0) ||
           (filter && g_ascii_strcasecmp(filter, "none") != 0);
}

static gboolean
style_contains_kind(const ns_style *st, int kind)
{
    const char *kw = ns_style_keyword(st, NS_CSS_POSITION);
    gboolean positioned = kw && g_ascii_strcasecmp(kw, "static") != 0;
    if (kind == 0) return TRUE;
    return (kind == 1 && positioned) || style_makes_fixed_cb(st);
}

static const ns_node *
node_root_element(const ns_node *n)
{
    while (!node_is_root_element(n)) n = n->parent;
    return n;
}

static const ns_node *
timeline_named_source(GHashTable *styles, const ns_node *n, const char *name,
                      int *index)
{
    for (const ns_node *p = n; p; p = p->parent) {
        int i = ns_css_timeline_name_index(g_hash_table_lookup(styles, p),
                                           NS_CSS_SCROLL_TIMELINE_NAME, name);
        if (i < 0) continue;
        *index = i;
        return p;
    }
    return NULL;
}

static const ns_node *
timeline_nearest_source(GHashTable *styles, const ns_node *n)
{
    int kind = style_containing_kind(g_hash_table_lookup(styles, n));
    const ns_node *p = n->parent;
    while (p && !node_is_root_element(p)) {
        const ns_style *st = g_hash_table_lookup(styles, p);
        if (style_contains_kind(st, kind)) {
            if (node_is_scroll_container(styles, p)) return p;
            kind = style_containing_kind(st);
        }
        p = p->parent;
    }
    return p;
}

static gboolean
style_declares_view_name(const ns_style *st, const char *name)
{
    return ns_css_timeline_name_index(st, NS_CSS_VIEW_TIMELINE_NAME, name) >= 0 ||
           ns_css_timeline_name_index(st, NS_CSS_TIMELINE_SCOPE, name) >= 0 ||
           ns_css_timeline_name_index(st, NS_CSS_TIMELINE_SCOPE, "all") >= 0;
}

static gboolean
timeline_view_declared(GHashTable *styles, const ns_node *n, const char *name)
{
    for (const ns_node *p = n; p; p = p->parent)
        if (style_declares_view_name(g_hash_table_lookup(styles, p), name))
            return TRUE;
    return FALSE;
}

static ns_css_timeline_kind
timeline_name_kind(GHashTable *styles, const ns_node *dom, const char *name)
{
    int index = 0;
    if (!styles || !name) return NS_CSS_TIMELINE_NONE;
    if (timeline_named_source(styles, dom, name, &index))
        return NS_CSS_TIMELINE_NAMED;
    return timeline_view_declared(styles, dom, name) ? NS_CSS_TIMELINE_VIEW
                                                     : NS_CSS_TIMELINE_NONE;
}

static void
entry_resolve_timeline(ns_css_anim_entry *e, GHashTable *styles,
                       const ns_node *dom, const ns_style *style, int index)
{
    if (e->timeline.kind == NS_CSS_TIMELINE_NAMED) {
        char *name = ns_css_timeline_name_at(style, index);
        e->timeline.kind = timeline_name_kind(styles, dom, name);
        g_free(name);
    }
    e->paused |= e->timeline.kind == NS_CSS_TIMELINE_NONE;
}

static void
anim_resolve_timelines(ns_css_anim_list *av, GHashTable *styles,
                       const ns_node *dom, const ns_style *style)
{
    for (int i = 0; i < av->n; i++)
        entry_resolve_timeline(&av->entries[i], styles, dom, style, i);
}

static const ns_node *
timeline_source_node(GHashTable *styles, const ns_node *n, const char *name,
                     const ns_css_timeline *t, ns_css_scroll_axis *axis)
{
    int index = 0;
    *axis = t->axis;
    if (t->kind == NS_CSS_TIMELINE_NAMED) {
        const ns_node *src = name ? timeline_named_source(styles, n, name, &index)
                                  : NULL;
        if (src) *axis = ns_css_timeline_axis_at(g_hash_table_lookup(styles, src), index);
        return src;
    }
    if (t->kind != NS_CSS_TIMELINE_SCROLL) return NULL;
    if (t->scroller == NS_CSS_SCROLLER_SELF) return n;
    if (t->scroller == NS_CSS_SCROLLER_ROOT) return node_root_element(n);
    return timeline_nearest_source(styles, n);
}

static int
timeline_physical_axis(const ns_style *st, ns_css_scroll_axis axis)
{
    gboolean vertical = ns_css_writing_mode(st) != 0;
    if (axis == NS_CSS_SCROLL_AXIS_X) return 0;
    if (axis == NS_CSS_SCROLL_AXIS_Y) return 1;
    return (axis == NS_CSS_SCROLL_AXIS_BLOCK) != vertical ? 1 : 0;
}

static double
run_scroll_progress(const ns_anim *a, const ns_anim_run *r)
{
    const ns_anim_scroll_pos *pos = r->tl_root
        ? (a->viewport_known ? &a->viewport : NULL)
        : g_hash_table_lookup(a->scroll_known, r->tl_source);
    double max = pos ? pos->max[r->tl_axis] : 0;
    if (!r->tl_source && !r->tl_root) return -1;
    if (r->tl_inactive) return -1;
    if (!pos) return 0.0;
    if (!(max > 0)) return -1;
    return CLAMP(pos->pos[r->tl_axis] / max, 0.0, 1.0);
}

static gboolean
run_timeline_zero(const ns_anim_run *r)
{
    return r->iterations <= 0 || !isfinite(r->iterations) ||
           (!r->duration_auto && r->duration_ms <= 0);
}

static gboolean
run_fills_forwards(const ns_anim_run *r)
{
    return r->fill == NS_CSS_ANIM_FILL_FORWARDS || r->fill == NS_CSS_ANIM_FILL_BOTH;
}

static double
run_timeline_progress(const ns_anim_run *r, double p)
{
    double overall = p * r->iterations;
    if (run_timeline_zero(r) || overall >= r->iterations)
        return run_end_progress(r);
    int iter = (int)overall;
    return directed_progress(iter, overall - iter, r->direction);
}

static gboolean
run_clear_values(ns_anim_run *r)
{
    gboolean had = (r->values && g_hash_table_size(r->values) > 0) ||
                   (r->partials && g_hash_table_size(r->partials) > 0);
    if (r->values) g_hash_table_remove_all(r->values);
    if (r->partials) g_hash_table_remove_all(r->partials);
    return had;
}

static gboolean
run_scroll_sample(ns_anim *a, ns_anim_run *r)
{
    double p = run_scroll_progress(a, r);
    if (p == r->tl_progress) return FALSE;
    r->tl_progress = p;
    if (p < 0 || (run_timeline_zero(r) && !run_fills_forwards(r)))
        return run_clear_values(r);
    return run_resample(r, run_timeline_progress(r, p));
}

static void
anim_scroll_lookup(ns_anim *a, const ns_node *src)
{
    ns_anim_scroll_pos pos = { { 0, 0 }, { 0, 0 } };
    if (!src || g_hash_table_contains(a->scroll_known, src)) return;
    a->scroll_fresh = TRUE;
    if (a->scroll_lookup &&
        a->scroll_lookup(src, pos.pos, pos.max, a->scroll_lookup_user))
        g_hash_table_insert(a->scroll_known, (gpointer)src,
                            g_memdup2(&pos, sizeof pos));
}

static void
run_bind_timeline(ns_anim *a, ns_anim_run *r, GHashTable *styles,
                  const ns_node *n)
{
    ns_css_scroll_axis axis = NS_CSS_SCROLL_AXIS_BLOCK;
    char *name = ns_css_timeline_name_at(g_hash_table_lookup(styles, n), r->index);
    const ns_node *src = timeline_source_node(styles, n, name, &r->timeline, &axis);
    const ns_style *src_style = src ? g_hash_table_lookup(styles, src) : NULL;
    g_free(name);
    r->tl_root = src && node_is_root_element(src);
    r->tl_source = src && !r->tl_root ? src : NULL;
    r->tl_inactive = r->tl_source && !node_is_scroll_container(styles, src);
    r->tl_axis = timeline_physical_axis(src_style, axis);
    if (r->tl_source) g_hash_table_add(a->scroll_sources, (gpointer)r->tl_source);
    anim_scroll_lookup(a, r->tl_source);
    a->scroll_fresh |= r->tl_root && !a->viewport_known;
    r->tl_progress = -2;
    a->scroll_runs++;
    a->scroll_root_runs += r->tl_root;
    run_scroll_sample(a, r);
}

static void
anim_scroll_bind(ns_anim *a, GHashTable *styles)
{
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_remove_all(a->scroll_sources);
    a->scroll_runs = 0;
    a->scroll_root_runs = 0;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = val;
        for (guint i = 0; s->runs && i < s->runs->len; i++) {
            ns_anim_run *r = s->runs->pdata[i];
            if (r->name && r->scroll_driven)
                run_bind_timeline(a, r, styles, key);
        }
    }
    a->scroll_sync = a->scroll_runs > 0;
}

static gboolean
run_touches_layout(const ns_anim_run *r)
{
    GHashTable *tables[2] = { r->values, r->partials };
    for (int t = 0; t < 2; t++) {
        GHashTableIter it;
        gpointer key, val;
        if (!tables[t]) continue;
        g_hash_table_iter_init(&it, tables[t]);
        while (g_hash_table_iter_next(&it, &key, &val))
            if (anim_prop_needs_relayout(GPOINTER_TO_INT(key))) return TRUE;
    }
    return FALSE;
}

void
ns_anim_set_scroll_lookup(ns_anim *a, ns_anim_scroll_lookup_cb cb, gpointer user)
{
    if (!a) return;
    a->scroll_lookup = cb;
    a->scroll_lookup_user = user;
}

gboolean
ns_anim_is_scroll_source(const ns_anim *a, const ns_node *node)
{
    return a && node && g_hash_table_contains(a->scroll_sources, node);
}

gboolean
ns_anim_scroll_uses_root(const ns_anim *a)
{
    return a && a->scroll_root_runs > 0;
}

gboolean
ns_anim_scroll_has_sources(const ns_anim *a)
{
    return a && g_hash_table_size(a->scroll_sources) > 0;
}

gboolean
ns_anim_scroll_fresh(const ns_anim *a)
{
    return a && a->scroll_fresh;
}

gboolean
ns_anim_scroll_needs_sync(const ns_anim *a, guint epoch)
{
    return a && a->scroll_runs > 0 && (a->scroll_sync || epoch != a->scroll_epoch);
}

void
ns_anim_scroll_reset(ns_anim *a, guint epoch)
{
    if (!a) return;
    g_hash_table_remove_all(a->scroll_known);
    a->viewport_known = FALSE;
    a->scroll_epoch = epoch;
}

void
ns_anim_note_scroll(ns_anim *a, const ns_node *node, double x, double y,
                    double max_x, double max_y)
{
    if (!a) return;
    ns_anim_scroll_pos pos = { { x, y }, { max_x, max_y } };
    if (!node) {
        a->viewport = pos;
        a->viewport_known = TRUE;
        return;
    }
    if (g_hash_table_contains(a->scroll_known, node)) return;
    g_hash_table_insert(a->scroll_known, (gpointer)node,
                        g_memdup2(&pos, sizeof pos));
}

static gboolean
run_scroll_resample(ns_anim *a, ns_anim_run *r, gboolean *layout)
{
    if (!r->name || !r->scroll_driven) return FALSE;
    gboolean before = run_touches_layout(r);
    if (!run_scroll_sample(a, r)) return FALSE;
    if (before || run_touches_layout(r)) *layout = TRUE;
    return TRUE;
}

static void
anim_scroll_mark_missing(ns_anim *a)
{
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, a->scroll_sources);
    while (g_hash_table_iter_next(&it, &key, NULL))
        if (!g_hash_table_contains(a->scroll_known, key))
            g_hash_table_insert(a->scroll_known, key,
                                g_new0(ns_anim_scroll_pos, 1));
}

gboolean
ns_anim_scroll_resample(ns_anim *a, gboolean *out_layout)
{
    GHashTableIter it;
    gpointer key, val;
    gboolean any = FALSE, layout = FALSE;
    if (!a) return FALSE;
    a->scroll_sync = FALSE;
    a->scroll_fresh = FALSE;
    anim_scroll_mark_missing(a);
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = val;
        for (guint i = 0; s->runs && i < s->runs->len; i++)
            any |= run_scroll_resample(a, s->runs->pdata[i], &layout);
    }
    if (out_layout) *out_layout = layout;
    return any;
}

static const ns_css_value *
state_prop_value(const ns_anim_state *s, int prop)
{
    for (int w = 1; w >= 0; w--) {
        GPtrArray *runs = state_runs(s, w);
        if (!runs) continue;
        for (guint i = runs->len; i > 0; i--) {
            const ns_anim_run *r = runs->pdata[i - 1];
            if (!r->values) continue;
            const ns_css_value *v = g_hash_table_lookup(r->values,
                                                        GINT_TO_POINTER(prop));
            if (v) return v;
        }
    }
    const ns_anim_chan *ch = chan_find(s, prop);
    if (ch && ch->active) return ch->current;
    return NULL;
}

gboolean
ns_anim_get_opacity(ns_anim *a, const ns_node *dom, double *out_opacity)
{
    if (!a || !dom || !out_opacity) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return FALSE;
    const ns_css_value *v = state_prop_value(s, NS_CSS_OPACITY);
    if (!v || v->kind != NS_CSS_V_LENGTH) return FALSE;
    double o = v->u.length.v;
    if (v->u.length.unit == NS_CSS_UNIT_PERCENT) o /= 100.0;
    *out_opacity = CLAMP(o, 0.0, 1.0);
    return TRUE;
}

const ns_css_transform *
ns_anim_get_transform(ns_anim *a, const ns_node *dom)
{
    if (!a || !dom) return NULL;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return NULL;
    const ns_css_value *v = state_prop_value(s, NS_CSS_TRANSFORM);
    if (!v || v->kind != NS_CSS_V_TRANSFORM || v->u.transform.n_ops == 0)
        return NULL;
    return &v->u.transform;
}

gboolean
ns_anim_get_color(ns_anim *a, const ns_node *dom,
                  ns_css_anim_target which, guint8 out_rgba[4])
{
    if (!a || !dom || !out_rgba) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, dom);
    if (!s) return FALSE;
    int prop = which == NS_CSS_ANIM_TARGET_COLOR ? NS_CSS_COLOR
             : which == NS_CSS_ANIM_TARGET_BG_COLOR ? NS_CSS_BACKGROUND_COLOR : -1;
    const ns_css_value *v = prop >= 0 ? state_prop_value(s, prop) : NULL;
    if (!v || v->kind != NS_CSS_V_COLOR) return FALSE;
    out_rgba[0] = v->u.color.r;
    out_rgba[1] = v->u.color.g;
    out_rgba[2] = v->u.color.b;
    out_rgba[3] = v->u.color.a;
    return TRUE;
}

static void
info_set_easing(ns_anim_info *out, const ns_css_timing *t)
{
    char *e = ns_css_timing_serialize(t);
    g_strlcpy(out->easing, e, sizeof out->easing);
    g_free(e);
}

static void
chan_info(const ns_anim_state *s, const ns_anim_chan *ch, gint64 now_us,
          ns_anim_info *out)
{
    memset(out, 0, sizeof *out);
    out->node = s->node;
    out->prop = ch->prop;
    out->run = -1;
    out->name = ns_css_prop_name(ch->prop);
    out->duration_ms = ch->duration_ms;
    out->delay_ms = ch->delay_ms;
    out->iterations = 1;
    out->fill = "backwards";
    out->direction = "normal";
    info_set_easing(out, &ch->timing);
    out->active = ch->active;
    out->paused = ch->paused;
    out->pending = ch->pending;
    out->finished = ch->finished;
    out->generation = ch->generation;
    double el = chan_elapsed_ms(ch, now_us) + ch->delay_ms;
    if (ch->finished) el = ch->delay_ms + ch->duration_ms;
    out->current_ms = el;
}

static const char *
fill_name(ns_css_anim_fill f)
{
    switch (f) {
    case NS_CSS_ANIM_FILL_FORWARDS:  return "forwards";
    case NS_CSS_ANIM_FILL_BACKWARDS: return "backwards";
    case NS_CSS_ANIM_FILL_BOTH:      return "both";
    default:                         return "none";
    }
}

static const char *
direction_name(ns_css_anim_direction d)
{
    switch (d) {
    case NS_CSS_ANIM_DIR_REVERSE:           return "reverse";
    case NS_CSS_ANIM_DIR_ALTERNATE:         return "alternate";
    case NS_CSS_ANIM_DIR_ALTERNATE_REVERSE: return "alternate-reverse";
    default:                                return "normal";
    }
}

static void
run_info(const ns_anim_state *s, const ns_anim_run *r, gint64 now_us,
         ns_anim_info *out)
{
    memset(out, 0, sizeof *out);
    out->node = s->node;
    out->prop = NS_ANIM_KEYFRAME_PROP;
    out->run = r->index;
    out->name = r->is_script ? NULL : g_intern_string(r->name);
    out->duration_ms = r->duration_ms;
    out->delay_ms = r->delay_ms;
    out->iterations = r->iterations;
    out->fill = fill_name(r->fill);
    out->direction = direction_name(r->direction);
    info_set_easing(out, &r->timing);
    out->active = r->active;
    out->paused = r->paused;
    out->pending = r->pending;
    out->finished = r->finished;
    out->generation = r->generation;
    double el = run_elapsed_ms(r, now_us) + r->delay_ms;
    if (r->finished && !r->paused) {
        double active = run_active_ms(r);
        el = MAX(el, r->delay_ms + (isfinite(active) ? active : 0));
    }
    out->current_ms = el;
}

static gboolean
run_is_live(const ns_anim_run *r)
{
    return r->active || r->scroll_driven;
}

static gboolean
run_in_effect(const ns_anim_run *r)
{
    if (!r->name) return FALSE;
    if (run_is_live(r)) return TRUE;
    return r->finished && (r->fill == NS_CSS_ANIM_FILL_FORWARDS ||
                           r->fill == NS_CSS_ANIM_FILL_BOTH);
}

void
ns_anim_visit(ns_anim *a, const ns_node *node, ns_anim_visit_cb cb,
              gpointer user)
{
    if (!a || !cb) return;
    gint64 now = anim_now(a);
    GArray *infos = g_array_new(FALSE, FALSE, sizeof(ns_anim_info));
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_anim_state *s = val;
        if (node && s->node != node) continue;
        ns_anim_info info;
        for (int w = 0; w < 2; w++) {
            GPtrArray *runs = state_runs(s, w);
            if (!runs) continue;
            for (guint i = 0; i < runs->len; i++) {
                ns_anim_run *r = runs->pdata[i];
                if (!run_in_effect(r)) continue;
                run_info(s, r, now, &info);
                g_array_append_val(infos, info);
            }
        }
        if (s->chans)
            for (guint i = 0; i < s->chans->len; i++) {
                ns_anim_chan *ch = s->chans->pdata[i];
                if (!ch->active) continue;
                chan_info(s, ch, now, &info);
                g_array_append_val(infos, info);
            }
    }
    for (guint i = 0; i < infos->len; i++)
        cb(&g_array_index(infos, ns_anim_info, i), user);
    g_array_free(infos, TRUE);
}

static ns_anim_run *
run_for(const ns_anim_state *s, int prop)
{
    int index = -1 - prop;
    GPtrArray *runs = s->runs;
    if (index >= NS_ANIM_SCRIPT_BASE) {
        index -= NS_ANIM_SCRIPT_BASE;
        runs = s->scripts;
    }
    if (!runs || index < 0 || (guint)index >= runs->len) return NULL;
    ns_anim_run *r = runs->pdata[index];
    return (r->name || r->cancelled_name) ? r : NULL;
}

static ns_css_anim_direction
direction_from_name(const char *d)
{
    if (!d) return NS_CSS_ANIM_DIR_NORMAL;
    if (strcmp(d, "reverse") == 0) return NS_CSS_ANIM_DIR_REVERSE;
    if (strcmp(d, "alternate") == 0) return NS_CSS_ANIM_DIR_ALTERNATE;
    if (strcmp(d, "alternate-reverse") == 0) return NS_CSS_ANIM_DIR_ALTERNATE_REVERSE;
    return NS_CSS_ANIM_DIR_NORMAL;
}

static ns_css_anim_fill
fill_from_name(const char *f)
{
    if (!f) return NS_CSS_ANIM_FILL_NONE;
    if (strcmp(f, "forwards") == 0) return NS_CSS_ANIM_FILL_FORWARDS;
    if (strcmp(f, "backwards") == 0) return NS_CSS_ANIM_FILL_BACKWARDS;
    if (strcmp(f, "both") == 0) return NS_CSS_ANIM_FILL_BOTH;
    return NS_CSS_ANIM_FILL_NONE;
}

gboolean
ns_anim_script_start(ns_anim *a, const ns_node *node,
                     const char *const *stop_css, const double *stop_pct,
                     int n_stops, const ns_anim_script_timing *t,
                     int *out_prop, guint *out_generation)
{
    if (!a || !node || !t) return FALSE;
    if (a->active_count >= NS_ANIM_MAX_ACTIVE) return FALSE;
    ns_anim_state *s = state_for(a, node);
    if (!s->scripts) s->scripts = g_ptr_array_new_with_free_func(run_free);
    ns_anim_run *r = NULL;
    for (guint i = 0; i < s->scripts->len && !r; i++) {
        ns_anim_run *c = s->scripts->pdata[i];
        if (!c->name && !c->cancelled_name && !c->active) r = c;
    }
    if (!r) {
        r = g_new0(ns_anim_run, 1);
        r->index = NS_ANIM_SCRIPT_BASE + (int)s->scripts->len;
        r->is_script = TRUE;
        g_ptr_array_add(s->scripts, r);
    }
    g_free(r->name);
    r->name = g_strdup("");
    g_free(r->cancelled_name);
    r->cancelled_name = NULL;
    ns_css_keyframes_resolved_free(r->kf);
    r->kf = NULL;
    anim_stops_free(r->stops);
    r->stops = g_array_new(FALSE, TRUE, sizeof(ns_anim_kf_stop));
    for (int i = 0; i < n_stops; i++) {
        ns_anim_kf_stop st = { stop_pct[i] * 100.0,
                               ns_css_parse_declarations(stop_css[i]) };
        g_array_append_val(r->stops, st);
    }
    gint64 now = anim_now(a);
    r->start_us = now;
    r->duration_ms = MAX(t->duration_ms, 0.0);
    r->delay_ms = t->delay_ms;
    r->iterations = t->iterations < 0 ? 0 : t->iterations;
    r->direction = direction_from_name(t->direction);
    r->fill = fill_from_name(t->fill);
    r->timing = (ns_css_timing){ .kind = NS_CSS_TIMING_LINEAR };
    if (t->easing) ns_css_timing_parse(t->easing, &r->timing);
    r->composite = composite_from_name(t->composite, NS_ANIM_COMPOSITE_REPLACE);
    r->active = TRUE;
    r->paused = FALSE;
    r->pending = TRUE;
    r->finished = FALSE;
    r->iters_emitted = 0;
    r->elapsed_base_ms = 0;
    r->generation = ++s->run_generation;
    if (r->values) g_hash_table_remove_all(r->values);
    a->active_count++;
    advance_run(a, r, now);
    anim_track(a, s);
    if (out_prop) *out_prop = -1 - r->index;
    if (out_generation) *out_generation = r->generation;
    return TRUE;
}

static GArray *
anim_decls_ref(const GArray *decls)
{
    if (!decls) return NULL;
    GArray *out = g_array_sized_new(FALSE, FALSE, sizeof(ns_css_decl),
                                    decls->len);
    for (guint i = 0; i < decls->len; i++) {
        ns_css_decl d = g_array_index(decls, ns_css_decl, i);
        d.value = ns_css_value_dup(d.value);
        g_array_append_val(out, d);
    }
    return out;
}

void
ns_anim_keyframes_visit(ns_anim *a, const ns_node *node, int prop,
                        ns_anim_keyframe_cb cb, gpointer user)
{
    if (!a || !node || !cb) return;
    ns_anim_state *s = g_hash_table_lookup(a->states, node);
    if (!s) return;
    ns_anim_run *r = run_for(s, prop);
    if (!r || !r->name || !r->stops) return;
    GArray *stops = g_array_sized_new(FALSE, TRUE, sizeof(ns_anim_kf_stop),
                                      r->stops->len);
    GPtrArray *easings = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < r->stops->len; i++) {
        const ns_anim_kf_stop *st = &g_array_index(r->stops, ns_anim_kf_stop, i);
        ns_css_timing tm;
        if (!stop_timing(st, &tm)) tm = r->timing;
        ns_anim_kf_stop copy = { st->pct, anim_decls_ref(st->decls) };
        g_array_append_val(stops, copy);
        g_ptr_array_add(easings, ns_css_timing_serialize(&tm));
    }
    for (guint i = 0; i < stops->len; i++) {
        const ns_anim_kf_stop *st = &g_array_index(stops, ns_anim_kf_stop, i);
        cb(st->pct / 100.0, g_ptr_array_index(easings, i), st->decls, user);
    }
    g_ptr_array_free(easings, TRUE);
    anim_stops_free(stops);
}

gboolean
ns_anim_info_for(ns_anim *a, const ns_node *node, int prop, ns_anim_info *out)
{
    if (!a || !node || !out) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, node);
    if (!s) return FALSE;
    gint64 now = anim_now(a);
    if (prop < 0) {
        ns_anim_run *r = run_for(s, prop);
        if (!r || !r->name) return FALSE;
        run_info(s, r, now, out);
        return TRUE;
    }
    ns_anim_chan *ch = chan_find(s, prop);
    if (!ch || (!ch->active && !ch->finished)) return FALSE;
    chan_info(s, ch, now, out);
    return TRUE;
}

gboolean
ns_anim_seek(ns_anim *a, const ns_node *node, int prop, double ms)
{
    if (!a || !node) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, node);
    if (!s) return FALSE;
    gint64 now = anim_now(a);
    if (prop < 0) {
        ns_anim_run *r = run_for(s, prop);
        if (!r) return FALSE;
        if (!r->name) {
            r->name = r->cancelled_name;
            r->cancelled_name = NULL;
            r->active = TRUE;
            r->finished = FALSE;
            r->paused = TRUE;
            r->pending = FALSE;
            r->elapsed_base_ms = ms - r->delay_ms;
            a->active_count++;
            advance_run(a, r, now);
            run_emit_progress(a, s, r, now);
            anim_track(a, s);
            return TRUE;
        }
        double el = ms - r->delay_ms;
        r->pending = FALSE;
        if (r->paused) r->elapsed_base_ms = el;
        else r->start_us = now - ms * 1000.0;
        if (!r->active && r->finished) {
            r->active = TRUE;
            r->finished = FALSE;
            a->active_count++;
        }
        advance_run(a, r, now);
        run_emit_progress(a, s, r, now);
        anim_track(a, s);
        return TRUE;
    }
    ns_anim_chan *ch = chan_find(s, prop);
    if (!ch || (!ch->active && !ch->finished)) return FALSE;
    double el = ms - ch->delay_ms;
    ch->pending = FALSE;
    if (ch->paused) ch->paused_elapsed_ms = el;
    else ch->start_us = now - ms * 1000.0;
    if (!ch->active) {
        ch->active = TRUE;
        ch->finished = FALSE;
        a->active_count++;
    }
    advance_chan(a, s, ch, now);
    anim_track(a, s);
    return TRUE;
}

gboolean
ns_anim_control(ns_anim *a, const ns_node *node, int prop, const char *op)
{
    if (!a || !node || !op) return FALSE;
    ns_anim_state *s = g_hash_table_lookup(a->states, node);
    if (!s) return FALSE;
    gint64 now = anim_now(a);
    if (prop < 0) {
        ns_anim_run *r = run_for(s, prop);
        if (!r) return FALSE;
        if (strcmp(op, "pause") == 0) {
            if (!r->name) return FALSE;
            if (r->pending) { r->pending = FALSE; r->start_us = now; }
            run_set_paused(r, TRUE, now);
            r->api_override = TRUE;
        } else if (strcmp(op, "play") == 0) {
            r->api_override = TRUE;
            if (!r->name) {
                r->name = r->cancelled_name;
                r->cancelled_name = NULL;
                r->active = TRUE;
                r->finished = FALSE;
                r->paused = FALSE;
                r->pending = TRUE;
                r->start_us = now;
                a->active_count++;
                advance_run(a, r, now);
            } else {
                run_set_paused(r, FALSE, now);
                if (!r->active && r->finished) {
                    r->active = TRUE;
                    r->finished = FALSE;
                    a->active_count++;
                    r->start_us = now;
                }
            }
        } else if (strcmp(op, "finish") == 0) {
            if (!r->name) return FALSE;
            double active = run_active_ms(r);
            double total = r->delay_ms + (isfinite(active) ? active : 0);
            r->paused = FALSE;
            r->pending = FALSE;
            r->start_us = now - total * 1000.0;
            advance_run(a, r, now);
            run_emit_progress(a, s, r, now);
        } else if (strcmp(op, "cancel") == 0) {
            if (!r->name) return FALSE;
            char *keep = g_strdup(r->name);
            run_cancel(a, s, r);
            g_free(r->cancelled_name);
            r->cancelled_name = keep;
        }
        anim_track(a, s);
        return TRUE;
    }
    ns_anim_chan *ch = chan_find(s, prop);
    if (!ch || (!ch->active && !ch->finished && !ch->cancelled)) return FALSE;
    if (ch->cancelled && strcmp(op, "play") == 0) {
        ch->cancelled = FALSE;
        ch->active = TRUE;
        ch->finished = FALSE;
        ch->paused = FALSE;
        ch->phase = NS_ANIM_PHASE_IDLE;
        ch->start_us = now;
        ns_css_value_free(ch->current);
        ch->current = ns_css_value_dup(ch->from);
        a->active_count++;
        anim_emit(a, s->node, "transitionrun", ns_css_prop_name(ch->prop), 0.0);
        advance_chan(a, s, ch, now);
        anim_track(a, s);
        return TRUE;
    }
    if (ch->cancelled) return FALSE;
    if (strcmp(op, "pause") == 0 && !ch->paused) {
        ch->paused_elapsed_ms = chan_elapsed_ms(ch, now);
        ch->pending = FALSE;
        ch->paused = TRUE;
    } else if (strcmp(op, "play") == 0 && ch->paused) {
        ch->start_us = now - (ch->paused_elapsed_ms + ch->delay_ms) * 1000.0;
        ch->paused = FALSE;
    } else if (strcmp(op, "finish") == 0) {
        ch->paused = FALSE;
        ch->pending = FALSE;
        ch->start_us = now - (ch->delay_ms + ch->duration_ms) * 1000.0;
        if (ch->active) advance_chan(a, s, ch, now);
    } else if (strcmp(op, "cancel") == 0) {
        if (ch->active) chan_cancel(a, s, ch, now);
        ch->finished = FALSE;
        ch->cancelled = TRUE;
    }
    anim_track(a, s);
    return TRUE;
}
