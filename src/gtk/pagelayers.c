/* Nordstjernen: the page as cached tile and viewport-layer textures. */

#include "pagelayers.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { PL_STICKY = 2 };

#define NS_PAGE_LAYERS_MAX_VIEW_LAYERS 4096

typedef struct {
    int         layer;
    int         x, y;
    GdkTexture *tex;
} PlPart;

typedef struct {
    long        index;
    int         gen;
    GPtrArray  *parts;
} PlTile;

typedef struct {
    int         kind;
    int         origin;
    int         x, y;
    gboolean    keep;
    double      x_offset;
    gboolean    has_top, has_bottom;
    double      top_start, top_cap, bottom_start, bottom_cap;
    GdkTexture *tex;
} PlViewLayer;

typedef struct {
    double   x, y, w, h;
    int      axes;
    gboolean fixed;
} PlScroller;

typedef struct {
    guint       serial;
    gint64      deadline_us;
    GdkTexture *tex;
} PlFrame;

typedef struct {
    guint       id;
    double      x, y, w, h;
    int         slot;
    GArray     *frames;
    guint       shown;
} PlVideo;

struct NsLayerUpdate {
    int         gen;
    int         width;
    int         tile_h;
    double      scale;
    long        sx;
    gboolean    fresh;
    int         n_vp;
    GdkRGBA     canvas;
    GPtrArray  *vp;
    GPtrArray  *tiles;
    GArray     *keeps;
    GArray     *scrollers;
    gboolean    scrollers_all;
    GArray     *wheels;
    gboolean    wheels_all;
    GPtrArray  *videos;
    gboolean    videos_listed;
};

struct NsPageLayers {
    gboolean    active;
    int         gen;
    int         width;
    int         tile_h;
    double      scale;
    long        sx;
    int         n_vp;
    GdkRGBA     canvas;
    GPtrArray  *vp;
    GHashTable *tiles;
    GArray     *scrollers;
    gboolean    scrollers_all;
    GArray     *wheels;
    gboolean    wheels_all;
    GPtrArray  *videos;
};

static void
pl_part_free(gpointer p)
{
    PlPart *part = p;
    g_clear_object(&part->tex);
    g_free(part);
}

static void
pl_tile_free(gpointer p)
{
    PlTile *t = p;
    g_ptr_array_unref(t->parts);
    g_free(t);
}

static void
pl_view_layer_free(gpointer p)
{
    PlViewLayer *l = p;
    g_clear_object(&l->tex);
    g_free(l);
}

static void
pl_frame_clear(gpointer p)
{
    g_clear_object(&((PlFrame *)p)->tex);
}

static void
pl_video_free(gpointer p)
{
    PlVideo *v = p;
    g_array_unref(v->frames);
    g_free(v);
}

static GdkTexture *
pl_texture(const unsigned char *map, size_t map_size, long long at, int width,
           int rows)
{
    size_t off = at >= 0 ? (size_t)at : map_size + 1;
    size_t row = (size_t)width * 4u;
    if (!map || width <= 0 || rows <= 0 || off > map_size ||
        (size_t)rows * row > map_size - off)
        return NULL;
    GBytes *bytes = g_bytes_new(map + off, (size_t)rows * row);
    GdkTexture *tex = gdk_memory_texture_new(width, rows, GDK_MEMORY_DEFAULT,
                                             bytes, row);
    g_bytes_unref(bytes);
    return tex;
}

static PlTile *
pl_update_tile(NsLayerUpdate *u, long index)
{
    for (guint i = 0; i < u->tiles->len; i++) {
        PlTile *t = g_ptr_array_index(u->tiles, i);
        if (t->index == index) return t;
    }
    PlTile *t = g_new0(PlTile, 1);
    t->index = index;
    t->gen = u->gen;
    t->parts = g_ptr_array_new_with_free_func(pl_part_free);
    g_ptr_array_add(u->tiles, t);
    return t;
}

