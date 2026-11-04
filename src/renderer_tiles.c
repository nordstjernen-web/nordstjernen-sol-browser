/* Nordstjernen: paint a page into shared-memory tiles for the window's compositor. */

#include "renderer_tiles.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ipc_http.h"
#include "layers.h"

#define NS_TILE_PREFETCH_MAX  4
#define NS_TILE_PREFETCH_US   8000
#define NS_TILE_LIST_MAX      128
#define NS_TILE_SCROLLERS_MAX 96

struct ns_tiles {
    int                 state;
    int                 gen;
    int                 width;
    int                 vh;
    int                 tile_h;
    long                sx;
    double              scale;
    ns_paint_layer_plan plan;
    GHashTable         *sent;
    GArray             *vp_sent;
};

typedef struct tiles_req {
    long tile_h;
    long want_y0, want_y1;
    long gen;
    long vp_held;
    long have[NS_TILE_LIST_MAX];
    int  n_have;
    long hold[NS_TILE_LIST_MAX];
    int  n_hold;
} tiles_req;

typedef struct tiles_out {
    GString       *desc;
    unsigned char *fb;
    size_t         off;
    size_t         cap;
    size_t         row_bytes;
    int            width;
} tiles_out;

typedef struct tiles_rect {
    int x, y, w, h;
} tiles_rect;

ns_tiles *
ns_tiles_new(void)
{
    ns_tiles *t = g_new0(ns_tiles, 1);
    ns_paint_layer_plan_init(&t->plan);
    t->sent = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                    g_free);
    t->vp_sent = g_array_new(FALSE, TRUE, sizeof(guint64));
    return t;
}

void
ns_tiles_free(ns_tiles *t)
{
    if (!t) return;
    ns_paint_layer_plan_clear(&t->plan);
    g_hash_table_destroy(t->sent);
    g_array_free(t->vp_sent, TRUE);
    g_free(t);
}

gboolean
ns_tiles_requested(const char *body)
{
    long on = 0;
    json_get_long(body, "tiles", &on);
    return on != 0;
}

static int
tiles_parse_list(const char *body, const char *key, long *out)
{
    char *list = json_get_str(body, key);
    int n = 0;
    for (char *p = list; p && *p && n < NS_TILE_LIST_MAX;) {
        char *end = NULL;
        long k = strtol(p, &end, 10);
        if (end == p) break;
        out[n++] = k;
        p = *end == ',' ? end + 1 : end;
    }
    free(list);
    return n;
}

static gboolean
tiles_req_parse(const char *body, tiles_req *r)
{
    memset(r, 0, sizeof *r);
    r->gen = -1;
    r->vp_held = -1;
    json_get_long(body, "tile_h", &r->tile_h);
    json_get_long(body, "want_y0", &r->want_y0);
    json_get_long(body, "want_y1", &r->want_y1);
    json_get_long(body, "gen", &r->gen);
    json_get_long(body, "vp_held", &r->vp_held);
    r->n_have = tiles_parse_list(body, "have", r->have);
    r->n_hold = tiles_parse_list(body, "hold", r->hold);
    return r->tile_h >= 64 && r->tile_h <= 2048;
}

static gboolean
tiles_list_has(const long *list, int n, long k)
{
    for (int i = 0; i < n; i++)
        if (list[i] == k) return TRUE;
    return FALSE;
}

static gboolean
tiles_geometry_changed(const ns_tiles *t, const ns_tiles_view *v,
                       const tiles_req *r)
{
    return t->width != v->vw || t->vh != v->vh ||
           t->tile_h != (int)r->tile_h || t->sx != v->sx ||
           t->scale != v->scale;
}

static gboolean
tiles_plan_usable(ns_tiles *t, ns_browser *b)
{
    for (guint j = 0; j < t->plan.vp->len; j++) {
        ns_vp_layer_info info;
        if (ns_browser_vp_layer_info(b, &t->plan, (int)j, &info) != 0)
            return FALSE;
    }
    return TRUE;
}

static void
tiles_prepare(ns_tiles *t, ns_browser *b, const ns_tiles_view *v,
              const tiles_req *r, gboolean invalid)
{
    gboolean geometry = tiles_geometry_changed(t, v, r);
    if (t->state != 0 && !invalid && !geometry) {
        ns_browser_note_viewport(b, (int)v->sx, (int)v->sy, v->vh, v->scale);
        return;
    }
    guint old_vp = t->plan.vp->len;
    t->gen++;
    ns_browser_set_video_layer_gen(b, (guint)t->gen);
    t->width = v->vw;
    t->vh = v->vh;
    t->tile_h = (int)r->tile_h;
    t->sx = v->sx;
    t->scale = v->scale;
    int rc = ns_browser_layers_prepare(b, (int)v->sx, (int)v->sy, v->vw,
                                       v->vh, v->scale, &t->plan);
    t->state = rc == 0 && tiles_plan_usable(t, b) ? 1 : -1;
    if (geometry || old_vp != t->plan.vp->len) {
        g_hash_table_remove_all(t->sent);
        g_array_set_size(t->vp_sent, 0);
    }
    g_array_set_size(t->vp_sent, t->plan.vp->len);
}

