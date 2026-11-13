/* Nordstjernen — SVG SMIL animation timeline: animate, set, animateTransform and animateMotion.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#include "smil.h"

#include <math.h>
#include <string.h>

#include "css.h"
#include "svg.h"

#define SMIL_KEPT_INSTANCES 16
#define SMIL_SYNC_PASSES    32
#define SMIL_TIME_EPSILON   1e-6
#define SMIL_MAX_EVENTS     4096

typedef enum { SMIL_ANIMATE, SMIL_SET, SMIL_TRANSFORM, SMIL_MOTION } smil_kind;
typedef enum { SMIL_LINEAR, SMIL_DISCRETE, SMIL_PACED, SMIL_SPLINE } smil_calc;
typedef enum { SMIL_RESTART_ALWAYS, SMIL_RESTART_WHEN_IDLE, SMIL_RESTART_NEVER } smil_restart;
typedef enum {
    SMIL_AT_OFFSET, SMIL_AT_SYNC_BEGIN, SMIL_AT_SYNC_END, SMIL_AT_EVENT,
} smil_begin_kind;

typedef struct smil_begin {
    smil_begin_kind kind;
    double          offset;
    char           *id;
    char           *event;
} smil_begin;

typedef struct smil_event {
    const ns_node     *node;
    ns_smil_event_kind kind;
    int                iteration;
} smil_event;

typedef struct smil_clock {
    gboolean paused;
    gint64   paused_at_us;
    gint64   offset_us;
} smil_clock;

typedef struct smil_el {
    const ns_node *node;
    const ns_node *target;
    const ns_node *svg;
    smil_kind      kind;
    smil_calc      calc;
    smil_restart   restart;
    char          *attr;
    char          *type;
    char          *begin_src;
    GPtrArray     *values;
    GArray        *key_times;
    GArray        *key_splines;
    GArray        *key_points;
    GArray        *begins;
    GArray        *ends;
    GArray        *end_specs;
    GArray        *end_instances;
    GArray        *instances;
    GArray        *path;
    double         dur;
    double         repeat_count;
    double         repeat_dur;
    double         forced_begin;
    double         forced_end;
    double         horizon;
    double         rotate;
    double         reported_begin;
    double         reported_iteration;
    guint          order;
    gboolean       reported;
    gboolean       reported_end;
    gboolean       has_forced_end;
    gboolean       is_color;
    gboolean       to_anim;
    gboolean       additive;
    gboolean       accumulate;
    gboolean       freeze;
    gboolean       rotate_auto;
    gboolean       rotate_reverse;
} smil_el;

typedef struct smil_override {
    const ns_node *node;
    char          *name;
    char          *value;
} smil_override;

typedef struct smil_running {
    smil_el *el;
    double   begin;
    double   active_time;
    gboolean ended;
} smil_running;

typedef struct smil_nums {
    GArray  *nums;
    GString *tpl;
} smil_nums;

struct ns_smil {
    const ns_node *doc;
    GPtrArray     *els;
    GHashTable    *by_node;
    GHashTable    *clocks;
    GHashTable    *overrides;
    GArray        *events;
    char          *signature;
    gint64         origin_us;
    gint64         now_us;
    guint64        serial;
    gboolean       scanned;
    gboolean       dirty;
    gboolean       active;
    gboolean       unreported;
    gboolean       has_event_specs;
};

static __thread GPtrArray *g_smils;
static __thread guint      g_smil_override_nodes;
static __thread guint      g_smil_event_docs;

static const char *
smil_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') p++;
    return p;
}

static gsize
smil_number_span(const char *p)
{
    const char *q = p;
    if (*q == '+' || *q == '-') q++;
    const char *digits = q;
    while (g_ascii_isdigit(*q)) q++;
    gsize int_len = (gsize)(q - digits);
    gsize frac_len = 0;
    if (*q == '.') {
        const char *f = q + 1;
        while (g_ascii_isdigit(*f)) f++;
        frac_len = (gsize)(f - (q + 1));
        if (int_len || frac_len) q = f;
    }
    if (!int_len && !frac_len) return 0;
    if (*q == 'e' || *q == 'E') {
        const char *e = q + 1;
        if (*e == '+' || *e == '-') e++;
        if (g_ascii_isdigit(*e)) {
            while (g_ascii_isdigit(*e)) e++;
            q = e;
        }
    }
    return (gsize)(q - p);
}

static double
smil_span_value(const char *p, gsize len)
{
    char buf[64];
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return g_ascii_strtod(buf, NULL);
}

static void
smil_nums_clear(smil_nums *n)
{
    if (n->nums) g_array_free(n->nums, TRUE);
    if (n->tpl) g_string_free(n->tpl, TRUE);
    n->nums = NULL;
    n->tpl = NULL;
}

static gboolean
smil_nums_parse(const char *s, smil_nums *out)
{
    out->nums = NULL;
    out->tpl = NULL;
    if (!s || strchr(s, '#') || strstr(s, "url(")) return FALSE;
    out->nums = g_array_new(FALSE, FALSE, sizeof(double));
    out->tpl = g_string_new(NULL);
    for (const char *p = s; *p;) {
        gsize len = smil_number_span(p);
        if (!len) {
            g_string_append_c(out->tpl, *p++);
            continue;
        }
        const char *digits = p + (*p == '+' || *p == '-');
        if (digits[0] == '0' && g_ascii_isdigit(digits[1])) {
            smil_nums_clear(out);
            return FALSE;
        }
        double v = smil_span_value(p, len);
        g_array_append_val(out->nums, v);
        g_string_append_c(out->tpl, '\001');
        p += len;
    }
    if (out->nums->len == 0) {
        smil_nums_clear(out);
        return FALSE;
    }
    return TRUE;
}

static gboolean
smil_is_separator(char c)
{
    return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r';
}

static gboolean
smil_nums_compatible(const smil_nums *a, const smil_nums *b)
{
    if (a->nums->len != b->nums->len) return FALSE;
    const char *p = a->tpl->str, *q = b->tpl->str;
    for (;;) {
        while (smil_is_separator(*p)) p++;
        while (smil_is_separator(*q)) q++;
        if (*p != *q) return FALSE;
        if (!*p) return TRUE;
        p++;
        q++;
    }
}

static void
smil_append_number(GString *out, double v)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    if (!isfinite(v)) v = 0;
    g_string_append(out, g_ascii_formatd(buf, sizeof buf, "%.6g", v));
}

static char *
smil_nums_format(const GString *tpl, const double *vals)
{
    GString *out = g_string_new(NULL);
    guint i = 0;
    for (const char *p = tpl->str; *p; p++) {
        if (*p == '\001') smil_append_number(out, vals[i++]);
        else g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static char *
smil_color_format(const double c[4])
{
    GString *out = g_string_new("rgba(");
    for (int i = 0; i < 3; i++) {
        smil_append_number(out, round(CLAMP(c[i], 0.0, 255.0)));
        g_string_append_c(out, ',');
    }
    smil_append_number(out, CLAMP(c[3], 0.0, 255.0) / 255.0);
    g_string_append_c(out, ')');
    return g_string_free(out, FALSE);
}

static gboolean
smil_color_parse(const char *s, double c[4])
{
    guint8 r, g, b, a;
    if (!s || !ns_css_parse_color(s, &r, &g, &b, &a)) return FALSE;
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    return TRUE;
}

static char *
smil_combine(const char *a, const char *b, double wa, double wb,
             gboolean is_color)
{
    if (is_color) {
        double ca[4], cb[4], out[4];
        if (!smil_color_parse(a, ca) || !smil_color_parse(b, cb)) return NULL;
        for (int i = 0; i < 4; i++) out[i] = ca[i] * wa + cb[i] * wb;
        return smil_color_format(out);
    }
    smil_nums na, nb;
    char *res = NULL;
    gboolean ok_a = smil_nums_parse(a, &na);
    gboolean ok_b = smil_nums_parse(b, &nb);
    if (ok_a && ok_b && smil_nums_compatible(&na, &nb)) {
        double *vals = g_new(double, na.nums->len);
        for (guint i = 0; i < na.nums->len; i++)
            vals[i] = g_array_index(na.nums, double, i) * wa +
                      g_array_index(nb.nums, double, i) * wb;
        res = smil_nums_format(na.tpl, vals);
        g_free(vals);
    }
    smil_nums_clear(&na);
    smil_nums_clear(&nb);
    return res;
}

static char *
smil_zero_like(const char *s)
{
    smil_nums n;
    if (!smil_nums_parse(s, &n)) return NULL;
    double *vals = g_new0(double, n.nums->len);
    char *res = smil_nums_format(n.tpl, vals);
    g_free(vals);
    smil_nums_clear(&n);
    return res;
}

static double
smil_distance(const char *a, const char *b, gboolean is_color)
{
    double sum = 0;
    if (is_color) {
        double ca[4], cb[4];
        if (!smil_color_parse(a, ca) || !smil_color_parse(b, cb)) return -1;
        for (int i = 0; i < 4; i++) sum += (ca[i] - cb[i]) * (ca[i] - cb[i]);
        return sqrt(sum);
    }
    smil_nums na, nb;
    double res = -1;
    gboolean ok_a = smil_nums_parse(a, &na);
    gboolean ok_b = smil_nums_parse(b, &nb);
    if (ok_a && ok_b && smil_nums_compatible(&na, &nb)) {
        for (guint i = 0; i < na.nums->len; i++) {
            double d = g_array_index(na.nums, double, i) -
                       g_array_index(nb.nums, double, i);
            sum += d * d;
        }
        res = sqrt(sum);
    }
    smil_nums_clear(&na);
    smil_nums_clear(&nb);
    return res;
}

static double
smil_bezier_axis(double a, double b, double t)
{
    double u = 1.0 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static double
smil_spline_ease(const double *k, double x)
{
    double lo = 0, hi = 1;
    for (int i = 0; i < 32; i++) {
        double mid = (lo + hi) / 2;
        if (smil_bezier_axis(k[0], k[2], mid) < x) lo = mid;
        else hi = mid;
    }
    return smil_bezier_axis(k[1], k[3], (lo + hi) / 2);
}

static gboolean
smil_key_times_usable(const smil_el *el, guint n)
{
    if (!el->key_times || el->key_times->len != n || n < 2) return FALSE;
    if (g_array_index(el->key_times, double, 0) != 0) return FALSE;
    for (guint i = 1; i < n; i++)
        if (g_array_index(el->key_times, double, i) <
            g_array_index(el->key_times, double, i - 1))
            return FALSE;
    return TRUE;
}

static guint
smil_key_segment(const GArray *times, double p, double *local)
{
    guint last = times->len - 1;
    guint i = 0;
    while (i + 1 < last && p >= g_array_index(times, double, i + 1)) i++;
    double t0 = g_array_index(times, double, i);
    double t1 = g_array_index(times, double, i + 1);
    *local = t1 > t0 ? CLAMP((p - t0) / (t1 - t0), 0.0, 1.0) : 1.0;
    return i;
}

static gboolean
smil_paced_segment(const smil_el *el, const char *const *vals, guint n,
                   double p, guint *seg, double *local)
{
    double *dist = g_new(double, n - 1);
    double total = 0;
    gboolean ok = TRUE;
    for (guint i = 0; i + 1 < n && ok; i++) {
        dist[i] = smil_distance(vals[i], vals[i + 1], el->is_color);
        if (dist[i] < 0) ok = FALSE;
        else total += dist[i];
    }
    if (ok && total > 0) {
        double at = p * total;
        guint i = 0;
        while (i + 2 < n && at > dist[i]) at -= dist[i++];
        *seg = i;
        *local = dist[i] > 0 ? CLAMP(at / dist[i], 0.0, 1.0) : 1.0;
    } else {
        ok = FALSE;
    }
    g_free(dist);
    return ok;
}

static guint
smil_discrete_index(const smil_el *el, guint n, double p)
{
    if (smil_key_times_usable(el, n)) {
        guint i = 0;
        while (i + 1 < n && p >= g_array_index(el->key_times, double, i + 1)) i++;
        return i;
    }
    guint i = (guint)floor(p * n);
    return MIN(i, n - 1);
}

static char *
smil_sample(const smil_el *el, const char *const *vals, guint n, double p)
{
    if (n == 0) return NULL;
    if (n == 1) return g_strdup(vals[0]);
    if (el->calc != SMIL_DISCRETE) {
        guint seg = 0;
        double local = 0;
        gboolean paced = el->calc == SMIL_PACED &&
                         smil_paced_segment(el, vals, n, p, &seg, &local);
        if (!paced && smil_key_times_usable(el, n)) {
            seg = smil_key_segment(el->key_times, p, &local);
        } else if (!paced) {
            double at = p * (n - 1);
            seg = MIN((guint)floor(at), n - 2);
            local = at - seg;
        }
        if (el->calc == SMIL_SPLINE && el->key_splines &&
            el->key_splines->len >= 4 * (seg + 1))
            local = smil_spline_ease(
                &g_array_index(el->key_splines, double, 4 * seg), local);
        char *mixed = smil_combine(vals[seg], vals[seg + 1], 1.0 - local, local,
                                   el->is_color);
        if (mixed) return mixed;
    }
    return g_strdup(vals[smil_discrete_index(el, n, p)]);
}

static const char *
smil_default_for(const char *attr)
{
    static const char *const unit_props[] = {
        "opacity", "fill-opacity", "stroke-opacity", "stop-opacity",
        "flood-opacity",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(unit_props); i++)
        if (strcmp(attr, unit_props[i]) == 0) return "1";
    return NULL;
}

static double
smil_path_length(const GArray *pts)
{
    double total = 0;
    for (guint i = 1; i < pts->len; i++) {
        const ns_svg_path_pt *a = &g_array_index(pts, ns_svg_path_pt, i - 1);
        const ns_svg_path_pt *b = &g_array_index(pts, ns_svg_path_pt, i);
        if (!b->move) total += hypot(b->x - a->x, b->y - a->y);
    }
    return total;
}

static void
smil_path_point(const GArray *pts, double at, double *x, double *y,
                double *angle)
{
    *x = 0; *y = 0; *angle = 0;
    if (pts->len == 0) return;
    const ns_svg_path_pt *first = &g_array_index(pts, ns_svg_path_pt, 0);
    *x = first->x;
    *y = first->y;
    double walked = 0;
    for (guint i = 1; i < pts->len; i++) {
        const ns_svg_path_pt *a = &g_array_index(pts, ns_svg_path_pt, i - 1);
        const ns_svg_path_pt *b = &g_array_index(pts, ns_svg_path_pt, i);
        if (b->move) continue;
        double seg = hypot(b->x - a->x, b->y - a->y);
        if (seg <= 0) continue;
        *angle = atan2(b->y - a->y, b->x - a->x);
        if (at <= walked + seg) {
            double t = (at - walked) / seg;
            *x = a->x + (b->x - a->x) * t;
            *y = a->y + (b->y - a->y) * t;
            return;
        }
        walked += seg;
        *x = b->x;
        *y = b->y;
    }
}

static char *
smil_motion_transform(const smil_el *el, double x, double y, double angle)
{
    GString *out = g_string_new("translate(");
    smil_append_number(out, x);
    g_string_append_c(out, ' ');
    smil_append_number(out, y);
    g_string_append_c(out, ')');
    double deg = el->rotate;
    if (el->rotate_auto)
        deg = angle * 180.0 / G_PI + (el->rotate_reverse ? 180.0 : 0.0);
    if (deg != 0) {
        g_string_append(out, " rotate(");
        smil_append_number(out, deg);
        g_string_append_c(out, ')');
    }
    return g_string_free(out, FALSE);
}

static double
smil_motion_fraction(const smil_el *el, double p)
{
    if (!el->key_points || el->key_points->len < 2 || !el->key_times ||
        el->key_times->len != el->key_points->len)
        return p;
    double local;
    guint i = smil_key_segment(el->key_times, p, &local);
    double a = g_array_index(el->key_points, double, i);
    double b = g_array_index(el->key_points, double, i + 1);
    if (el->calc == SMIL_DISCRETE) return local >= 1.0 ? b : a;
    return a + (b - a) * local;
}

static char *
smil_motion_on_path(const smil_el *el, double p)
{
    double x, y, angle;
    double f = CLAMP(smil_motion_fraction(el, p), 0.0, 1.0);
    smil_path_point(el->path, f * smil_path_length(el->path), &x, &y, &angle);
    return smil_motion_transform(el, x, y, angle);
}

static char *
smil_motion_from_pair(const smil_el *el, const char *raw)
{
    smil_nums n;
    if (!smil_nums_parse(raw, &n)) return NULL;
    char *res = NULL;
    if (n.nums->len >= 2)
        res = smil_motion_transform(el, g_array_index(n.nums, double, 0),
                                    g_array_index(n.nums, double, 1), 0);
    smil_nums_clear(&n);
    return res;
}

static char *
smil_sample_to(const smil_el *el, const char *under, double p)
{
    const char *to = g_ptr_array_index(el->values, 0);
    const char *from = el->kind == SMIL_ANIMATE ? under : NULL;
    if (!from && el->kind == SMIL_ANIMATE) from = smil_default_for(el->attr);
    char *zero = NULL;
    if (!from) from = zero = smil_zero_like(to);
    char *raw;
    if (from) {
        const char *pair[2] = { from, to };
        raw = smil_sample(el, pair, 2, p);
    } else {
        raw = g_strdup(to);
    }
    g_free(zero);
    return raw;
}

static char *
smil_eval(const smil_el *el, double active_time, gboolean ended,
          const char *under)
{
    double p = 0, iteration = 0;
    if (el->dur > 0) {
        double q = active_time / el->dur;
        iteration = floor(q);
        p = q - iteration;
        if (ended && iteration > 0 && p < 1e-9) {
            iteration -= 1;
            p = 1;
        }
    }
    if (el->kind == SMIL_MOTION && el->path && el->path->len > 0)
        return smil_motion_on_path(el, p);
    guint n = el->values->len;
    if (n == 0) return NULL;

    const char *const *vals = (const char *const *)el->values->pdata;
    char *raw;
    if (el->kind == SMIL_SET) raw = g_strdup(vals[0]);
    else if (el->to_anim)     raw = smil_sample_to(el, under, p);
    else                      raw = smil_sample(el, vals, n, p);
    if (!raw) return NULL;

    if (el->accumulate && iteration > 0 && !el->to_anim &&
        el->kind != SMIL_SET) {
        char *sum = smil_combine(raw, vals[n - 1], 1.0, iteration, el->is_color);
        if (sum) { g_free(raw); raw = sum; }
    }

    if (el->kind == SMIL_MOTION) {
        char *motion = smil_motion_from_pair(el, raw);
        g_free(raw);
        return motion;
    }
    if (el->kind == SMIL_TRANSFORM) {
        char *wrapped = g_strdup_printf("%s%s%s(%s)",
                                        el->additive && under ? under : "",
                                        el->additive && under ? " " : "",
                                        el->type, raw);
        g_free(raw);
        return wrapped;
    }
    if (el->kind == SMIL_ANIMATE && el->additive && !el->to_anim) {
        const char *base = under ? under : smil_default_for(el->attr);
        char *sum = base ? smil_combine(base, raw, 1.0, 1.0, el->is_color) : NULL;
        if (sum) { g_free(raw); raw = sum; }
    }
    return raw;
}

static gboolean
smil_parse_clock(const char *s, double *out)
{
    if (!s) return FALSE;
    s = smil_skip_ws(s);
    double sign = 1;
    if (*s == '+') s = smil_skip_ws(s + 1);
    else if (*s == '-') { sign = -1; s = smil_skip_ws(s + 1); }
    if (!g_ascii_isdigit(*s) && *s != '.') return FALSE;
    double parts[3];
    int n = 0;
    const char *p = s;
    for (;;) {
        gsize len = smil_number_span(p);
        if (!len || *p == '+' || *p == '-') return FALSE;
        parts[n++] = smil_span_value(p, len);
        p += len;
        if (*p != ':' || n == 3) break;
        p++;
    }
    double v;
    if (n == 3)      v = parts[0] * 3600 + parts[1] * 60 + parts[2];
    else if (n == 2) v = parts[0] * 60 + parts[1];
    else {
        v = parts[0];
        p = smil_skip_ws(p);
        if (g_str_has_prefix(p, "ms"))       { v /= 1000.0; p += 2; }
        else if (g_str_has_prefix(p, "min")) { v *= 60.0;   p += 3; }
        else if (*p == 'h')                  { v *= 3600.0; p += 1; }
        else if (*p == 's')                  { p += 1; }
    }
    if (*smil_skip_ws(p)) return FALSE;
    *out = sign * v;
    return TRUE;
}

static GArray *
smil_parse_number_list(const char *s)
{
    if (!s) return NULL;
    GArray *out = g_array_new(FALSE, FALSE, sizeof(double));
    for (const char *p = s; *p;) {
        gsize len = smil_number_span(p);
        if (!len) { p++; continue; }
        double v = smil_span_value(p, len);
        g_array_append_val(out, v);
        p += len;
    }
    return out;
}

static void
smil_add_value(smil_el *el, const char *text, gssize len)
{
    char *v = len < 0 ? g_strdup(text) : g_strndup(text, (gsize)len);
    g_ptr_array_add(el->values, g_strstrip(v));
}

static void
smil_split_values(smil_el *el, const char *list)
{
    const char *p = list;
    while (*p) {
        const char *end = strchr(p, ';');
        gsize len = end ? (gsize)(end - p) : strlen(p);
        gboolean blank = TRUE;
        for (gsize i = 0; i < len; i++)
            if (!g_ascii_isspace(p[i])) { blank = FALSE; break; }
        if (!blank) smil_add_value(el, p, (gssize)len);
        if (!end) break;
        p = end + 1;
    }
}

static void
smil_build_values(smil_el *el, const ns_node *n)
{
    const char *values = ns_element_get_attr(n, "values");
    const char *from = ns_element_get_attr(n, "from");
    const char *to = ns_element_get_attr(n, "to");
    const char *by = ns_element_get_attr(n, "by");
    if (el->kind == SMIL_SET) {
        if (to) smil_add_value(el, to, -1);
        return;
    }
    if (values) {
        smil_split_values(el, values);
        if (el->values->len) return;
    }
    if (to) {
        if (from) smil_add_value(el, from, -1);
        else el->to_anim = TRUE;
        smil_add_value(el, to, -1);
        return;
    }
    if (!by) return;
    if (from) {
        char *sum = smil_combine(from, by, 1.0, 1.0, el->is_color);
        smil_add_value(el, from, -1);
        smil_add_value(el, sum ? sum : by, -1);
        g_free(sum);
        return;
    }
    char *zero = smil_zero_like(by);
    if (zero) {
        smil_add_value(el, zero, -1);
        el->additive = TRUE;
    }
    smil_add_value(el, by, -1);
    g_free(zero);
}

static const char *
smil_event_name_start(const char *item)
{
    const char *start = item;
    for (const char *p = item; *p; p++)
        if (*p == '.' && g_ascii_isalpha(p[1])) start = p + 1;
    return start;
}

static gboolean
smil_parse_reference(const char *item, smil_begin *out)
{
    const char *name = smil_event_name_start(item);
    const char *end = name;
    while (g_ascii_isalpha(*end)) end++;
    if (end == name) return FALSE;
    const char *rest = smil_skip_ws(end);
    if (*rest && !smil_parse_clock(rest, &out->offset)) return FALSE;
    gsize id_len = name > item ? (gsize)(name - item - 1) : 0;
    gsize name_len = (gsize)(end - name);
    out->kind = SMIL_AT_EVENT;
    if (id_len && name_len == 5 && strncmp(name, "begin", 5) == 0)
        out->kind = SMIL_AT_SYNC_BEGIN;
    else if (id_len && name_len == 3 && strncmp(name, "end", 3) == 0)
        out->kind = SMIL_AT_SYNC_END;
    out->id = id_len ? g_strndup(item, id_len) : NULL;
    if (out->kind == SMIL_AT_EVENT) out->event = g_strndup(name, name_len);
    return TRUE;
}

static void
smil_parse_time_list(const char *list, GArray *offsets, GArray *specs)
{
    char **items = g_strsplit(list, ";", -1);
    for (char **it = items; *it; it++) {
        const char *item = g_strstrip(*it);
        smil_begin b = { SMIL_AT_OFFSET, 0, NULL, NULL };
        if (smil_parse_clock(item, &b.offset)) {
            if (offsets) g_array_append_val(offsets, b.offset);
            else         g_array_append_val(specs, b);
        } else if (smil_parse_reference(item, &b)) {
            g_array_append_val(specs, b);
        }
    }
    g_strfreev(items);
}

static void
smil_parse_begins(smil_el *el, const char *list)
{
    if (!list || !*smil_skip_ws(list)) {
        smil_begin b = { SMIL_AT_OFFSET, 0, NULL, NULL };
        g_array_append_val(el->begins, b);
        return;
    }
    smil_parse_time_list(list, NULL, el->begins);
}

static void
smil_parse_ends(smil_el *el, const char *list)
{
    if (list) smil_parse_time_list(list, el->ends, el->end_specs);
}

static double
smil_parse_repeat(const char *s, gboolean clock)
{
    if (!s) return -1;
    s = smil_skip_ws(s);
    if (g_str_has_prefix(s, "indefinite")) return INFINITY;
    double v;
    if (clock) return smil_parse_clock(s, &v) && v > 0 ? v : -1;
    v = g_ascii_strtod(s, NULL);
    return v > 0 ? v : -1;
}

static const ns_node *
smil_outer_svg(const ns_node *n)
{
    const ns_node *outer = NULL;
    for (const ns_node *p = n; p; p = p->parent)
        if (p->kind == NS_NODE_ELEMENT && p->name &&
            g_ascii_strcasecmp(p->name, "svg") == 0)
            outer = p;
    return outer;
}

static const char *
smil_href(const ns_node *n)
{
    const char *h = ns_element_get_attr(n, "href");
    return h ? h : ns_element_get_attr(n, "xlink:href");
}

static const ns_node *
smil_find_target(const ns_smil *s, const ns_node *n)
{
    const char *h = smil_href(n);
    if (h && *h == '#') return ns_node_find_by_id(s->doc, h + 1);
    const ns_node *p = n->parent;
    return p && p->kind == NS_NODE_ELEMENT ? p : NULL;
}

static void
smil_load_motion_path(const ns_smil *s, smil_el *el, const ns_node *n)
{
    const char *d = ns_element_get_attr(n, "path");
    for (const ns_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name ||
            g_ascii_strcasecmp(c->name, "mpath") != 0)
            continue;
        const char *h = smil_href(c);
        const ns_node *path = h && *h == '#'
            ? ns_node_find_by_id(s->doc, h + 1) : NULL;
        if (path) d = ns_element_get_attr(path, "d");
        break;
    }
    if (d) el->path = ns_svg_path_polyline(d);

    const char *rotate = ns_element_get_attr(n, "rotate");
    if (!rotate) return;
    rotate = smil_skip_ws(rotate);
    if (g_str_has_prefix(rotate, "auto-reverse")) {
        el->rotate_auto = TRUE;
        el->rotate_reverse = TRUE;
    } else if (g_str_has_prefix(rotate, "auto")) {
        el->rotate_auto = TRUE;
    } else {
        el->rotate = g_ascii_strtod(rotate, NULL);
    }
}

static gboolean
smil_attr_is_color(const char *attr)
{
    static const char *const colors[] = {
        "fill", "stroke", "color", "stop-color", "flood-color",
        "lighting-color",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(colors); i++)
        if (strcmp(attr, colors[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
smil_attr_is(const ns_node *n, const char *name, const char *value)
{
    const char *v = ns_element_get_attr(n, name);
    return v && g_ascii_strcasecmp(smil_skip_ws(v), value) == 0;
}

static void
smil_begin_clear(gpointer data)
{
    smil_begin *b = data;
    g_free(b->id);
    g_free(b->event);
}

static void
smil_el_free(gpointer data)
{
    smil_el *el = data;
    g_free(el->attr);
    g_free(el->type);
    g_free(el->begin_src);
    g_ptr_array_free(el->values, TRUE);
    if (el->key_times) g_array_free(el->key_times, TRUE);
    if (el->key_splines) g_array_free(el->key_splines, TRUE);
    if (el->key_points) g_array_free(el->key_points, TRUE);
    if (el->path) g_array_free(el->path, TRUE);
    g_array_free(el->begins, TRUE);
    g_array_free(el->ends, TRUE);
    g_array_free(el->end_specs, TRUE);
    g_array_free(el->end_instances, TRUE);
    g_array_free(el->instances, TRUE);
    g_free(el);
}

static gboolean
smil_kind_of(const char *name, smil_kind *out)
{
    static const struct { const char *tag; smil_kind kind; } kinds[] = {
        { "animate", SMIL_ANIMATE },
        { "set", SMIL_SET },
        { "animateTransform", SMIL_TRANSFORM },
        { "animateMotion", SMIL_MOTION },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(kinds); i++)
        if (g_ascii_strcasecmp(name, kinds[i].tag) == 0) {
            *out = kinds[i].kind;
            return TRUE;
        }
    return FALSE;
}

static smil_el *
smil_el_parse(const ns_smil *s, const ns_node *n, smil_kind kind)
{
    smil_el *el = g_new0(smil_el, 1);
    el->node = n;
    el->kind = kind;
    el->svg = smil_outer_svg(n);
    el->target = smil_find_target(s, n);
    el->values = g_ptr_array_new_with_free_func(g_free);
    el->begins = g_array_new(FALSE, FALSE, sizeof(smil_begin));
    g_array_set_clear_func(el->begins, smil_begin_clear);
    el->ends = g_array_new(FALSE, FALSE, sizeof(double));
    el->end_specs = g_array_new(FALSE, FALSE, sizeof(smil_begin));
    g_array_set_clear_func(el->end_specs, smil_begin_clear);
    el->end_instances = g_array_new(FALSE, FALSE, sizeof(double));
    el->instances = g_array_new(FALSE, FALSE, sizeof(double));
    el->horizon = -INFINITY;

    const char *attr = ns_element_get_attr(n, "attributeName");
    if (kind == SMIL_MOTION) attr = NS_SMIL_MOTION_ATTR;
    else if (kind == SMIL_TRANSFORM && !attr) attr = "transform";
    el->attr = attr ? g_strstrip(g_strdup(attr)) : NULL;
    el->is_color = el->attr && smil_attr_is_color(el->attr);
    if (kind == SMIL_TRANSFORM) {
        const char *type = ns_element_get_attr(n, "type");
        el->type = g_strstrip(g_strdup(type ? type : "translate"));
    }

    el->calc = kind == SMIL_MOTION ? SMIL_PACED : SMIL_LINEAR;
    if (smil_attr_is(n, "calcMode", "discrete"))    el->calc = SMIL_DISCRETE;
    else if (smil_attr_is(n, "calcMode", "linear")) el->calc = SMIL_LINEAR;
    else if (smil_attr_is(n, "calcMode", "paced"))  el->calc = SMIL_PACED;
    else if (smil_attr_is(n, "calcMode", "spline")) el->calc = SMIL_SPLINE;

    if (smil_attr_is(n, "restart", "whenNotActive"))
        el->restart = SMIL_RESTART_WHEN_IDLE;
    else if (smil_attr_is(n, "restart", "never"))
        el->restart = SMIL_RESTART_NEVER;

    el->additive = smil_attr_is(n, "additive", "sum");
    el->accumulate = smil_attr_is(n, "accumulate", "sum");
    el->freeze = smil_attr_is(n, "fill", "freeze");
    smil_build_values(el, n);
    if (kind == SMIL_MOTION) smil_load_motion_path(s, el, n);

    el->key_times = smil_parse_number_list(ns_element_get_attr(n, "keyTimes"));
    el->key_splines = smil_parse_number_list(ns_element_get_attr(n, "keySplines"));
    el->key_points = smil_parse_number_list(ns_element_get_attr(n, "keyPoints"));

    el->dur = -1;
    double dur;
    if (smil_parse_clock(ns_element_get_attr(n, "dur"), &dur) && dur > 0)
        el->dur = dur;
    el->repeat_count = smil_parse_repeat(ns_element_get_attr(n, "repeatCount"), FALSE);
    el->repeat_dur = smil_parse_repeat(ns_element_get_attr(n, "repeatDur"), TRUE);

    const char *begin = ns_element_get_attr(n, "begin");
    el->begin_src = g_strdup(begin ? begin : "");
    smil_parse_begins(el, begin);
    smil_parse_ends(el, ns_element_get_attr(n, "end"));
    return el;
}

static gboolean
smil_time_insert(GArray *times, double at, double horizon)
{
    if (!isfinite(at) || at <= horizon) return FALSE;
    guint i = 0;
    for (; i < times->len; i++) {
        double v = g_array_index(times, double, i);
        if (fabs(v - at) < SMIL_TIME_EPSILON) return FALSE;
        if (v > at) break;
    }
    g_array_insert_val(times, i, at);
    return TRUE;
}

static gboolean
smil_instance_add(smil_el *el, double at)
{
    return smil_time_insert(el->instances, at, el->horizon);
}

static void
smil_seed_instances(smil_el *el)
{
    for (guint i = 0; i < el->begins->len; i++) {
        const smil_begin *b = &g_array_index(el->begins, smil_begin, i);
        if (b->kind == SMIL_AT_OFFSET) smil_instance_add(el, b->offset);
    }
}

static void
smil_inherit_runtime(smil_el *el, smil_el *old)
{
    GArray *fresh = el->instances;
    el->instances = old->instances;
    old->instances = fresh;
    fresh = el->end_instances;
    el->end_instances = old->end_instances;
    old->end_instances = fresh;
    el->reported = old->reported;
    el->reported_end = old->reported_end;
    el->reported_begin = old->reported_begin;
    el->reported_iteration = old->reported_iteration;
    el->horizon = old->horizon;
    el->has_forced_end = old->has_forced_end;
    el->forced_begin = old->forced_begin;
    el->forced_end = old->forced_end;
}

static gboolean
smil_has_event_spec(const GArray *specs)
{
    for (guint i = 0; i < specs->len; i++)
        if (g_array_index(specs, smil_begin, i).kind == SMIL_AT_EVENT)
            return TRUE;
    return FALSE;
}

static void
smil_set_event_specs(ns_smil *s, gboolean has)
{
    if (s->has_event_specs == has) return;
    s->has_event_specs = has;
    if (has) g_smil_event_docs++;
    else     g_smil_event_docs--;
}

static int
smil_node_order(gconstpointer a, gconstpointer b)
{
    return ns_node_document_order_cmp(*(const ns_node *const *)a,
                                      *(const ns_node *const *)b);
}

static void
smil_prune_clocks(ns_smil *s)
{
    if (g_hash_table_size(s->clocks) == 0) return;
    GPtrArray *svgs = ns_doc_tag_index_lookup(s->doc, "svg");
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, s->clocks);
    while (g_hash_table_iter_next(&it, &key, NULL)) {
        gboolean live = FALSE;
        for (guint i = 0; svgs && i < svgs->len && !live; i++)
            live = g_ptr_array_index(svgs, i) == key;
        if (!live) g_hash_table_iter_remove(&it);
    }
}

static void
smil_scan(ns_smil *s)
{
    static const char *const tags[] = {
        "animate", "set", "animatetransform", "animatemotion",
    };
    ns_node *doc = (ns_node *)s->doc;
    if (!doc->tag_index) ns_doc_tag_index_build(doc);

    GPtrArray *nodes = g_ptr_array_new();
    for (gsize t = 0; t < G_N_ELEMENTS(tags); t++) {
        GPtrArray *list = ns_doc_tag_index_lookup(doc, tags[t]);
        for (guint i = 0; list && i < list->len; i++) {
            ns_node *n = g_ptr_array_index(list, i);
            if (smil_outer_svg(n)) g_ptr_array_add(nodes, n);
        }
    }
    if (nodes->len > 1) g_ptr_array_sort(nodes, smil_node_order);

    GPtrArray *els = g_ptr_array_new_with_free_func(smil_el_free);
    GHashTable *by_node = g_hash_table_new(g_direct_hash, g_direct_equal);
    gboolean event_specs = FALSE;
    for (guint i = 0; i < nodes->len; i++) {
        const ns_node *n = g_ptr_array_index(nodes, i);
        smil_kind kind;
        if (!smil_kind_of(n->name, &kind)) continue;
        smil_el *el = smil_el_parse(s, n, kind);
        el->order = els->len;
        smil_el *old = g_hash_table_lookup(s->by_node, n);
        if (old && strcmp(old->begin_src, el->begin_src) == 0)
            smil_inherit_runtime(el, old);
        else
            smil_seed_instances(el);
        g_ptr_array_add(els, el);
        g_hash_table_insert(by_node, (gpointer)n, el);
        event_specs |= smil_has_event_spec(el->begins) ||
                       smil_has_event_spec(el->end_specs);
    }
    g_ptr_array_free(nodes, TRUE);
    smil_set_event_specs(s, event_specs);

    g_hash_table_destroy(s->by_node);
    g_ptr_array_free(s->els, TRUE);
    s->els = els;
    s->by_node = by_node;
    s->scanned = TRUE;
    smil_prune_clocks(s);
}

static double
smil_time_at(const ns_smil *s, const ns_node *svg, gint64 now_us)
{
    const smil_clock *c = svg ? g_hash_table_lookup(s->clocks, svg) : NULL;
    gint64 at = now_us, offset = 0;
    if (c) {
        if (c->paused) at = c->paused_at_us;
        offset = c->offset_us;
    }
    return (double)(at - s->origin_us - offset) / 1e6;
}

static gboolean
smil_clock_paused(const ns_smil *s, const ns_node *svg)
{
    const smil_clock *c = svg ? g_hash_table_lookup(s->clocks, svg) : NULL;
    return c && c->paused;
}

static double
smil_active_duration(const smil_el *el)
{
    if (el->dur < 0) return el->repeat_dur >= 0 ? el->repeat_dur : INFINITY;
    double ad = el->dur;
    if (el->repeat_count >= 0) ad = el->dur * el->repeat_count;
    if (el->repeat_dur >= 0)
        ad = el->repeat_count >= 0 ? MIN(ad, el->repeat_dur) : el->repeat_dur;
    return ad;
}

static double
smil_interval_end(const smil_el *el, double begin)
{
    double end = begin + smil_active_duration(el);
    for (guint i = 0; i < el->ends->len; i++) {
        double v = g_array_index(el->ends, double, i);
        if (v >= begin && v < end) end = v;
    }
    for (guint i = 0; i < el->end_instances->len; i++) {
        double v = g_array_index(el->end_instances, double, i);
        if (v >= begin && v < end) end = v;
    }
    if (el->has_forced_end &&
        fabs(el->forced_begin - begin) < SMIL_TIME_EPSILON &&
        el->forced_end < end)
        end = el->forced_end;
    return end;
}

static gboolean
smil_current_begin(const smil_el *el, double t, double *out)
{
    gboolean found = FALSE;
    double cur = 0;
    for (guint i = 0; i < el->instances->len; i++) {
        double b = g_array_index(el->instances, double, i);
        if (b > t) break;
        if (!found) { cur = b; found = TRUE; continue; }
        if (el->restart == SMIL_RESTART_NEVER) break;
        if (el->restart == SMIL_RESTART_WHEN_IDLE &&
            b < smil_interval_end(el, cur))
            continue;
        cur = b;
    }
    if (found) *out = cur;
    return found;
}

static gboolean
smil_has_future_begin(const smil_el *el, double t)
{
    if (el->instances->len == 0) return FALSE;
    return g_array_index(el->instances, double, el->instances->len - 1) > t;
}

static smil_el *
smil_el_by_id(const ns_smil *s, const char *id)
{
    const ns_node *n = ns_node_find_by_id(s->doc, id);
    return n ? g_hash_table_lookup(s->by_node, n) : NULL;
}

static gboolean
smil_sync_from(const ns_smil *s, smil_el *el, const smil_begin *spec, double t,
               GArray *into)
{
    if (spec->kind != SMIL_AT_SYNC_BEGIN && spec->kind != SMIL_AT_SYNC_END)
        return FALSE;
    const smil_el *base = smil_el_by_id(s, spec->id);
    if (!base || base == el) return FALSE;
    gboolean grew = FALSE;
    for (guint i = 0; i < base->instances->len; i++) {
        double at = g_array_index(base->instances, double, i);
        if (at > t) break;
        if (spec->kind == SMIL_AT_SYNC_END) at = smil_interval_end(base, at);
        at += spec->offset;
        if (at <= t && smil_time_insert(into, at, el->horizon)) grew = TRUE;
    }
    return grew;
}

static gboolean
smil_sync_specs(const ns_smil *s, smil_el *el, const GArray *specs, double t,
                GArray *into)
{
    gboolean grew = FALSE;
    for (guint k = 0; k < specs->len; k++)
        if (smil_sync_from(s, el, &g_array_index(specs, smil_begin, k), t, into))
            grew = TRUE;
    return grew;
}

static void
smil_prune_times(GArray *times, double t)
{
    guint past = 0;
    while (past < times->len && g_array_index(times, double, past) <= t) past++;
    if (past > SMIL_KEPT_INSTANCES)
        g_array_remove_range(times, 0, past - SMIL_KEPT_INSTANCES);
}

static void
smil_prune_instances(smil_el *el, double t)
{
    guint past = 0;
    while (past < el->instances->len &&
           g_array_index(el->instances, double, past) <= t)
        past++;
    if (past <= SMIL_KEPT_INSTANCES) return;
    guint drop = past - SMIL_KEPT_INSTANCES;
    el->horizon = g_array_index(el->instances, double, drop - 1);
    g_array_remove_range(el->instances, 0, drop);
}

static void
smil_resolve_sync(ns_smil *s)
{
    for (int pass = 0; pass < SMIL_SYNC_PASSES; pass++) {
        gboolean grew = FALSE;
        for (guint i = 0; i < s->els->len; i++) {
            smil_el *el = g_ptr_array_index(s->els, i);
            double t = smil_time_at(s, el->svg, s->now_us);
            if (smil_sync_specs(s, el, el->begins, t, el->instances)) grew = TRUE;
            if (smil_sync_specs(s, el, el->end_specs, t, el->end_instances))
                grew = TRUE;
        }
        if (!grew) break;
    }
    for (guint i = 0; i < s->els->len; i++) {
        smil_el *el = g_ptr_array_index(s->els, i);
        double t = smil_time_at(s, el->svg, s->now_us);
        smil_prune_instances(el, t);
        smil_prune_times(el->end_instances, t);
    }
}

static int
smil_running_order(gconstpointer a, gconstpointer b)
{
    const smil_running *ra = a, *rb = b;
    if (ra->begin < rb->begin) return -1;
    if (ra->begin > rb->begin) return 1;
    return ra->el->order < rb->el->order ? -1 : ra->el->order > rb->el->order;
}

static void
smil_queue_event(ns_smil *s, const smil_el *el, ns_smil_event_kind kind,
                 double iteration)
{
    smil_event ev = { el->node, kind, (int)MIN(iteration, (double)G_MAXINT) };
    if (s->events->len < SMIL_MAX_EVENTS) g_array_append_val(s->events, ev);
}

static void
smil_note_progress(ns_smil *s, smil_el *el, double t)
{
    double begin;
    if (!smil_current_begin(el, t, &begin)) return;
    if (!el->reported || fabs(el->reported_begin - begin) > SMIL_TIME_EPSILON) {
        if (el->reported && !el->reported_end)
            smil_queue_event(s, el, NS_SMIL_EVENT_END, 0);
        smil_queue_event(s, el, NS_SMIL_EVENT_BEGIN, 0);
        el->reported = TRUE;
        el->reported_end = FALSE;
        el->reported_begin = begin;
        el->reported_iteration = 0;
    }
    if (el->reported_end) return;
    if (t >= smil_interval_end(el, begin)) {
        smil_queue_event(s, el, NS_SMIL_EVENT_END, 0);
        el->reported_end = TRUE;
        return;
    }
    if (el->dur <= 0) return;
    double iteration = floor((t - begin) / el->dur);
    if (iteration > el->reported_iteration) {
        el->reported_iteration = iteration;
        smil_queue_event(s, el, NS_SMIL_EVENT_REPEAT, iteration);
    }
}

static GArray *
smil_collect_running(ns_smil *s)
{
    GArray *running = g_array_new(FALSE, FALSE, sizeof(smil_running));
    s->active = FALSE;
    for (guint i = 0; i < s->els->len; i++) {
        smil_el *el = g_ptr_array_index(s->els, i);
        double t = smil_time_at(s, el->svg, s->now_us);
        smil_note_progress(s, el, t);
        if (!el->target || !el->attr) continue;
        gboolean ticking = !smil_clock_paused(s, el->svg);
        if (ticking && smil_has_future_begin(el, t)) s->active = TRUE;
        smil_running r = { .el = el };
        if (!smil_current_begin(el, t, &r.begin)) continue;
        double end = smil_interval_end(el, r.begin);
        r.ended = t >= end;
        if (r.ended && !el->freeze) continue;
        if (!r.ended && ticking) s->active = TRUE;
        r.active_time = (r.ended ? end : t) - r.begin;
        g_array_append_val(running, r);
    }
    g_array_sort(running, smil_running_order);
    return running;
}

static void
smil_override_free(gpointer data)
{
    smil_override *ov = data;
    g_free(ov->name);
    g_free(ov->value);
    g_free(ov);
}

static void
smil_override_list_free(gpointer data)
{
    g_ptr_array_free(data, TRUE);
}

static smil_override *
smil_override_get(GHashTable *table, const ns_node *node, const char *name,
                  gboolean create)
{
    GPtrArray *list = g_hash_table_lookup(table, node);
    for (guint i = 0; list && i < list->len; i++) {
        smil_override *ov = g_ptr_array_index(list, i);
        if (strcmp(ov->name, name) == 0) return ov;
    }
    if (!create) return NULL;
    if (!list) {
        list = g_ptr_array_new_with_free_func(smil_override_free);
        g_hash_table_insert(table, (gpointer)node, list);
    }
    smil_override *ov = g_new0(smil_override, 1);
    ov->node = node;
    ov->name = g_strdup(name);
    g_ptr_array_add(list, ov);
    return ov;
}

static GHashTable *
smil_overrides_new(void)
{
    return g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                 smil_override_list_free);
}

static void
smil_apply_running(const smil_running *r, GHashTable *table, GString *sig)
{
    const smil_el *el = r->el;
    smil_override *ov = smil_override_get(table, el->target, el->attr, FALSE);
    const char *under = ov ? ov->value
        : el->kind == SMIL_MOTION ? NULL
        : ns_element_get_attr(el->target, el->attr);
    char *value = smil_eval(el, r->active_time, r->ended, under);
    if (!value) return;
    if (!ov) ov = smil_override_get(table, el->target, el->attr, TRUE);
    g_free(ov->value);
    ov->value = value;
    g_string_append_printf(sig, "%p %s=%s\n", (const void *)el->target,
                           el->attr, value);
}

static void
smil_swap_overrides(ns_smil *s, GHashTable *next)
{
    g_smil_override_nodes -= g_hash_table_size(s->overrides);
    g_hash_table_destroy(s->overrides);
    s->overrides = next;
    g_smil_override_nodes += g_hash_table_size(next);
}

gboolean
ns_smil_tick(ns_smil *s, guint64 dom_serial, gint64 now_us)
{
    if (!s) return FALSE;
    if (!s->scanned || dom_serial != s->serial) {
        smil_scan(s);
        s->serial = dom_serial;
    }
    s->now_us = now_us;
    s->dirty = FALSE;
    if (s->els->len == 0 && g_hash_table_size(s->overrides) == 0) {
        s->active = FALSE;
        return FALSE;
    }
    smil_resolve_sync(s);
    GArray *running = smil_collect_running(s);
    GHashTable *next = smil_overrides_new();
    GString *sig = g_string_new(NULL);
    for (guint i = 0; i < running->len; i++)
        smil_apply_running(&g_array_index(running, smil_running, i), next, sig);
    g_array_free(running, TRUE);

    gboolean changed = s->unreported ||
                       g_strcmp0(sig->str, s->signature) != 0;
    s->unreported = FALSE;
    g_free(s->signature);
    s->signature = g_string_free(sig, FALSE);
    smil_swap_overrides(s, next);
    return changed;
}

gboolean
ns_smil_has_active(const ns_smil *s)
{
    return s && (s->active || s->dirty);
}

ns_smil *
ns_smil_new(const ns_node *doc, gint64 now_us)
{
    if (!doc) return NULL;
    ns_smil *s = g_new0(ns_smil, 1);
    s->doc = doc;
    s->origin_us = now_us;
    s->now_us = now_us;
    s->els = g_ptr_array_new_with_free_func(smil_el_free);
    s->by_node = g_hash_table_new(g_direct_hash, g_direct_equal);
    s->clocks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    s->overrides = smil_overrides_new();
    s->events = g_array_new(FALSE, FALSE, sizeof(smil_event));
    s->dirty = TRUE;
    if (!g_smils) g_smils = g_ptr_array_new();
    g_ptr_array_add(g_smils, s);
    return s;
}

void
ns_smil_free(ns_smil *s)
{
    if (!s) return;
    if (g_smils) g_ptr_array_remove_fast(g_smils, s);
    smil_set_event_specs(s, FALSE);
    g_array_free(s->events, TRUE);
    g_smil_override_nodes -= g_hash_table_size(s->overrides);
    g_hash_table_destroy(s->overrides);
    g_hash_table_destroy(s->clocks);
    g_hash_table_destroy(s->by_node);
    g_ptr_array_free(s->els, TRUE);
    g_free(s->signature);
    g_free(s);
}

const char *
ns_smil_attr(const ns_node *n, const char *name)
{
    if (!g_smil_override_nodes || !g_smils) return NULL;
    for (guint i = 0; i < g_smils->len; i++) {
        const ns_smil *s = g_ptr_array_index(g_smils, i);
        smil_override *ov = smil_override_get(s->overrides, n, name, FALSE);
        if (ov) return ov->value;
    }
    return NULL;
}

gboolean
ns_smil_node_animated(const ns_node *n)
{
    if (!g_smil_override_nodes || !g_smils) return FALSE;
    for (guint i = 0; i < g_smils->len; i++) {
        const ns_smil *s = g_ptr_array_index(g_smils, i);
        if (g_hash_table_contains(s->overrides, n)) return TRUE;
    }
    return FALSE;
}

static ns_smil *
smil_for_node(const ns_node *n)
{
    if (!n || !g_smils) return NULL;
    const ns_node *root = ns_node_root(n);
    for (guint i = 0; i < g_smils->len; i++) {
        ns_smil *s = g_ptr_array_index(g_smils, i);
        if (s->doc == root) return s;
    }
    return NULL;
}

void
ns_smil_refresh(const ns_node *n)
{
    ns_smil *s = smil_for_node(n);
    if (!s || !s->dirty) return;
    if (ns_smil_tick(s, s->serial, g_get_monotonic_time())) {
        s->unreported = TRUE;
        s->dirty = TRUE;
    }
}

void
ns_smil_drain_events(ns_smil *s, ns_smil_event_cb cb, gpointer user)
{
    if (!s || s->events->len == 0) return;
    GArray *events = s->events;
    s->events = g_array_new(FALSE, FALSE, sizeof(smil_event));
    gboolean script_ran = FALSE;
    for (guint i = 0; i < events->len; i++) {
        const smil_event *ev = &g_array_index(events, smil_event, i);
        if (script_ran) smil_scan(s);
        script_ran = FALSE;
        if (g_hash_table_contains(s->by_node, ev->node))
            script_ran = cb(ev->node, ev->kind, ev->iteration, user);
    }
    g_array_free(events, TRUE);
}

const char *
ns_smil_event_type(ns_smil_event_kind kind)
{
    static const char *const types[] = { "beginEvent", "endEvent", "repeatEvent" };
    return types[kind];
}

static gboolean
smil_settle_event(const ns_node *node, ns_smil_event_kind kind, int iteration,
                  gpointer user)
{
    (void)iteration; (void)user;
    ns_smil_dom_event(node, ns_smil_event_type(kind));
    return FALSE;
}

void
ns_smil_settle_events(ns_smil *s)
{
    ns_smil_drain_events(s, smil_settle_event, NULL);
}

static gboolean
smil_event_bubbles(const char *type)
{
    static const char *const stays[] = {
        "mouseenter", "mouseleave", "focus", "blur", "load", "beginEvent",
        "endEvent", "repeatEvent",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(stays); i++)
        if (strcmp(type, stays[i]) == 0) return FALSE;
    return TRUE;
}

static gboolean
smil_event_reaches(const ns_node *base, const ns_node *target, gboolean bubbles)
{
    if (!base) return FALSE;
    if (base == target) return TRUE;
    for (const ns_node *p = bubbles ? target->parent : NULL; p; p = p->parent)
        if (p == base) return TRUE;
    return FALSE;
}

static gboolean
smil_event_matches(const ns_smil *s, const smil_el *el, const smil_begin *spec,
                   const ns_node *target, const char *type)
{
    if (spec->kind != SMIL_AT_EVENT) return FALSE;
    const char *want = strcmp(spec->event, "activate") == 0 ? "click"
                                                             : spec->event;
    if (strcmp(want, type) != 0) return FALSE;
    const ns_node *base = spec->id ? ns_node_find_by_id(s->doc, spec->id)
                                   : el->target;
    return smil_event_reaches(base, target, smil_event_bubbles(type));
}

void
ns_smil_dom_event(const ns_node *target, const char *type)
{
    if (!g_smil_event_docs || !target || !type) return;
    ns_smil *s = smil_for_node(target);
    if (!s || !s->has_event_specs) return;
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < s->els->len; i++) {
        smil_el *el = g_ptr_array_index(s->els, i);
        double t = smil_time_at(s, el->svg, now);
        for (guint k = 0; k < el->begins->len; k++) {
            const smil_begin *spec = &g_array_index(el->begins, smil_begin, k);
            if (smil_event_matches(s, el, spec, target, type) &&
                smil_instance_add(el, t + spec->offset))
                s->dirty = TRUE;
        }
        for (guint k = 0; k < el->end_specs->len; k++) {
            const smil_begin *spec = &g_array_index(el->end_specs, smil_begin, k);
            if (smil_event_matches(s, el, spec, target, type) &&
                smil_time_insert(el->end_instances, t + spec->offset, -INFINITY))
                s->dirty = TRUE;
        }
    }
}

static smil_clock *
smil_clock_for(ns_smil *s, const ns_node *svg)
{
    smil_clock *c = g_hash_table_lookup(s->clocks, svg);
    if (c) return c;
    c = g_new0(smil_clock, 1);
    g_hash_table_insert(s->clocks, (gpointer)svg, c);
    return c;
}

void
ns_smil_pause(const ns_node *svg, gboolean paused)
{
    ns_smil *s = smil_for_node(svg);
    const ns_node *outer = smil_outer_svg(svg);
    if (!s || !outer) return;
    smil_clock *c = smil_clock_for(s, outer);
    if (c->paused == paused) return;
    gint64 now = g_get_monotonic_time();
    if (paused) c->paused_at_us = now;
    else        c->offset_us += now - c->paused_at_us;
    c->paused = paused;
    s->dirty = TRUE;
}

gboolean
ns_smil_paused(const ns_node *svg)
{
    ns_smil *s = smil_for_node(svg);
    return s && smil_clock_paused(s, smil_outer_svg(svg));
}

double
ns_smil_current_time(const ns_node *el)
{
    ns_smil *s = smil_for_node(el);
    if (!s) return 0;
    return smil_time_at(s, smil_outer_svg(el), g_get_monotonic_time());
}

void
ns_smil_set_current_time(const ns_node *svg, double seconds)
{
    ns_smil *s = smil_for_node(svg);
    const ns_node *outer = smil_outer_svg(svg);
    if (!s || !outer || !isfinite(seconds)) return;
    smil_clock *c = smil_clock_for(s, outer);
    gint64 at = c->paused ? c->paused_at_us : g_get_monotonic_time();
    c->offset_us = at - s->origin_us - (gint64)(MAX(seconds, 0.0) * 1e6);
    s->dirty = TRUE;
}

static smil_el *
smil_el_for(const ns_node *anim, ns_smil **out_s)
{
    ns_smil *s = smil_for_node(anim);
    if (!s) return NULL;
    smil_scan(s);
    *out_s = s;
    return g_hash_table_lookup(s->by_node, anim);
}

gboolean
ns_smil_begin_element(const ns_node *anim, double offset)
{
    ns_smil *s;
    smil_el *el = smil_el_for(anim, &s);
    if (!el || !isfinite(offset)) return FALSE;
    double t = smil_time_at(s, el->svg, g_get_monotonic_time());
    smil_instance_add(el, t + offset);
    s->dirty = TRUE;
    return TRUE;
}

gboolean
ns_smil_end_element(const ns_node *anim, double offset)
{
    ns_smil *s;
    smil_el *el = smil_el_for(anim, &s);
    if (!el || !isfinite(offset)) return FALSE;
    double t = smil_time_at(s, el->svg, g_get_monotonic_time());
    double begin;
    if (!smil_current_begin(el, t, &begin)) return FALSE;
    el->has_forced_end = TRUE;
    el->forced_begin = begin;
    el->forced_end = t + offset;
    s->dirty = TRUE;
    return TRUE;
}

gboolean
ns_smil_start_time(const ns_node *anim, double *out)
{
    ns_smil *s;
    smil_el *el = smil_el_for(anim, &s);
    if (!el) return FALSE;
    double t = smil_time_at(s, el->svg, g_get_monotonic_time());
    if (smil_current_begin(el, t, out) && t < smil_interval_end(el, *out))
        return TRUE;
    for (guint i = 0; i < el->instances->len; i++) {
        double b = g_array_index(el->instances, double, i);
        if (b > t) { *out = b; return TRUE; }
    }
    return FALSE;
}

gboolean
ns_smil_simple_duration(const ns_node *anim, double *out)
{
    ns_smil *s;
    smil_el *el = smil_el_for(anim, &s);
    if (!el || el->dur < 0) return FALSE;
    *out = el->dur;
    return TRUE;
}

const ns_node *
ns_smil_target(const ns_node *anim)
{
    ns_smil *s = smil_for_node(anim);
    return s ? smil_find_target(s, anim) : NULL;
}