static gboolean
pl_parse_gen(NsLayerUpdate *u, const char *line)
{
    int fresh = 0, r = 255, g = 255, b = 255;
    if (sscanf(line, "gen %d %d %d %lf %ld %d %d %d %d %d", &u->gen,
               &u->width, &u->tile_h, &u->scale, &u->sx, &fresh, &u->n_vp,
               &r, &g, &b) != 10)
        return FALSE;
    u->fresh = fresh != 0;
    u->canvas = (GdkRGBA){ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
    return u->width > 0 && u->tile_h > 0 && u->scale > 0 && u->n_vp >= 0 &&
           u->n_vp <= NS_PAGE_LAYERS_MAX_VIEW_LAYERS;
}

static void
pl_parse_vp(NsLayerUpdate *u, const char *line, const unsigned char *map,
            size_t map_size)
{
    PlViewLayer l = { 0 };
    int j = 0, w = 0, h = 0, has_top = 0, has_bottom = 0;
    long long at = 0;
    if (sscanf(line, "vp %d %d %d %d %d %d %d %lld %lf %d %lf %lf %d %lf %lf",
               &j, &l.kind, &l.origin, &l.x, &l.y, &w, &h, &at, &l.x_offset,
               &has_top, &l.top_start, &l.top_cap, &has_bottom,
               &l.bottom_start, &l.bottom_cap) != 15)
        return;
    l.has_top = has_top != 0;
    l.has_bottom = has_bottom != 0;
    l.keep = at < 0;
    l.tex = pl_texture(map, map_size, at, w, h);
    g_ptr_array_add(u->vp, g_memdup2(&l, sizeof l));
}

static void
pl_parse_tile(NsLayerUpdate *u, const char *line, const unsigned char *map,
              size_t map_size)
{
    PlPart part = { 0 };
    long index = 0;
    int w = 0, h = 0;
    long long at = 0;
    if (sscanf(line, "tile %ld %d %d %d %d %d %lld", &index, &part.layer,
               &part.x, &part.y, &w, &h, &at) != 7 ||
        part.layer < 0 || part.layer > u->n_vp)
        return;
    part.tex = pl_texture(map, map_size, at, w, h);
    if (part.tex)
        g_ptr_array_add(pl_update_tile(u, index)->parts,
                        g_memdup2(&part, sizeof part));
}

static void
pl_parse_scroller(NsLayerUpdate *u, const char *line)
{
    int x, y, w, h, axes, fixed;
    if (strcmp(line, "sr-all") == 0) {
        u->scrollers_all = TRUE;
        return;
    }
    if (sscanf(line, "sr %d %d %d %d %d %d", &x, &y, &w, &h, &axes,
               &fixed) != 6)
        return;
    PlScroller s = { x, y, w, h, axes, fixed != 0 };
    g_array_append_val(u->scrollers, s);
}

static void
pl_parse_wheel(NsLayerUpdate *u, const char *line)
{
    int x, y, w, h, fixed;
    if (strcmp(line, "wr-all") == 0) {
        u->wheels_all = TRUE;
        return;
    }
    if (sscanf(line, "wr %d %d %d %d %d", &x, &y, &w, &h, &fixed) != 5)
        return;
    PlScroller s = { x, y, w, h, 3, fixed != 0 };
    g_array_append_val(u->wheels, s);
}

static void
pl_parse_video(NsLayerUpdate *u, const char *line)
{
    PlVideo v = { 0 };
    if (sscanf(line, "video %u %lf %lf %lf %lf %d", &v.id, &v.x, &v.y, &v.w,
               &v.h, &v.slot) < 5 || !(v.w > 0) || !(v.h > 0))
        return;
    if (v.slot < 0) v.slot = 0;
    v.frames = g_array_new(FALSE, TRUE, sizeof(PlFrame));
    g_array_set_clear_func(v.frames, pl_frame_clear);
    g_ptr_array_add(u->videos, g_memdup2(&v, sizeof v));
}

static void
pl_parse_video_frame(NsLayerUpdate *u, const char *line,
                     const unsigned char *map, size_t map_size)
{
    if (u->videos->len == 0) return;
    PlVideo *v = g_ptr_array_index(u->videos, u->videos->len - 1);
    PlFrame f = { 0 };
    int fw = 0, fh = 0;
    long long deadline = 0, at = -1;
    if (sscanf(line, "vf %u %lld %d %d %lld", &f.serial, &deadline, &fw, &fh,
               &at) != 5)
        return;
    f.deadline_us = deadline;
    f.tex = at >= 0 ? pl_texture(map, map_size, at, fw, fh) : NULL;
    g_array_append_val(v->frames, f);
}

static void
pl_parse_keep(NsLayerUpdate *u, const char *line)
{
    long index = 0;
    if (sscanf(line, "keep %ld", &index) == 1)
        g_array_append_val(u->keeps, index);
}

static void
pl_parse_line(NsLayerUpdate *u, const char *line, const unsigned char *map,
              size_t map_size)
{
    if (g_str_has_prefix(line, "tile "))
        pl_parse_tile(u, line, map, map_size);
    else if (g_str_has_prefix(line, "keep "))
        pl_parse_keep(u, line);
    else if (g_str_has_prefix(line, "vp "))
        pl_parse_vp(u, line, map, map_size);
    else if (g_str_has_prefix(line, "sr"))
        pl_parse_scroller(u, line);
    else if (g_str_has_prefix(line, "wr"))
        pl_parse_wheel(u, line);
    else if (g_str_has_prefix(line, "video "))
        pl_parse_video(u, line);
    else if (g_str_has_prefix(line, "vf "))
        pl_parse_video_frame(u, line, map, map_size);
    else if (strcmp(line, "videos") == 0)
        u->videos_listed = TRUE;
}

NsLayerUpdate *
ns_layer_update_parse(const char *desc, const unsigned char *map,
                      size_t map_size)
{
    if (!desc) return NULL;
    NsLayerUpdate *u = g_new0(NsLayerUpdate, 1);
    u->vp = g_ptr_array_new_with_free_func(pl_view_layer_free);
    u->tiles = g_ptr_array_new_with_free_func(pl_tile_free);
    u->keeps = g_array_new(FALSE, FALSE, sizeof(long));
    u->scrollers = g_array_new(FALSE, FALSE, sizeof(PlScroller));
    u->wheels = g_array_new(FALSE, FALSE, sizeof(PlScroller));
    u->videos = g_ptr_array_new_with_free_func(pl_video_free);
    char **lines = g_strsplit(desc, "\n", -1);
    gboolean ok = lines[0] && pl_parse_gen(u, lines[0]);
    for (int i = 1; ok && lines[i]; i++)
        pl_parse_line(u, lines[i], map, map_size);
    g_strfreev(lines);
    if (ok && u->fresh && (int)u->vp->len != u->n_vp) ok = FALSE;
    if (!ok) {
        ns_layer_update_free(u);
        return NULL;
    }
    return u;
}

void
ns_layer_update_free(NsLayerUpdate *u)
{
    if (!u) return;
    g_ptr_array_unref(u->vp);
    g_ptr_array_unref(u->tiles);
    g_array_unref(u->keeps);
    g_array_unref(u->scrollers);
    g_array_unref(u->wheels);
    g_ptr_array_unref(u->videos);
    g_free(u);
}

NsPageLayers *
ns_page_layers_new(void)
{
    NsPageLayers *pl = g_new0(NsPageLayers, 1);
    pl->vp = g_ptr_array_new_with_free_func(pl_view_layer_free);
    pl->tiles = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                      pl_tile_free);
    pl->scrollers = g_array_new(FALSE, FALSE, sizeof(PlScroller));
    pl->wheels = g_array_new(FALSE, FALSE, sizeof(PlScroller));
    pl->videos = g_ptr_array_new_with_free_func(pl_video_free);
    return pl;
}