static guint64
tiles_hash(const unsigned char *p, size_t n, guint64 h)
{
    guint64 lane[4] = { h, h ^ 0x9e3779b97f4a7c15ULL,
                        h ^ 0xc2b2ae3d27d4eb4fULL, h ^ 0x165667b19e3779f9ULL };
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int l = 0; l < 4; l++) {
            guint64 w;
            memcpy(&w, p + i + (size_t)l * 8, 8);
            lane[l] = (lane[l] ^ w) * 0x9e3779b97f4a7c15ULL;
            lane[l] ^= lane[l] >> 29;
        }
    for (; i < n; i++)
        lane[0] = (lane[0] ^ p[i]) * 0x100000001b3ULL;
    return lane[0] ^ (lane[1] * 3) ^ (lane[2] * 5) ^ (lane[3] * 7);
}

static gboolean
tiles_row_clear(const guint32 *px, int x0, int x1)
{
    guint32 acc = 0;
    for (int x = x0; x < x1; x++) acc |= px[x];
    return acc == 0;
}

static int
tiles_row_first(const guint32 *px, int width, int limit)
{
    for (int x = 0; x < limit && x < width; x++)
        if (px[x]) return x;
    return limit;
}

static int
tiles_row_last(const guint32 *px, int width, int limit)
{
    for (int x = width - 1; x >= limit; x--)
        if (px[x]) return x + 1;
    return limit;
}

static tiles_rect
tiles_crop(const unsigned char *buf, int width, int rows, size_t row_bytes)
{
    tiles_rect r = { 0, 0, 0, 0 };
    const guint32 *px = (const guint32 *)(const void *)buf;
    size_t stride = row_bytes / 4;
    int top = 0, bottom = rows;
    while (top < rows && tiles_row_clear(px + (size_t)top * stride, 0, width))
        top++;
    while (bottom > top &&
           tiles_row_clear(px + (size_t)(bottom - 1) * stride, 0, width))
        bottom--;
    if (top == bottom) return r;
    int left = width, right = 0;
    for (int y = top; y < bottom; y++) {
        const guint32 *row = px + (size_t)y * stride;
        left = tiles_row_first(row, width, left);
        right = tiles_row_last(row, width, right);
    }
    r.x = left;
    r.y = top;
    r.w = right - left;
    r.h = bottom - top;
    return r;
}

static gboolean
tiles_out_fits(const tiles_out *o, size_t rows)
{
    return o->off + rows * o->row_bytes <= o->cap;
}

static size_t
tiles_out_pack(tiles_out *o, size_t src, tiles_rect r, guint64 *hash)
{
    size_t at = o->off;
    size_t line = (size_t)r.w * 4u;
    for (int y = 0; y < r.h; y++) {
        const unsigned char *from = o->fb + src +
                                    (size_t)(r.y + y) * o->row_bytes +
                                    (size_t)r.x * 4u;
        unsigned char *to = o->fb + at + (size_t)y * line;
        if (from != to) memmove(to, from, line);
    }
    o->off += (size_t)r.h * line;
    o->off = (o->off + 15) & ~(size_t)15;
    guint64 meta[4] = { (guint64)r.x, (guint64)r.y, (guint64)r.w,
                        (guint64)r.h };
    *hash = tiles_hash(o->fb + at, (size_t)r.h * line,
                       tiles_hash((const unsigned char *)meta, sizeof meta,
                                  *hash));
    return at;
}

static void
tiles_sticky_desc(GString *out, const ns_vp_layer_info *info)
{
    const ns_sticky_y *m = &info->sticky;
    g_string_append_printf(out, " %.3f %d %.3f %.3f %d %.3f %.3f\n",
                           info->x_offset, m->has_top ? 1 : 0, m->top_start,
                           m->top_cap, m->has_bottom ? 1 : 0,
                           m->bottom_start, m->bottom_cap);
}