void
ns_page_layers_reset(NsPageLayers *pl)
{
    if (!pl) return;
    pl->active = FALSE;
    g_ptr_array_set_size(pl->vp, 0);
    g_hash_table_remove_all(pl->tiles);
    g_array_set_size(pl->scrollers, 0);
    pl->scrollers_all = FALSE;
    g_array_set_size(pl->wheels, 0);
    pl->wheels_all = FALSE;
    g_ptr_array_set_size(pl->videos, 0);
}

void
ns_page_layers_free(NsPageLayers *pl)
{
    if (!pl) return;
    g_ptr_array_unref(pl->vp);
    g_hash_table_destroy(pl->tiles);
    g_array_unref(pl->scrollers);
    g_array_unref(pl->wheels);
    g_ptr_array_unref(pl->videos);
    g_free(pl);
}

static gboolean
pl_same_geometry(const NsPageLayers *pl, const NsLayerUpdate *u)
{
    return pl->active && pl->width == u->width && pl->tile_h == u->tile_h &&
           pl->scale == u->scale && pl->sx == u->sx && pl->n_vp == u->n_vp;
}

static void
pl_keep_view_layers(const NsPageLayers *pl, NsLayerUpdate *u)
{
    guint n = MIN(u->vp->len, pl->vp->len);
    for (guint j = 0; j < n; j++) {
        PlViewLayer *l = g_ptr_array_index(u->vp, j);
        PlViewLayer *old = g_ptr_array_index(pl->vp, j);
        if (l->keep && !l->tex && old->tex) l->tex = g_object_ref(old->tex);
    }
}

static void
pl_adopt_geometry(NsPageLayers *pl, NsLayerUpdate *u)
{
    if (pl_same_geometry(pl, u))
        pl_keep_view_layers(pl, u);
    else
        g_hash_table_remove_all(pl->tiles);
    pl->active = TRUE;
    pl->gen = u->gen;
    pl->width = u->width;
    pl->tile_h = u->tile_h;
    pl->scale = u->scale;
    pl->sx = u->sx;
    pl->n_vp = u->n_vp;
    pl->canvas = u->canvas;
    GPtrArray *vp = pl->vp;
    pl->vp = u->vp;
    u->vp = vp;
    GArray *sc = pl->scrollers;
    pl->scrollers = u->scrollers;
    u->scrollers = sc;
    pl->scrollers_all = u->scrollers_all;
    GArray *wh = pl->wheels;
    pl->wheels = u->wheels;
    u->wheels = wh;
    pl->wheels_all = u->wheels_all;
}

static void
pl_relabel_kept(NsPageLayers *pl, const NsLayerUpdate *u)
{
    for (guint i = 0; i < u->keeps->len; i++) {
        gint64 key = g_array_index(u->keeps, long, i);
        PlTile *t = g_hash_table_lookup(pl->tiles, &key);
        if (t) t->gen = u->gen;
    }
}

static const PlVideo *
pl_video_by_id(const NsPageLayers *pl, guint id)
{
    for (guint i = 0; i < pl->videos->len; i++) {
        const PlVideo *v = g_ptr_array_index(pl->videos, i);
        if (v->id == id) return v;
    }
    return NULL;
}

static GdkTexture *
pl_video_frame_texture(const PlVideo *v, guint serial)
{
    for (guint i = 0; v && i < v->frames->len; i++) {
        const PlFrame *f = &g_array_index(v->frames, PlFrame, i);
        if (f->serial == serial) return f->tex;
    }
    return NULL;
}

static guint
pl_video_due(const PlVideo *v, gint64 now_us)
{
    guint due = 0;
    for (guint i = 0; i < v->frames->len; i++) {
        const PlFrame *f = &g_array_index(v->frames, PlFrame, i);
        if (!f->tex) continue;
        if (!due || f->deadline_us <= now_us) due = f->serial;
        if (f->deadline_us > now_us) break;
    }
    return due;
}

static gboolean
pl_adopt_videos(NsPageLayers *pl, NsLayerUpdate *u)
{
    gboolean changed = u->videos->len != pl->videos->len;
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < u->videos->len; i++) {
        PlVideo *v = g_ptr_array_index(u->videos, i);
        const PlVideo *old = pl_video_by_id(pl, v->id);
        for (guint j = 0; j < v->frames->len; j++) {
            PlFrame *f = &g_array_index(v->frames, PlFrame, j);
            GdkTexture *kept = f->tex ? NULL
                                      : pl_video_frame_texture(old, f->serial);
            if (kept) f->tex = g_object_ref(kept);
        }
        v->shown = old ? old->shown : 0;
        if (!pl_video_frame_texture(v, v->shown)) {
            v->shown = pl_video_due(v, now);
            changed = TRUE;
        }
        if (!old || old->slot != v->slot || old->x != v->x ||
            old->y != v->y || old->w != v->w ||
            old->h != v->h)
            changed = TRUE;
    }
    GPtrArray *videos = pl->videos;
    pl->videos = u->videos;
    u->videos = videos;
    return changed;
}

gboolean
ns_page_layers_advance_videos(NsPageLayers *pl, gint64 now_us)
{
    gboolean changed = FALSE;
    for (guint i = 0; pl && i < pl->videos->len; i++) {
        PlVideo *v = g_ptr_array_index(pl->videos, i);
        guint due = pl_video_due(v, now_us);
        if (due && due != v->shown) {
            v->shown = due;
            changed = TRUE;
        }
    }
    return changed;
}

gboolean
ns_page_layers_apply(NsPageLayers *pl, NsLayerUpdate *u)
{
    if (!pl || !u) return FALSE;
    if (u->fresh)
        pl_adopt_geometry(pl, u);
    else if (!pl->active || u->gen != pl->gen)
        return FALSE;
    for (guint i = 0; i < u->tiles->len; i++) {
        PlTile *t = g_ptr_array_index(u->tiles, i);
        gint64 *key = g_new(gint64, 1);
        *key = t->index;
        g_hash_table_replace(pl->tiles, key, t);
    }
    pl_relabel_kept(pl, u);
    g_ptr_array_set_free_func(u->tiles, NULL);
    gboolean changed = u->fresh || u->tiles->len > 0 || u->keeps->len > 0;
    if (u->videos_listed && pl_adopt_videos(pl, u)) changed = TRUE;
    g_ptr_array_set_size(u->tiles, 0);
    return changed;
}