static void
tiles_write_vp_layer(ns_tiles *t, ns_browser *b, const ns_tiles_view *v,
                     const tiles_req *r, tiles_out *o, guint j)
{
    ns_vp_layer_info info;
    ns_browser_vp_layer_info(b, &t->plan, (int)j, &info);
    int origin = (int)floor(info.top * v->scale);
    int rows = (int)ceil(info.bottom * v->scale) - origin;
    size_t src = o->off;
    tiles_rect rect = { 0, 0, 0, 0 };
    if (rows > 0 && tiles_out_fits(o, (size_t)rows) &&
        ns_browser_render_vp_layer(b, &t->plan, (int)j, (int)v->sx,
                                   (int)v->sy, origin, v->vw, rows, v->scale,
                                   o->fb + src, (int)o->row_bytes) == 0)
        rect = tiles_crop(o->fb + src, v->vw, rows, o->row_bytes);
    guint64 hash = 0x51ed27;
    size_t at = tiles_out_pack(o, src, rect, &hash);
    guint64 *sent = &g_array_index(t->vp_sent, guint64, j);
    gboolean keep = r->vp_held == (long)t->plan.vp->len && *sent == hash;
    if (keep) o->off = src;
    *sent = hash;
    g_string_append_printf(o->desc, "vp %u %d %d %d %d %d %d %lld", j,
                           info.kind, origin, rect.x, rect.y, rect.w, rect.h,
                           keep ? -1LL : (long long)at);
    tiles_sticky_desc(o->desc, &info);
}

static void
tiles_write_upper(tiles_out *o, GString *lines, long k, int layer,
                  size_t src, int th, guint64 *hash)
{
    tiles_rect r = tiles_crop(o->fb + src, o->width, th, o->row_bytes);
    if (r.w <= 0 || r.h <= 0) return;
    size_t at = tiles_out_pack(o, src, r, hash);
    g_string_append_printf(lines, "tile %ld %d %d %d %d %d %zu\n", k, layer,
                           r.x, r.y, r.w, r.h, at);
}

static gboolean
tiles_paint_one(ns_tiles *t, ns_browser *b, const ns_tiles_view *v,
                const tiles_req *r, tiles_out *o, long k)
{
    int n_upper = (int)t->plan.vp->len;
    int th = t->tile_h;
    if (!tiles_out_fits(o, (size_t)th * (size_t)(n_upper + 1))) return FALSE;
    size_t start = o->off;
    unsigned char **bufs = g_new(unsigned char *, n_upper + 1);
    gboolean *used = g_new0(gboolean, MAX(n_upper, 1));
    for (int i = 0; i <= n_upper; i++)
        bufs[i] = o->fb + start + (size_t)i * th * o->row_bytes;
    int rc = ns_browser_render_doc_tile(b, &t->plan, (int)v->sx,
                                        (int)(k * th), v->vw, th, v->scale,
                                        bufs, (int)o->row_bytes, used);
    if (rc == -2) t->state = -1;
    if (rc == 0) {
        GString *lines = g_string_new(NULL);
        guint64 hash = 0x7e57;
        tiles_rect full = { 0, 0, v->vw, th };
        size_t at = tiles_out_pack(o, start, full, &hash);
        g_string_append_printf(lines, "tile %ld 0 0 0 %d %d %zu\n", k, v->vw,
                               th, at);
        for (int i = 0; i < n_upper; i++)
            if (used[i])
                tiles_write_upper(o, lines, k, i + 1,
                                  start + (size_t)(i + 1) * th * o->row_bytes,
                                  th, &hash);
        gint64 key = k;
        guint64 *sent = g_hash_table_lookup(t->sent, &key);
        if (sent && *sent == hash && tiles_list_has(r->hold, r->n_hold, k)) {
            o->off = start;
            g_string_append_printf(o->desc, "keep %ld\n", k);
        } else {
            g_string_append(o->desc, lines->str);
            g_hash_table_replace(t->sent, g_memdup2(&key, sizeof key),
                                 g_memdup2(&hash, sizeof hash));
        }
        g_string_free(lines, TRUE);
    }
    g_free(bufs);
    g_free(used);
    return rc == 0;
}

static long
tiles_distance(long k, long v0, long v1)
{
    return k < v0 ? v0 - k : k > v1 ? k - v1 : 0;
}

static gboolean
tiles_budget_left(int prefetched, gint64 t0)
{
    return prefetched < NS_TILE_PREFETCH_MAX &&
           g_get_monotonic_time() - t0 <= NS_TILE_PREFETCH_US;
}

static gboolean
tiles_wanted(const tiles_req *r, gboolean fresh, long k, long distance)
{
    if (!fresh && tiles_list_has(r->have, r->n_have, k)) return FALSE;
    return distance == 0 || !tiles_list_has(r->hold, r->n_hold, k);
}