gboolean
ns_page_layers_active(const NsPageLayers *pl)
{
    return pl && pl->active;
}

int
ns_page_layers_gen(const NsPageLayers *pl)
{
    return pl && pl->active ? pl->gen : -1;
}

static long
pl_tile_of(const NsPageLayers *pl, double y)
{
    return (long)floor(y * pl->scale / pl->tile_h);
}

static const PlTile *
pl_lookup(const NsPageLayers *pl, long k)
{
    gint64 key = k;
    return g_hash_table_lookup(pl->tiles, &key);
}

int
ns_page_layers_vp_held(const NsPageLayers *pl)
{
    return pl && pl->active ? pl->n_vp : -1;
}

char *
ns_page_layers_video_held(const NsPageLayers *pl)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; pl && pl->active && i < pl->videos->len; i++) {
        const PlVideo *v = g_ptr_array_index(pl->videos, i);
        for (guint j = 0; j < v->frames->len; j++) {
            const PlFrame *f = &g_array_index(v->frames, PlFrame, j);
            if (f->tex)
                g_string_append_printf(out, "%s%u", out->len ? "," : "",
                                       f->serial);
        }
    }
    return g_string_free(out, FALSE);
}

char *
ns_page_layers_have(const NsPageLayers *pl, double y0, double y1,
                    gboolean any_gen)
{
    GString *out = g_string_new(NULL);
    if (pl && pl->active)
        for (long k = MAX(pl_tile_of(pl, y0), 0); k <= pl_tile_of(pl, y1);
             k++) {
            const PlTile *t = pl_lookup(pl, k);
            if (t && (any_gen || t->gen == pl->gen))
                g_string_append_printf(out, "%s%ld", out->len ? "," : "", k);
        }
    return g_string_free(out, FALSE);
}

gboolean
ns_page_layers_missing(const NsPageLayers *pl, double y0, double y1,
                       double view_y0, double view_y1, double page_h)
{
    if (!pl || !pl->active) return FALSE;
    long last = MAX((long)ceil(page_h * pl->scale / pl->tile_h) - 1, 0);
    long k1 = MIN(pl_tile_of(pl, y1), last);
    long v0 = pl_tile_of(pl, view_y0), v1 = pl_tile_of(pl, view_y1);
    for (long k = MAX(pl_tile_of(pl, y0), 0); k <= k1; k++) {
        const PlTile *t = pl_lookup(pl, k);
        gboolean visible = k >= v0 && k <= v1;
        if (!t || (visible && t->gen != pl->gen)) return TRUE;
    }
    return FALSE;
}

void
ns_page_layers_evict(NsPageLayers *pl, double y0, double y1)
{
    if (!pl || !pl->active) return;
    long k0 = pl_tile_of(pl, y0), k1 = pl_tile_of(pl, y1);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, pl->tiles);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        long k = (long)*(gint64 *)key;
        if (k < k0 || k > k1) g_hash_table_iter_remove(&it);
    }
}

static gboolean
pl_scroller_contains(const PlScroller *s, double x, double y)
{
    return x >= s->x && x < s->x + s->w && y >= s->y && y < s->y + s->h;
}

gboolean
ns_page_layers_scroller_at(const NsPageLayers *pl, double x, double y,
                           double scroll_x, double scroll_y,
                           gboolean vertical)
{
    if (!pl || !pl->active) return FALSE;
    if (pl->scrollers_all) return TRUE;
    int axis = vertical ? 2 : 1;
    for (guint i = 0; i < pl->scrollers->len; i++) {
        const PlScroller *s = &g_array_index(pl->scrollers, PlScroller, i);
        if ((s->axes & axis) &&
            pl_scroller_contains(s, s->fixed ? x : x + scroll_x,
                                 s->fixed ? y : y + scroll_y))
            return TRUE;
    }
    return FALSE;
}

gboolean
ns_page_layers_wheel_listener_at(const NsPageLayers *pl, double x, double y,
                                 double scroll_x, double scroll_y)
{
    if (!pl || !pl->active) return TRUE;
    if (pl->wheels_all) return TRUE;
    for (guint i = 0; i < pl->wheels->len; i++) {
        const PlScroller *s = &g_array_index(pl->wheels, PlScroller, i);
        if (pl_scroller_contains(s, s->fixed ? x : x + scroll_x,
                                 s->fixed ? y : y + scroll_y))
            return TRUE;
    }
    return FALSE;
}

static double
pl_sticky_offset(const PlViewLayer *l, double scroll_y)
{
    double dy = 0;
    if (l->has_top && scroll_y > l->top_start)
        dy = MIN(scroll_y - l->top_start, l->top_cap);
    if (l->has_bottom && dy == 0 && scroll_y < l->bottom_start)
        dy = MAX(scroll_y - l->bottom_start, l->bottom_cap);
    return dy;
}

static void
pl_append(GtkSnapshot *snapshot, GdkTexture *tex, double x, double y,
          double raster)
{
    graphene_rect_t r = GRAPHENE_RECT_INIT(
        (float)(x / raster), (float)(y / raster),
        (float)(gdk_texture_get_width(tex) / raster),
        (float)(gdk_texture_get_height(tex) / raster));
    gtk_snapshot_append_texture(snapshot, tex, &r);
}

static void
pl_snapshot_view_layer(const NsPageLayers *pl, const PlViewLayer *l,
                       GtkSnapshot *snapshot, double raster, double ox,
                       double oy, double scroll_y)
{
    if (!l->tex) return;
    if (l->kind != PL_STICKY) {
        pl_append(snapshot, l->tex, l->x, l->origin + l->y, raster);
        return;
    }
    double dy = round(pl_sticky_offset(l, scroll_y) * pl->scale);
    double dx = round(l->x_offset * pl->scale);
    pl_append(snapshot, l->tex, l->x + dx - ox, l->origin + l->y + dy - oy,
              raster);
}

static void
pl_snapshot_doc_layer(const NsPageLayers *pl, int layer, GtkSnapshot *snapshot,
                      double raster, double ox, double oy, long k0, long k1)
{
    for (long k = k0; k <= k1; k++) {
        const PlTile *t = pl_lookup(pl, k);
        for (guint i = 0; t && i < t->parts->len; i++) {
            const PlPart *part = g_ptr_array_index(t->parts, i);
            if (part->layer == layer)
                pl_append(snapshot, part->tex, part->x - ox,
                          (double)k * pl->tile_h + part->y - oy, raster);
        }
    }
}

static void
pl_snapshot_videos(const NsPageLayers *pl, int slot, GtkSnapshot *snapshot,
                   double raster, double ox, double oy)
{
    for (guint i = 0; i < pl->videos->len; i++) {
        const PlVideo *v = g_ptr_array_index(pl->videos, i);
        if (MIN(v->slot, pl->n_vp) != slot) continue;
        GdkTexture *tex = pl_video_frame_texture(v, v->shown);
        if (!tex) continue;
        graphene_rect_t r = GRAPHENE_RECT_INIT(
            (float)((v->x - ox) / raster), (float)((v->y - oy) / raster),
            (float)(v->w / raster), (float)(v->h / raster));
        gtk_snapshot_append_scaled_texture(snapshot, tex,
                                           GSK_SCALING_FILTER_LINEAR, &r);
    }
}

void
ns_page_layers_snapshot(const NsPageLayers *pl, GtkSnapshot *snapshot,
                        double width, double height, double raster,
                        double scroll_x, double scroll_y)
{
    if (!pl || !pl->active || !(raster > 0)) return;
    graphene_rect_t area = GRAPHENE_RECT_INIT(0, 0, (float)width,
                                              (float)height);
    gtk_snapshot_append_color(snapshot, &pl->canvas, &area);
    double oy = round(scroll_y * pl->scale);
    double ox = round((scroll_x - pl->sx) * pl->scale);
    long k0 = (long)floor(oy / pl->tile_h);
    long k1 = (long)floor((oy + height * raster - 1) / pl->tile_h);
    gtk_snapshot_push_clip(snapshot, &area);
    for (int layer = 0; layer <= pl->n_vp; layer++) {
        if (layer > 0 && (guint)layer <= pl->vp->len)
            pl_snapshot_view_layer(pl, g_ptr_array_index(pl->vp, layer - 1),
                                   snapshot, raster, ox, oy, scroll_y);
        pl_snapshot_videos(pl, layer, snapshot, raster, ox, oy);
        pl_snapshot_doc_layer(pl, layer, snapshot, raster, ox, oy, k0, k1);
    }
    gtk_snapshot_pop(snapshot);
}