static void
tiles_write_doc(ns_tiles *t, ns_browser *b, const ns_tiles_view *v,
                const tiles_req *r, gboolean fresh, tiles_out *o)
{
    long th = t->tile_h;
    long last = MAX((long)ceil(v->page_h * v->scale / th) - 1, 0);
    long v0 = (long)floor(v->sy * v->scale / th);
    long v1 = MIN((long)floor((v->sy * v->scale + v->vh - 1) / th), last);
    long w0 = MIN(MAX((long)floor(r->want_y0 * v->scale / th), 0), v0);
    long w1 = MAX(MIN((long)floor(r->want_y1 * v->scale / th), last), v1);
    gint64 t0 = g_get_monotonic_time();
    int prefetched = 0;
    for (long d = 0; d <= MAX(v0 - w0, w1 - v1); d++)
        for (long k = w0; k <= w1; k++) {
            if (tiles_distance(k, v0, v1) != d || !tiles_wanted(r, fresh, k, d))
                continue;
            if (d > 0 && !tiles_budget_left(prefetched, t0)) return;
            if (!tiles_paint_one(t, b, v, r, o, k)) return;
            if (d > 0) prefetched++;
        }
}

typedef struct tiles_video_out {
    tiles_out *o;
    gboolean   fresh;
} tiles_video_out;

static gboolean
tiles_write_video_frame(tiles_video_out *vo, const ns_video_frame_ref *f,
                        guint sent)
{
    tiles_out *o = vo->o;
    int fw = ns_texture_get_width(f->tex);
    int fh = ns_texture_get_height(f->tex);
    if (fw <= 0 || fh <= 0) return FALSE;
    size_t row = (size_t)fw * 4u;
    size_t need = row * (size_t)fh;
    long long at = -1;
    if (vo->fresh || f->serial > sent) {
        if (o->off > o->cap || need > o->cap - o->off) return FALSE;
        ns_texture_download(f->tex, o->fb + o->off, row);
        at = (long long)o->off;
        o->off += need;
    }
    g_string_append_printf(o->desc, "vf %u %lld %d %d %lld\n", f->serial,
                           (long long)f->deadline_us, fw, fh, at);
    return TRUE;
}

static void
tiles_write_video(const ns_video_layer *layer, gpointer data)
{
    tiles_video_out *vo = data;
    g_string_append_printf(vo->o->desc, "video %u %.2f %.2f %.2f %.2f %d\n",
                           layer->id, layer->x, layer->y, layer->w, layer->h,
                           layer->slot);
    guint sent = *layer->sent_serial;
    for (guint i = 0; i < layer->n_frames; i++) {
        if (!tiles_write_video_frame(vo, &layer->frames[i], sent)) break;
        if (layer->frames[i].serial > *layer->sent_serial)
            *layer->sent_serial = layer->frames[i].serial;
    }
}

static void
tiles_write_videos(ns_browser *b, gboolean fresh, tiles_out *o)
{
    tiles_video_out vo = { o, fresh };
    g_string_append(o->desc, "videos\n");
    ns_browser_foreach_video_layer(b, tiles_write_video, &vo);
}

static void
tiles_write_header(const ns_tiles *t, ns_browser *b, const ns_tiles_view *v,
                   gboolean fresh, GString *desc)
{
    double bg[4] = { 1, 1, 1, 1 };
    ns_browser_canvas_color(b, bg);
    g_string_append_printf(desc, "gen %d %d %d %.6f %ld %d %u %d %d %d\n",
                           t->gen, v->vw, t->tile_h, v->scale, v->sx,
                           fresh ? 1 : 0, t->plan.vp->len,
                           (int)lround(bg[0] * 255), (int)lround(bg[1] * 255),
                           (int)lround(bg[2] * 255));
}

int
ns_tiles_render(ns_tiles *t, ns_browser *b, const char *body,
                const ns_tiles_view *view, gboolean invalid,
                unsigned char *fb, size_t fb_size, GString *desc)
{
    tiles_req r;
    if (!t || !b || !tiles_req_parse(body, &r)) return -1;
    gint64 t0 = g_get_monotonic_time();
    tiles_prepare(t, b, view, &r, invalid);
    if (t->state < 0) return -1;
    gboolean fresh = r.gen != t->gen;
    tiles_write_header(t, b, view, fresh, desc);
    tiles_out o = { desc, fb, 0, fb_size, (size_t)view->vw * 4u, view->vw };
    if (fresh) {
        for (guint j = 0; j < t->plan.vp->len; j++)
            tiles_write_vp_layer(t, b, view, &r, &o, j);
        ns_browser_scroller_rects(b, desc, NS_TILE_SCROLLERS_MAX);
    }
    tiles_write_doc(t, b, view, &r, fresh, &o);
    if (t->state < 0) return -1;
    tiles_write_videos(b, fresh, &o);
    if (g_getenv("NS_PROFILE"))
        g_printerr("[profile] tiles %6.1fms gen=%d fresh=%d bytes=%zu\n",
                   (double)(g_get_monotonic_time() - t0) / 1000.0, t->gen,
                   fresh ? 1 : 0, o.off);
    return 0;
}
