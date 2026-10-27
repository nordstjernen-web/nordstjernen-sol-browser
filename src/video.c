/* Nordstjernen — inline video playback and poster cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include <stdio.h>
#include "video.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "camera.h"
#include "dom.h"
#include "image.h"
#include "layout.h"
#include "net.h"
#include "video_decode.h"

#define NS_VIDEO_AHEAD_MIN_S      0.12
#define NS_VIDEO_AHEAD_MAX_S      1.50
#define NS_VIDEO_STALL_DECAY_S    8.0
#define NS_VIDEO_AHEAD_BYTES      (64u * 1024u * 1024u)
#define NS_VIDEO_PREROLL_MAX_US   G_GINT64_CONSTANT(2000000)

#define NS_VIDEO_MAX_BYTES (256u * 1024u * 1024u)

typedef struct ns_vtt_cue {
    double start;
    double end;
    char  *text;
} ns_vtt_cue;

static void
ns_vtt_cue_free(gpointer p)
{
    ns_vtt_cue *c = p;
    if (!c) return;
    g_free(c->text);
    g_free(c);
}

static gboolean
ns_vtt_read_uint(const char **pp, long *out)
{
    const char *p = *pp;
    if (!g_ascii_isdigit(*p)) return FALSE;
    long v = 0;
    while (g_ascii_isdigit(*p)) {
        if (v < 100000000L) v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    *pp = p;
    return TRUE;
}

static double
ns_vtt_parse_ts(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    long a, b, c;
    if (!ns_vtt_read_uint(&s, &a)) return -1.0;
    if (*s != ':') return -1.0;
    s++;
    if (!ns_vtt_read_uint(&s, &b)) return -1.0;
    long hours = 0, mins, secs;
    if (*s == ':') {
        s++;
        if (!ns_vtt_read_uint(&s, &c)) return -1.0;
        hours = a; mins = b; secs = c;
    } else {
        mins = a; secs = b;
    }
    long ms = 0;
    if (*s == '.' || *s == ',') {
        s++;
        long f = 0;
        int nd = 0;
        for (; g_ascii_isdigit(*s); s++)
            if (nd < 3) { f = f * 10 + (*s - '0'); nd++; }
        for (; nd < 3; nd++) f *= 10;
        ms = f;
    }
    return (double)hours * 3600.0 + (double)mins * 60.0 + (double)secs
         + (double)ms / 1000.0;
}

static char *
ns_vtt_clean_text(const char *raw)
{
    GString *out = g_string_new(NULL);
    for (const char *p = raw; *p; ) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            p = e + 1;
            continue;
        }
        if (*p == '&') {
            if (g_str_has_prefix(p, "&amp;"))  { g_string_append_c(out, '&'); p += 5; continue; }
            if (g_str_has_prefix(p, "&lt;"))   { g_string_append_c(out, '<'); p += 4; continue; }
            if (g_str_has_prefix(p, "&gt;"))   { g_string_append_c(out, '>'); p += 4; continue; }
            if (g_str_has_prefix(p, "&nbsp;")) { g_string_append_c(out, ' '); p += 6; continue; }
            if (g_str_has_prefix(p, "&lrm;"))  { p += 5; continue; }
            if (g_str_has_prefix(p, "&rlm;"))  { p += 5; continue; }
        }
        g_string_append_c(out, *p);
        p++;
    }
    return g_string_free(out, FALSE);
}

static GPtrArray *
ns_vtt_parse(const char *text)
{
    GPtrArray *cues = g_ptr_array_new_with_free_func(ns_vtt_cue_free);
    char **lines = g_strsplit(text, "\n", -1);
    guint n = g_strv_length(lines);
    for (guint i = 0; i < n; ) {
        char *line = lines[i];
        gsize ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = '\0';
        const char *arrow = strstr(line, "-->");
        if (!arrow) { i++; continue; }

        double start = ns_vtt_parse_ts(line);
        double end = ns_vtt_parse_ts(arrow + 3);
        i++;
        GString *txt = g_string_new(NULL);
        while (i < n) {
            char *tl = lines[i];
            gsize tll = strlen(tl);
            if (tll && tl[tll - 1] == '\r') tl[--tll] = '\0';
            if (tll == 0) { i++; break; }
            if (txt->len) g_string_append_c(txt, '\n');
            char *clean = ns_vtt_clean_text(tl);
            g_string_append(txt, clean);
            g_free(clean);
            i++;
        }
        if (start >= 0.0 && end > start && txt->len > 0) {
            ns_vtt_cue *cue = g_new0(ns_vtt_cue, 1);
            cue->start = start;
            cue->end = end;
            cue->text = g_string_free(txt, FALSE);
            g_ptr_array_add(cues, cue);
        } else {
            g_string_free(txt, TRUE);
        }
    }
    g_strfreev(lines);
    return cues;
}

const char *
ns_video_active_cue_text(const ns_video *v)
{
    if (!v || !v->cues) return NULL;
    double t = v->cur_time;
    for (guint i = 0; i < v->cues->len; i++) {
        const ns_vtt_cue *c = g_ptr_array_index(v->cues, i);
        if (t >= c->start && t < c->end) return c->text;
    }
    return NULL;
}

struct ns_video_cache {
    GHashTable       *by_url;
    GHashTable       *requested;
    GHashTable       *mse_streams;
    GHashTable       *node_state;
    GPtrArray        *pending;
    char             *base_url;
    ns_video_js_cb    js_cb;
    gpointer          js_user;
    ns_video_audio_cb audio_cb;
    gpointer          audio_user;
    guint             next_token;
    guint             next_seq;
    guint             layout_note_gen;
    guint             discover_gen;
    guint             layer_gen;
};

typedef struct ns_pending {
    ns_video       *video;
    ns_video_cache *cache;
    gboolean        dead;
} ns_pending;

static void ns_video_materialize_audio(ns_video_cache *cache, ns_video *v,
                                       const guint8 *data, gsize len,
                                       guint gen, double start);

typedef struct ns_node_media_state {
    gboolean has_playing, playing;
    gboolean has_muted, muted;
    gboolean has_volume;
    double   volume;
    gboolean has_seek;
    double   seek_time;
} ns_node_media_state;

#define NS_MSE_GAP_S 0.5
#define NS_MSE_WINDOW_TRIM_BYTES (96u * 1024u * 1024u)

typedef struct ns_mse_chunk {
    guint  offset;
    guint  length;
    guint  serial;
    double start;
    double end;
} ns_mse_chunk;

typedef struct ns_mse_stream {
    guint       id;
    GByteArray *video_bytes;
    GByteArray *audio_bytes;
    GByteArray *video_init;
    GByteArray *audio_init;
    GArray     *video_chunks;
    GArray     *audio_chunks;
    double      video_end;
    double      audio_end;
    gboolean    eos;
    gboolean    audio_dirty;
    gboolean    audio_rebased;
    gint64      audio_flush_us;
    gboolean    video_dirty;
    gint64      video_flush_us;
    guint       video_gen;
    guint       audio_gen;
    guint       init_gen;
    guint       next_serial;
    double      video_gen_start;
    double      audio_gen_start;
} ns_mse_stream;

static gboolean
ns_mse_bytes_are_init_segment(const guint8 *d, gsize n)
{
    if (n >= 8 && d[4] == 'f' && d[5] == 't' && d[6] == 'y' && d[7] == 'p')
        return TRUE;
    if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3)
        return TRUE;
    return FALSE;
}

static void
ns_mse_stream_free(gpointer p)
{
    ns_mse_stream *s = p;
    if (!s) return;
    if (s->video_init) g_byte_array_free(s->video_init, TRUE);
    if (s->audio_init) g_byte_array_free(s->audio_init, TRUE);
    if (s->video_bytes) g_byte_array_free(s->video_bytes, TRUE);
    if (s->audio_bytes) g_byte_array_free(s->audio_bytes, TRUE);
    if (s->video_chunks) g_array_free(s->video_chunks, TRUE);
    if (s->audio_chunks) g_array_free(s->audio_chunks, TRUE);
    g_free(s);
}
static gboolean
ns_mse_chunk_ranged(const ns_mse_chunk *c)
{
    return isfinite(c->start) && isfinite(c->end) && c->end > c->start;
}

static gint
ns_mse_chunk_by_start(gconstpointer pa, gconstpointer pb)
{
    const ns_mse_chunk *a = pa, *b = pb;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return a->serial < b->serial ? -1 : (a->serial > b->serial ? 1 : 0);
}

static GArray *
ns_mse_sorted_chunks(const GArray *chunks)
{
    GArray *sorted = g_array_new(FALSE, FALSE, sizeof(ns_mse_chunk));
    for (guint i = 0; chunks && i < chunks->len; i++) {
        const ns_mse_chunk *c = &g_array_index(chunks, ns_mse_chunk, i);
        if (ns_mse_chunk_ranged(c)) g_array_append_val(sorted, *c);
    }
    g_array_sort(sorted, ns_mse_chunk_by_start);
    return sorted;
}

static GArray *
ns_mse_ranges(const GArray *chunks)
{
    GArray *sorted = ns_mse_sorted_chunks(chunks);
    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(double));
    double start = 0.0, end = -1.0;
    for (guint i = 0; i < sorted->len; i++) {
        const ns_mse_chunk *c = &g_array_index(sorted, ns_mse_chunk, i);
        if (end >= 0.0 && c->start <= end + NS_MSE_GAP_S) {
            if (c->end > end) end = c->end;
            continue;
        }
        if (end >= 0.0) {
            g_array_append_val(ranges, start);
            g_array_append_val(ranges, end);
        }
        start = c->start;
        end = c->end;
    }
    if (end >= 0.0) {
        g_array_append_val(ranges, start);
        g_array_append_val(ranges, end);
    }
    g_array_free(sorted, TRUE);
    return ranges;
}

static gboolean
ns_mse_range_near(const GArray *chunks, double t, double *out_start,
                  double *out_end)
{
    GArray *ranges = ns_mse_ranges(chunks);
    gboolean found = FALSE;
    for (guint i = 0; i + 1 < ranges->len; i += 2) {
        double rs = g_array_index(ranges, double, i);
        double re = g_array_index(ranges, double, i + 1);
        if (re > t || i + 2 >= ranges->len) {
            *out_start = rs;
            *out_end = re;
            found = TRUE;
            break;
        }
    }
    g_array_free(ranges, TRUE);
    return found;
}

static ns_video *
ns_video_for_mse_id(ns_video_cache *cache, guint id)
{
    char key[32];
    g_snprintf(key, sizeof key, "ndmse:%u", id);
    return g_hash_table_lookup(cache->by_url, key);
}

static void on_msaudio_fetched(GObject *src, GAsyncResult *result,
                               gpointer user_data);
static void ns_video_build_player(ns_pending *pending, ns_response *resp);
static void ns_video_show_frame_at(ns_video *v, double t);
static void ns_video_mse_seek(ns_video_cache *cache, ns_video *v,
                              double from, double t);

static void
ns_video_free(gpointer p)
{
    ns_video *v = p;
    if (!v) return;
    g_free(v->url);
    g_free(v->audio_url);
    if (v->audio_file) {
        if (g_str_has_prefix(v->audio_file, "file://"))
            g_unlink(v->audio_file + 7);
        g_free(v->audio_file);
    }
    if (v->pending_audio_file) {
        if (g_str_has_prefix(v->pending_audio_file, "file://"))
            g_unlink(v->pending_audio_file + 7);
        g_free(v->pending_audio_file);
    }
    if (v->mse_window) g_byte_array_free(v->mse_window, TRUE);
    g_free(v->token);
    if (v->cues) g_ptr_array_free(v->cues, TRUE);
    ns_texture_unref(v->poster_texture);
    g_free(v->poster_url);
    g_free(v->cors_allow_origin);
    ns_texture_unref(v->frame_texture);
    ns_video_player_free(v->player);
    g_free(v);
}

ns_video_cache *
ns_video_cache_new(void)
{
    ns_video_cache *c = g_new0(ns_video_cache, 1);
    c->by_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ns_video_free);
    c->requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    c->mse_streams = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                           NULL, ns_mse_stream_free);
    c->node_state = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, g_free);
    c->pending = g_ptr_array_new();
    return c;
}

void
ns_video_cache_set_base(ns_video_cache *cache, const char *base_url)
{
    if (!cache) return;
    g_free(cache->base_url);
    cache->base_url = g_strdup(base_url);
}

void
ns_video_cache_set_js_cb(ns_video_cache *cache, ns_video_js_cb cb, gpointer user_data)
{
    if (!cache) return;
    cache->js_cb = cb;
    cache->js_user = user_data;
}

void
ns_video_cache_set_audio_cb(ns_video_cache *cache, ns_video_audio_cb cb,
                            gpointer user_data)
{
    if (!cache) return;
    cache->audio_cb = cb;
    cache->audio_user = user_data;
}

static void
ns_video_emit_audio(ns_video_cache *cache, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

static void
ns_video_emit_audio(ns_video_cache *cache, const char *fmt, ...)
{
    if (!cache || !cache->audio_cb) return;
    va_list ap;
    va_start(ap, fmt);
    char *cmd = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    cache->audio_cb(cmd, cache->audio_user);
    g_free(cmd);
}

static gboolean
ns_video_url_audio_safe(const char *url)
{
    if (!url || !*url) return FALSE;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return FALSE;
    return TRUE;
}

static void
ns_video_audio_start(ns_video_cache *cache, ns_video *v)
{
    if (v && g_getenv("NS_DBG_AUDIO"))
        g_printerr("[audio-start] muted=%d has_audio=%d file=%s playing=%d\n",
                   v->muted, v->has_audio,
                   v->audio_file ? v->audio_file : "-", v->playing);
    if (!cache || !cache->audio_cb || !v || v->muted) return;
    if (!v->has_audio && !v->audio_file) return;
    if (!v->audio_file) {
        if (v->audio_fetching || !v->url) return;
        v->audio_fetching = TRUE;
        ns_pending *pa = g_new0(ns_pending, 1);
        pa->video = v;
        pa->cache = cache;
        g_ptr_array_add(cache->pending, pa);
        ns_net_fetch_async(v->url, cache->base_url, NULL, on_msaudio_fetched,
                           pa);
        return;
    }
    const char *src = v->audio_file;
    if (!ns_video_url_audio_safe(src)) return;
    if (!v->token)
        v->token = g_strdup_printf("nv%u", ++cache->next_token);
    if (!v->audio_opened) {
        ns_video_emit_audio(cache, "open %s %s", v->token, src);
        if (v->audio_timeline_start > 0.0)
            ns_video_emit_audio(cache, "offset %s %.3f", v->token,
                                v->audio_timeline_start);
        if (v->loop)
            ns_video_emit_audio(cache, "loop %s 1", v->token);
        if (v->volume >= 0.0 && v->volume < 1.0)
            ns_video_emit_audio(cache, "volume %s %.3f", v->token, v->volume);
        v->audio_opened = TRUE;
    }
    ns_video_emit_audio(cache, "play %s", v->token);
}

static void
ns_video_audio_pause(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->audio_opened) return;
    ns_video_emit_audio(cache, "pause %s", v->token);
}

static void
ns_video_audio_stop(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->audio_opened) return;
    ns_video_emit_audio(cache, "stop %s", v->token);
    v->audio_opened = FALSE;
}

static void
ns_video_audio_resync(ns_video_cache *cache, ns_video *v)
{
    if (!cache || !cache->audio_cb || !v || !v->audio_opened) return;
    ns_video_emit_audio(cache, "seek %s 0", v->token);
}

void
ns_video_note_painted(ns_video *v)
{
    if (v) v->last_paint_us = g_get_monotonic_time();
}

void
ns_video_cache_note_layout(ns_video_cache *cache, const ns_box *root)
{
    if (!cache || !root) return;
    cache->layout_note_gen++;
    GPtrArray *videos = g_ptr_array_new();
    ns_layout_collect_videos(root, videos);
    for (guint i = 0; i < videos->len; i++) {
        ns_box *box = g_ptr_array_index(videos, i);
        ns_video *v = box->media ? box->media->video : NULL;
        if (!v) continue;
        v->layout_note_gen = cache->layout_note_gen;
        ns_video_note_painted(v);
    }
    g_ptr_array_free(videos, TRUE);
}

void
ns_video_cache_free(ns_video_cache *cache)
{
    if (!cache) return;
    if (cache->audio_cb) {
        GHashTableIter it;
        gpointer key, val;
        g_hash_table_iter_init(&it, cache->by_url);
        while (g_hash_table_iter_next(&it, &key, &val))
            ns_video_audio_stop(cache, val);
    }
    for (guint i = 0; i < cache->pending->len; i++) {
        ns_pending *p = g_ptr_array_index(cache->pending, i);
        p->dead = TRUE;
    }
    g_hash_table_destroy(cache->by_url);
    g_hash_table_destroy(cache->requested);
    g_hash_table_destroy(cache->mse_streams);
    g_hash_table_destroy(cache->node_state);
    g_ptr_array_free(cache->pending, TRUE);
    g_free(cache->base_url);
    g_free(cache);
}

static void
ns_video_emit_js(ns_video_cache *cache, ns_video *v, const char *kind, double value)
{
    if (cache && cache->js_cb && v && v->dom_node)
        cache->js_cb(v->dom_node, kind, value, cache->js_user);
}

void
ns_video_play(ns_video *v, gint64 now_us)
{
    if (!v || !v->player) return;
    if (v->playing) return;
    if (v->ended) { v->cur_time = 0.0; v->ended = FALSE; }
    v->base_us = now_us - (gint64)(v->cur_time * 1e6);
    v->playing = TRUE;
}

void
ns_video_pause(ns_video *v, gint64 now_us)
{
    if (!v || !v->player || !v->playing) return;
    v->cur_time = (double)(now_us - v->base_us) / 1e6;
    v->playing = FALSE;
}

gboolean
ns_video_toggle(ns_video *v, gint64 now_us)
{
    if (!v || !v->player) return FALSE;
    if (v->playing) ns_video_pause(v, now_us);
    else ns_video_play(v, now_us);
    return TRUE;
}

static ns_node_media_state *
ns_video_node_state(ns_video_cache *cache, const void *dom_node)
{
    ns_node_media_state *st = g_hash_table_lookup(cache->node_state,
                                                  (gpointer)dom_node);
    if (!st) {
        st = g_new0(ns_node_media_state, 1);
        g_hash_table_insert(cache->node_state, (gpointer)dom_node, st);
    }
    return st;
}

static void
ns_video_apply_node_state(ns_video_cache *cache, ns_video *v,
                          const void *dom_node)
{
    ns_node_media_state *st = g_hash_table_lookup(cache->node_state,
                                                  (gpointer)dom_node);
    if (!st) return;
    if (st->has_muted)
        v->muted = st->muted;
    if (st->has_volume)
        v->volume = st->volume;
    if (st->has_seek) {
        v->cur_time = st->seek_time;
        v->prev_tick_time = st->seek_time;
        v->last_emit_time = st->seek_time;
        if (v->playing)
            v->base_us = g_get_monotonic_time()
                       - (gint64)(st->seek_time * 1e6);
    }
    if (st->has_playing) {
        if (v->player && st->playing != v->playing) {
            gint64 now = g_get_monotonic_time();
            if (st->playing) {
                ns_video_play(v, now);
                ns_video_audio_start(cache, v);
            } else {
                ns_video_pause(v, now);
                ns_video_audio_pause(cache, v);
            }
        } else {
            v->playing = st->playing;
        }
    }
    g_hash_table_remove(cache->node_state, (gpointer)dom_node);
}

static ns_video *
ns_video_cache_find_by_node(ns_video_cache *cache, const void *dom_node)
{
    ns_video *best = NULL;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *cand = val;
        if (cand->dom_node != dom_node || cand->is_camera) continue;
        if (!best || cand->seq > best->seq) best = cand;
    }
    return best;
}

ns_texture *
ns_video_cache_frame_for_node(ns_video_cache *cache, const void *dom_node,
                              const char **out_url, const char **out_cors)
{
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (!v) return NULL;
    if (!v->sampled && v->player && !v->frame_texture) {
        gboolean ended = FALSE;
        ns_texture *frame = ns_video_player_frame_at(v->player, v->cur_time,
                                                     v->loop, &ended);
        if (frame) v->frame_texture = ns_texture_ref(frame);
    }
    v->sampled = TRUE;
    if (v->frame_texture) {
        if (out_url) *out_url = v->url;
        if (out_cors) *out_cors = v->cors_allow_origin;
        return v->frame_texture;
    }
    if (out_url) *out_url = v->poster_url;
    return v->poster_texture;
}

static ns_video *
ns_video_cache_find_mse_rollover(ns_video_cache *cache)
{
    ns_video *best = NULL;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *cand = val;
        if (!cand->mse_id || !cand->playing || cand->is_camera) continue;
        if (!best || cand->seq > best->seq) best = cand;
    }
    return best;
}

static void
ns_video_inherit_node_playback(ns_video *v, const ns_video *previous)
{
    if (!v || !previous) return;
    v->playing = previous->playing;
    v->muted = previous->muted;
    v->volume = previous->volume;
    v->cur_time = previous->cur_time;
    v->prev_tick_time = previous->cur_time;
    v->last_emit_time = previous->cur_time;
    if (v->playing)
        v->base_us = g_get_monotonic_time()
                   - (gint64)(v->cur_time * 1e6);
}

static void
ns_video_release_node(ns_video_cache *cache, ns_video *v)
{
    ns_video_audio_stop(cache, v);
    v->playing = FALSE;
    v->dom_node = NULL;
}

gboolean
ns_video_cache_seek_node(ns_video_cache *cache, const void *dom_node,
                         double seconds, gint64 now_us)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (v && v->is_camera) return FALSE;
    if (!v || !v->player) {
        if (v) {
            v->cur_time = seconds < 0.0 ? 0.0 : seconds;
            v->prev_tick_time = v->cur_time;
            v->last_emit_time = v->cur_time;
        } else {
            ns_node_media_state *st = ns_video_node_state(cache, dom_node);
            st->has_seek = TRUE;
            st->seek_time = seconds < 0.0 ? 0.0 : seconds;
        }
        return TRUE;
    }

    double t = seconds;
    if (t < 0.0) t = 0.0;
    if (!v->mse_id && v->duration > 0.0 && t > v->duration) t = v->duration;
    double from = v->prev_tick_time;
    v->cur_time = t;
    v->prev_tick_time = t;
    v->last_emit_time = t;
    v->ended = !v->mse_id && v->duration > 0.0 && t >= v->duration &&
               !v->loop;
    v->stall_since_us = 0;
    if (v->playing)
        v->base_us = now_us - (gint64)(t * 1e6);

    if (v->mse_id)
        ns_video_mse_seek(cache, v, from, t);
    else
        ns_video_show_frame_at(v, t);
    if (v->audio_opened)
        ns_video_emit_audio(cache, "seek %s %.3f", v->token, t);
    ns_video_emit_js(cache, v, "pos", t);
    return TRUE;
}

gboolean
ns_video_cache_set_node_playing(ns_video_cache *cache, const void *dom_node,
                                gboolean play, gint64 now_us)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (v && v->is_camera) return FALSE;
    if (!v) {
        ns_node_media_state *st = ns_video_node_state(cache, dom_node);
        st->has_playing = TRUE;
        st->playing = play;
        return TRUE;
    }
    if (!v->player) { v->playing = play; return TRUE; }
    if (play == v->playing) return FALSE;
    gboolean restart = play && v->ended;
    if (play) {
        if (v->player) {
            ns_video_play(v, now_us);
        } else {
            if (restart) {
                v->cur_time = 0.0;
                v->ended = FALSE;
            }
            v->base_us = now_us - (gint64)(v->cur_time * 1e6);
            v->playing = TRUE;
        }
        ns_video_audio_start(cache, v);
        if (restart && v->audio_opened)
            ns_video_emit_audio(cache, "seek %s 0", v->token);
    } else {
        if (v->player) {
            ns_video_pause(v, now_us);
        } else {
            if (v->base_us > 0)
                v->cur_time = (double)(now_us - v->base_us) / 1e6;
            v->playing = FALSE;
        }
        ns_video_audio_pause(cache, v);
    }
    return TRUE;
}

gboolean
ns_video_cache_set_node_volume(ns_video_cache *cache, const void *dom_node,
                               double volume)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (!v) {
        ns_node_media_state *st = ns_video_node_state(cache, dom_node);
        st->has_volume = TRUE;
        st->volume = volume;
        return TRUE;
    }
    v->volume = volume;
    if (v->token && v->audio_opened)
        ns_video_emit_audio(cache, "volume %s %.3f", v->token, volume);
    if (volume > 0 && !v->muted && v->playing && !v->audio_opened) {
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
    }
    return TRUE;
}

gboolean
ns_video_cache_set_node_muted(ns_video_cache *cache, const void *dom_node,
                              gboolean muted)
{
    if (!cache || !dom_node) return FALSE;
    ns_video *v = ns_video_cache_find_by_node(cache, dom_node);
    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[set-muted] muted=%d video=%s was=%d\n",
                   muted, v ? "yes" : "NULL", v ? v->muted : -1);
    if (!v) {
        ns_node_media_state *st = ns_video_node_state(cache, dom_node);
        st->has_muted = TRUE;
        st->muted = muted;
        return TRUE;
    }
    if (v->muted == muted) return TRUE;
    v->muted = muted;
    if (muted) {
        ns_video_audio_pause(cache, v);
    } else if (v->playing) {
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
    }
    return TRUE;
}

gboolean
ns_video_cache_toggle(ns_video_cache *cache, ns_video *v, gint64 now_us)
{
    if (!v || !v->player) return FALSE;
    if (v->playing && v->muted) {
        v->muted = FALSE;
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
        ns_video_emit_js(cache, v, "unmuted", v->cur_time);
        return TRUE;
    }
    gboolean was_playing = v->playing;
    gboolean restart = !was_playing && v->ended;
    if (v->player) {
        ns_video_toggle(v, now_us);
    } else if (was_playing) {
        if (v->base_us > 0)
            v->cur_time = (double)(now_us - v->base_us) / 1e6;
        v->playing = FALSE;
    } else {
        if (restart) {
            v->cur_time = 0.0;
            v->ended = FALSE;
        }
        v->base_us = now_us - (gint64)(v->cur_time * 1e6);
        v->playing = TRUE;
    }
    ns_video_emit_js(cache, v, was_playing ? "pause" : "play", v->cur_time);
    if (was_playing) ns_video_audio_pause(cache, v);
    else ns_video_audio_start(cache, v);
    if (restart && v->audio_opened)
        ns_video_emit_audio(cache, "seek %s 0", v->token);
    return TRUE;
}

static gboolean
ns_video_url_is_growing_stream(const char *url)
{
    if (!url) return FALSE;
    const char *marker = strstr(url, "#ndms=");
    return marker != NULL && strstr(marker, "&eos") == NULL;
}

static gboolean
ns_video_stream_growing(ns_video_cache *cache, const ns_video *v)
{
    if (v->mse_id) {
        ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                               GUINT_TO_POINTER(v->mse_id));
        return !s || !s->eos;
    }
    return ns_video_url_is_growing_stream(v->url);
}

static double
ns_video_buffered_end(ns_video_cache *cache, const ns_video *v)
{
    if (v->mse_id) {
        double start = 0.0;
        double video_end = ns_video_cache_mse_buffered(cache, v->mse_id, 'v',
                                                       -1, &start);
        double audio_end = ns_video_cache_mse_buffered(cache, v->mse_id, 'a',
                                                       -1, &start);
        if (video_end > 0.0 && audio_end > 0.0)
            return MIN(video_end, audio_end);
        if (video_end > 0.0) return video_end;
        if (audio_end > 0.0) return audio_end;
    }
    return v->player ? ns_video_player_buffered_end(v->player) : 0.0;
}

static double
ns_video_buffered_start(ns_video_cache *cache, const ns_video *v)
{
    if (!v->mse_id) return 0.0;
    double video_start = 0.0, audio_start = 0.0;
    double video_end = ns_video_cache_mse_buffered(cache, v->mse_id, 'v', -1,
                                                   &video_start);
    double audio_end = ns_video_cache_mse_buffered(cache, v->mse_id, 'a', -1,
                                                   &audio_start);
    double start = 0.0;
    if (video_end > 0.0) start = video_start;
    if (audio_end > 0.0 && audio_start > start) start = audio_start;
    return start;
}

static guint
ns_mse_url_id(const char *url)
{
    if (!url || !g_str_has_prefix(url, "blob:nd-mse/")) return 0;
    return (guint)g_ascii_strtoull(url + strlen("blob:nd-mse/"), NULL, 10);
}

static char *
ns_video_stream_key(const char *abs_url)
{
    guint mse_id = ns_mse_url_id(abs_url);
    if (mse_id) return g_strdup_printf("ndmse:%u", mse_id);
    const char *marker = strstr(abs_url, "#ndms=");
    return marker ? g_strndup(abs_url, (gsize)(marker - abs_url))
                  : g_strdup(abs_url);
}

static gboolean
extend_timed(ns_video_player *player, const guint8 *data, gsize len)
{
    gint64 t0 = g_get_monotonic_time();
    gboolean ok = ns_video_player_extend(player, data, len);
    if (g_getenv("NS_PROFILE"))
        fprintf(stderr, "[profile] mse extend %6.1fms  %zub ok=%d\n",
                (double)(g_get_monotonic_time() - t0) / 1000.0,
                (size_t)len, ok);
    return ok;
}

static void
on_extend_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    ns_video *v = pending->video;
    if (resp && !resp->error && resp->body && resp->body->len > 0 &&
        resp->body->len <= NS_VIDEO_MAX_BYTES) {
        if (!v->player) {
            ns_video_build_player(pending, resp);
            v->loaded = TRUE;
        } else if (extend_timed(v->player, resp->body->data,
                                resp->body->len)) {
            v->buf_sent = FALSE;
            if (v->has_audio && !v->audio_url)
                ns_video_materialize_audio(pending->cache, v,
                                           resp->body->data,
                                           resp->body->len, 0, 0.0);
        } else {
            ns_video_player *fresh =
                ns_video_player_new(resp->body->data, resp->body->len);
            if (fresh) {
                ns_video_player_free(v->player);
                v->player = fresh;
                v->duration = ns_video_player_duration(fresh);
                v->has_audio = ns_video_player_has_audio(fresh);
                v->buf_sent = FALSE;
                if (v->playing)
                    v->base_us = g_get_monotonic_time()
                               - (gint64)(v->cur_time * 1e6);
            }
        }
    }
    g_clear_error(&err);
    ns_response_free(resp);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static void
ns_video_check_audio_attr(ns_video_cache *cache, ns_video *v,
                          const ns_node *dom)
{
    const char *asrc = dom ? ns_element_get_attr(dom, "data-audio-src") : NULL;
    if (!asrc || !*asrc || !g_str_has_prefix(asrc, "blob:")) return;
    if (v->audio_url && strcmp(v->audio_url, asrc) == 0) return;
    g_free(v->audio_url);
    v->audio_url = g_strdup(asrc);
    ns_pending *pa = g_new0(ns_pending, 1);
    pa->video = v;
    pa->cache = cache;
    g_ptr_array_add(cache->pending, pa);
    ns_net_fetch_async(asrc, cache->base_url, NULL, on_msaudio_fetched, pa);
}

static void
ns_video_refresh_growing_stream(ns_video_cache *cache, ns_video *v,
                                gint64 now_us)
{
    if (v->mse_id) return;
    if (!v->url || !v->player) return;
    if (now_us - v->last_refresh_us < G_GINT64_CONSTANT(500000)) return;
    v->last_refresh_us = now_us;
    char *base = ns_video_stream_key(v->url);
    ns_pending *pe = g_new0(ns_pending, 1);
    pe->video = v;
    pe->cache = cache;
    g_ptr_array_add(cache->pending, pe);
    ns_net_fetch_async(base, cache->base_url, NULL, on_extend_fetched, pe);
    g_free(base);
    ns_video_check_audio_attr(cache, v, v->dom_node);
}

static void
ns_video_note_stream_version(ns_video_cache *cache, ns_video *v,
                             const ns_node *dom, const char *abs_url)
{
    if (!v || v->is_camera || !abs_url) return;
    if (v->url && strcmp(v->url, abs_url) == 0) return;
    if (g_hash_table_contains(cache->requested, abs_url)) return;
    g_hash_table_add(cache->requested, g_strdup(abs_url));
    g_free(v->url);
    v->url = g_strdup(abs_url);
    ns_pending *pe = g_new0(ns_pending, 1);
    pe->video = v;
    pe->cache = cache;
    g_ptr_array_add(cache->pending, pe);
    ns_net_fetch_async(abs_url, cache->base_url, NULL,
                       on_extend_fetched, pe);
    ns_video_check_audio_attr(cache, v, dom);
}

static void
ns_video_build_player(ns_pending *pending, ns_response *resp)
{
    ns_video *v = pending->video;
    if (!resp || resp->error || !resp->body || resp->body->len == 0 ||
        resp->body->len > NS_VIDEO_MAX_BYTES)
        return;

    ns_video_player *player = ns_video_player_new(resp->body->data,
                                                  resp->body->len);
    if (!player) return;

    v->player = player;
    v->duration = ns_video_player_duration(player);
    v->has_audio = ns_video_player_has_audio(player);
    if (v->natural_width <= 0)  v->natural_width  = ns_video_player_width(player);
    if (v->natural_height <= 0) v->natural_height = ns_video_player_height(player);

    {
        gboolean ended = FALSE;
        ns_texture *frame = ns_video_player_frame_at(player, v->cur_time,
                                                     v->loop, &ended);
        if (frame) {
            ns_texture_unref(v->frame_texture);
            v->frame_texture = ns_texture_ref(frame);
        }
    }

    if (v->has_audio && !v->audio_file && v->url &&
        g_str_has_prefix(v->url, "blob:"))
        ns_video_materialize_audio(pending->cache, v,
                                   resp->body->data, resp->body->len, 0, 0.0);

    if (v->playing) {
        if (v->base_us == 0)
            v->base_us = g_get_monotonic_time()
                       - (gint64)(v->cur_time * 1e6);
        ns_video_audio_start(pending->cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(pending->cache, "seek %s %.3f",
                                v->token, v->cur_time);
    } else if (v->autoplay) {
        ns_video_play(v, g_get_monotonic_time());
        ns_video_audio_start(pending->cache, v);
        ns_video_emit_js(pending->cache, v, "play", v->cur_time);
    }
}

static gboolean
ns_video_stream_file_append(const char *file_url, const guint8 *data,
                            gsize len, gsize *written)
{
    if (!file_url || !g_str_has_prefix(file_url, "file://") ||
        *written > len)
        return FALSE;
    if (*written == len) return TRUE;
    FILE *f = g_fopen(file_url + 7, "ab");
    if (!f) return FALSE;
    gsize wrote = fwrite(data + *written, 1, len - *written, f);
    fclose(f);
    if (wrote != len - *written) return FALSE;
    *written = len;
    return TRUE;
}

static char *
ns_video_stream_file_create(ns_video_cache *cache, const char *subdir,
                            char prefix, const guint8 *data, gsize len)
{
    char *dir = g_build_filename(g_get_user_cache_dir(),
                                 "nordstjernen", subdir, NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_strdup_printf("%s/%c%d-%u.dat", dir, prefix,
                                 (int)getpid(), ++cache->next_token);
    char *file_url = NULL;
    if (g_file_set_contents_full(path, (const char *)data, (gssize)len,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_ONLY_EXISTING, 0600, NULL))
        file_url = g_strdup_printf("file://%s", path);
    g_free(path);
    g_free(dir);
    return file_url;
}

static void
ns_video_materialize_audio(ns_video_cache *cache, ns_video *v,
                           const guint8 *data, gsize len, guint gen,
                           double start)
{
    if (v->audio_file && gen != v->audio_file_gen) {
        if (v->pending_audio_file && gen == v->pending_audio_file_gen) {
            ns_video_stream_file_append(v->pending_audio_file, data, len,
                                        &v->pending_audio_file_len);
            if (start >= 0.0) v->pending_audio_start = start;
            return;
        }
        if (v->pending_audio_file) {
            if (g_str_has_prefix(v->pending_audio_file, "file://"))
                g_unlink(v->pending_audio_file + 7);
            g_free(v->pending_audio_file);
        }
        v->pending_audio_file = ns_video_stream_file_create(
            cache, "msaudio", 'a', data, len);
        if (!v->pending_audio_file) return;
        v->pending_audio_file_len = len;
        v->pending_audio_file_gen = gen;
        v->pending_audio_start = start;
        return;
    }
    if (v->audio_file)
        ns_video_stream_file_append(v->audio_file, data, len,
                                    &v->audio_file_len);
    if (!v->audio_file) {
        v->audio_file = ns_video_stream_file_create(cache, "msaudio", 'a',
                                                    data, len);
        if (!v->audio_file) return;
        v->audio_file_len = len;
        v->audio_file_gen = gen;
        v->audio_timeline_start = start > 0.0 ? start : 0.0;
    }
    if (v->audio_opened) {
        ns_video_emit_audio(cache, "reload %s %s", v->token, v->audio_file);
        if (v->playing && !v->muted)
            ns_video_emit_audio(cache, "resume %s", v->token);
    } else if (v->playing && !v->muted) {
        ns_video_audio_start(cache, v);
        if (v->audio_opened && v->cur_time > 0)
            ns_video_emit_audio(cache, "seek %s %.3f",
                                v->token, v->cur_time);
    }
}

static void
ns_video_activate_pending(ns_video_cache *cache, ns_video *v)
{
    if (v->pending_audio_file && v->pending_audio_file_len >= 4096 &&
        v->pending_audio_start >= 0.0 &&
        v->cur_time + 0.35 >= v->pending_audio_start) {
        char *old = v->audio_file;
        v->audio_file = v->pending_audio_file;
        v->audio_file_len = v->pending_audio_file_len;
        v->audio_file_gen = v->pending_audio_file_gen;
        v->audio_timeline_start = v->pending_audio_start;
        v->pending_audio_file = NULL;
        v->pending_audio_file_len = 0;
        ns_video_emit_audio(cache, "reload %s %s", v->token, v->audio_file);
        ns_video_emit_audio(cache, "offset %s %.3f", v->token,
                            v->audio_timeline_start);
        ns_video_emit_audio(cache, "seek %s %.3f", v->token, v->cur_time);
        if (v->playing && !v->muted)
            ns_video_emit_audio(cache, "resume %s", v->token);
        if (old) {
            if (g_str_has_prefix(old, "file://")) g_unlink(old + 7);
            g_free(old);
        }
    }
}

static void
ns_mse_window_append(GByteArray *window, const ns_mse_stream *s,
                     const ns_mse_chunk *c)
{
    const GByteArray *bytes = s->video_bytes;
    if (c->offset > bytes->len || c->length > bytes->len - c->offset) return;
    g_byte_array_append(window, bytes->data + c->offset, c->length);
}

static void
ns_video_show_frame_at(ns_video *v, double t)
{
    gboolean ended = FALSE;
    ns_texture *frame = ns_video_player_frame_at(v->player, t, v->loop,
                                                 &ended);
    if (frame) {
        ns_texture_unref(v->frame_texture);
        v->frame_texture = ns_texture_ref(frame);
    }
}

static gboolean
ns_video_mse_window_build(ns_video *v, ns_mse_stream *s, double t,
                          gboolean any_start)
{
    if (!s->video_init || !s->video_bytes || !s->video_chunks ||
        s->video_chunks->len == 0)
        return FALSE;
    GByteArray *window = g_byte_array_new();
    g_byte_array_append(window, s->video_init->data, s->video_init->len);
    GArray *sorted = ns_mse_sorted_chunks(s->video_chunks);
    double start = 0.0, end = 0.0;
    guint last_serial = 0;
    gboolean ranged = sorted->len > 0;
    if (ranged) {
        gint first = -1;
        for (guint i = 0; i < sorted->len; i++) {
            const ns_mse_chunk *c = &g_array_index(sorted, ns_mse_chunk, i);
            if (c->end <= t + 0.001) continue;
            if (c->start <= t + NS_MSE_GAP_S || (any_start && first < 0))
                first = (gint)i;
            if (c->start > t) break;
        }
        if (first < 0) {
            g_array_free(sorted, TRUE);
            g_byte_array_free(window, TRUE);
            return FALSE;
        }
        const ns_mse_chunk *head = &g_array_index(sorted, ns_mse_chunk, first);
        start = head->start;
        end = head->end;
        last_serial = head->serial;
        ns_mse_window_append(window, s, head);
        for (guint i = (guint)first + 1; i < sorted->len; i++) {
            const ns_mse_chunk *c = &g_array_index(sorted, ns_mse_chunk, i);
            if (c->end <= end + 0.001) continue;
            if (c->start > end + NS_MSE_GAP_S) break;
            ns_mse_window_append(window, s, c);
            end = c->end;
            if (c->serial > last_serial) last_serial = c->serial;
        }
    } else {
        for (guint i = 0; i < s->video_chunks->len; i++) {
            const ns_mse_chunk *c = &g_array_index(s->video_chunks,
                                                   ns_mse_chunk, i);
            ns_mse_window_append(window, s, c);
            last_serial = c->serial;
        }
    }
    g_array_free(sorted, TRUE);

    ns_video_player *player = ns_video_player_new(window->data, window->len);
    if (!player) {
        g_byte_array_free(window, TRUE);
        return FALSE;
    }
    ns_video_player_free(v->player);
    if (v->mse_window) g_byte_array_free(v->mse_window, TRUE);
    v->player = player;
    v->mse_window = window;
    v->mse_window_gen = s->init_gen;
    v->mse_window_ranged = ranged;
    v->mse_window_start = start;
    v->mse_window_end = end;
    v->mse_window_serial = last_serial;
    v->mse_window_wanted = FALSE;
    v->has_audio = ns_video_player_has_audio(player);
    double d = ns_video_player_duration(player);
    if (d > v->duration) v->duration = d;
    if (v->natural_width <= 0)
        v->natural_width = ns_video_player_width(player);
    if (v->natural_height <= 0)
        v->natural_height = ns_video_player_height(player);
    v->loaded = TRUE;
    v->buf_sent = FALSE;
    if (ranged && t < start && any_start) {
        v->cur_time = start;
        v->prev_tick_time = start;
        if (v->playing) v->base_us = g_get_monotonic_time()
                                   - (gint64)(start * 1e6);
    }
    ns_video_show_frame_at(v, v->cur_time);
    if (v->playing && v->base_us == 0)
        v->base_us = g_get_monotonic_time() - (gint64)(v->cur_time * 1e6);
    return TRUE;
}

static gboolean
ns_video_mse_window_extend(ns_video *v, ns_mse_stream *s)
{
    if (!v->player || !v->mse_window || v->mse_window_gen != s->init_gen ||
        !s->video_chunks)
        return TRUE;
    gsize before = v->mse_window->len;
    if (v->mse_window_ranged) {
        GArray *sorted = ns_mse_sorted_chunks(s->video_chunks);
        for (guint i = 0; i < sorted->len; i++) {
            const ns_mse_chunk *c = &g_array_index(sorted, ns_mse_chunk, i);
            if (c->end <= v->mse_window_end + 0.001) continue;
            if (c->start > v->mse_window_end + NS_MSE_GAP_S) break;
            ns_mse_window_append(v->mse_window, s, c);
            v->mse_window_end = c->end;
            if (c->serial > v->mse_window_serial)
                v->mse_window_serial = c->serial;
        }
        g_array_free(sorted, TRUE);
    } else {
        for (guint i = 0; i < s->video_chunks->len; i++) {
            const ns_mse_chunk *c = &g_array_index(s->video_chunks,
                                                   ns_mse_chunk, i);
            if (c->serial <= v->mse_window_serial) continue;
            ns_mse_window_append(v->mse_window, s, c);
            v->mse_window_serial = c->serial;
        }
    }
    if (v->mse_window->len == before) return TRUE;
    v->buf_sent = FALSE;
    return ns_video_player_extend(v->player, v->mse_window->data,
                                  v->mse_window->len);
}

static void
ns_video_mse_seek(ns_video_cache *cache, ns_video *v, double from, double t)
{
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(v->mse_id));
    gboolean in_window = v->mse_window && v->mse_window_ranged &&
                         t >= from - 1e-3 && t <= from + 2.0 &&
                         t < v->mse_window_end;
    if (in_window) {
        ns_video_show_frame_at(v, t);
        return;
    }
    if (s && ns_video_mse_window_build(v, s, t, FALSE)) return;
    v->mse_window_wanted = TRUE;
    v->buf_sent = FALSE;
}

static gboolean
ns_video_mse_window_wants_trim(const ns_video *v, const ns_mse_stream *s)
{
    if (!v->mse_window || !v->mse_window_ranged ||
        v->mse_window->len < NS_MSE_WINDOW_TRIM_BYTES)
        return FALSE;
    for (guint i = 0; s->video_chunks && i < s->video_chunks->len; i++) {
        const ns_mse_chunk *c = &g_array_index(s->video_chunks,
                                               ns_mse_chunk, i);
        if (!ns_mse_chunk_ranged(c)) continue;
        if (c->start > v->mse_window_start + 1.0 &&
            c->start <= v->cur_time && v->cur_time - c->start < 0.5)
            return TRUE;
    }
    return FALSE;
}

static double
ns_video_mse_window_edge(const ns_video *v)
{
    if (v->mse_window && v->mse_window_ranged) return v->mse_window_end;
    return v->player ? ns_video_player_buffered_end(v->player) : 0.0;
}

static void
ns_video_mse_sync(ns_video_cache *cache, ns_mse_stream *s, ns_video *v)
{
    if (!v || !s) return;
    if (!v->player) {
        ns_video_mse_window_build(v, s, v->cur_time, TRUE);
    } else if (v->mse_window_wanted || v->mse_window_gen != s->init_gen ||
               ns_video_mse_window_wants_trim(v, s)) {
        ns_video_mse_window_build(v, s, v->cur_time, FALSE);
    } else if (!ns_video_mse_window_extend(v, s)) {
        ns_video_mse_window_build(v, s, v->cur_time, FALSE);
    }
    if (v->player && s->video_end > 0.0) {
        ns_video_player_note_end(v->player, s->video_end);
        double d = ns_video_player_duration(v->player);
        if (d > v->duration) v->duration = d;
    }
    gint64 now = g_get_monotonic_time();
    gboolean has_side_audio = s->audio_bytes && s->audio_bytes->len > 0;
    if (s->video_bytes && s->video_bytes->len >= 4096 && s->video_dirty &&
        (s->eos || now - s->video_flush_us > G_GINT64_CONSTANT(250000))) {
        s->video_dirty = FALSE;
        s->video_flush_us = now;
        if (!has_side_audio && v->has_audio && v->audio_file)
            ns_video_materialize_audio(cache, v, s->video_bytes->data,
                                       s->video_bytes->len, s->video_gen,
                                       s->video_gen_start);
    }
    if (has_side_audio && s->audio_bytes->len >= 4096 && s->audio_dirty &&
        (s->eos || now - s->audio_flush_us > G_GINT64_CONSTANT(250000))) {
        s->audio_dirty = FALSE;
        s->audio_flush_us = now;
        ns_video_materialize_audio(cache, v, s->audio_bytes->data,
                                   s->audio_bytes->len, s->audio_gen,
                                   s->audio_gen_start);
        if (s->audio_rebased) {
            s->audio_rebased = FALSE;
            if (v->audio_opened)
                ns_video_emit_audio(cache, "seek %s 0", v->token);
        }
    }
    if (!has_side_audio && v->player && v->has_audio && !v->audio_file &&
        s->video_bytes && s->video_bytes->len > 0)
        ns_video_materialize_audio(cache, v, s->video_bytes->data,
                                   s->video_bytes->len, s->video_gen,
                                   s->video_gen_start);
}

gboolean
ns_video_cache_mse_append(ns_video_cache *cache, guint stream_id, char kind,
                          const guint8 *data, gsize len)
{
    if (!cache || !stream_id || !data || !len) return FALSE;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) {
        s = g_new0(ns_mse_stream, 1);
        s->id = stream_id;
        g_hash_table_insert(cache->mse_streams,
                            GUINT_TO_POINTER(stream_id), s);
    }
    s->eos = FALSE;
    GByteArray **dst = kind == 'a' ? &s->audio_bytes : &s->video_bytes;
    GByteArray **init = kind == 'a' ? &s->audio_init : &s->video_init;
    GArray **chunks = kind == 'a' ? &s->audio_chunks : &s->video_chunks;
    double *appended_end = kind == 'a' ? &s->audio_end : &s->video_end;
    if (!*dst) *dst = g_byte_array_new();
    if (!*chunks) *chunks = g_array_new(FALSE, FALSE, sizeof(ns_mse_chunk));
    ns_video *sv = ns_video_for_mse_id(cache, stream_id);

    gboolean is_init = ns_mse_bytes_are_init_segment(data, len);
    if (is_init && *init && (*dst)->len > 0 && (*init)->len == len &&
        memcmp((*init)->data, data, len) == 0)
        return TRUE;
    gboolean new_generation = (*dst)->len > 0 && is_init;
    double *gen_start = kind == 'a' ? &s->audio_gen_start
                                    : &s->video_gen_start;
    guint *generation = kind == 'a' ? &s->audio_gen : &s->video_gen;
    if (new_generation) {
        if (kind != 'a') s->init_gen++;
        g_byte_array_set_size(*dst, 0);
        g_array_set_size(*chunks, 0);
        (*generation)++;
        *gen_start = -1.0;
        *appended_end = 0.0;
    }
    if (is_init) {
        if (!*init) *init = g_byte_array_new();
        g_byte_array_set_size(*init, 0);
        g_byte_array_append(*init, data, len);
    }
    if ((*dst)->len + len > NS_VIDEO_MAX_BYTES) return FALSE;
    guint chunk_offset = (*dst)->len;
    g_byte_array_append(*dst, data, len);
    if (kind == 'a') s->audio_dirty = TRUE;
    else s->video_dirty = TRUE;

    if (!is_init) {
        double chunk_start = 0.0;
        double chunk_end = 0.0;
        gboolean ranged = ns_video_probe_chunk_range(
                *init ? (*init)->data : NULL, *init ? (*init)->len : 0,
                data, len, &chunk_start, &chunk_end);
        if (ranged) {
            if (*gen_start < 0.0) *gen_start = chunk_start;
            if (chunk_end > *appended_end) *appended_end = chunk_end;
        }
        ns_mse_chunk chunk = {
            .offset = chunk_offset,
            .length = (guint)len,
            .serial = ++s->next_serial,
            .start = ranged ? chunk_start : NAN,
            .end = ranged ? chunk_end : NAN,
        };
        g_array_append_val(*chunks, chunk);
    }

    if (g_getenv("NS_DBG_AUDIO"))
        g_printerr("[mse-append] id=%u kind=%c len=%zu total=%u vend=%.1f "
                   "aend=%.1f%s\n",
                   stream_id, kind, len, (*dst)->len, s->video_end,
                   s->audio_end, new_generation ? " NEW-GEN" : "");
    ns_video_mse_sync(cache, s, sv);
    return TRUE;
}

double
ns_video_cache_mse_buffered(ns_video_cache *cache, guint stream_id, char kind,
                            int index, double *start)
{
    if (start) *start = 0.0;
    if (!cache || !stream_id) return 0.0;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) return 0.0;
    GArray *chunks = kind == 'a' ? s->audio_chunks : s->video_chunks;
    double end = kind == 'a' ? s->audio_end : s->video_end;
    if (index < 0) {
        ns_video *v = ns_video_for_mse_id(cache, stream_id);
        double t = v ? v->cur_time : 0.0;
        double rs = 0.0, re = 0.0;
        if (ns_mse_range_near(chunks, t, &rs, &re)) {
            if (start) *start = rs;
            return re;
        }
        double first = kind == 'a' ? s->audio_gen_start : s->video_gen_start;
        if (start && end > 0.0 && first >= 0.0 && first < end) *start = first;
        return end;
    }
    GArray *ranges = ns_mse_ranges(chunks);
    double out = -1.0;
    guint at = (guint)index * 2;
    if (at + 1 < ranges->len) {
        if (start) *start = g_array_index(ranges, double, at);
        out = g_array_index(ranges, double, at + 1);
    } else if (index == 0 && ranges->len == 0 && end > 0.0) {
        double first = kind == 'a' ? s->audio_gen_start : s->video_gen_start;
        if (start) *start = first >= 0.0 && first < end ? first : 0.0;
        out = end;
    }
    g_array_free(ranges, TRUE);
    return out;
}

gsize
ns_video_cache_mse_bytes(ns_video_cache *cache, guint stream_id, char kind)
{
    if (!cache || !stream_id) return 0;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) return 0;
    GByteArray *bytes = kind == 'a' ? s->audio_bytes : s->video_bytes;
    return bytes ? bytes->len : 0;
}

gboolean
ns_video_cache_mse_remove(ns_video_cache *cache, guint stream_id, char kind,
                          double start, double end)
{
    if (!cache || !stream_id || start < 0.0 || end <= start) return FALSE;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) return FALSE;
    GByteArray **bytes_slot = kind == 'a' ? &s->audio_bytes : &s->video_bytes;
    GByteArray *bytes = *bytes_slot;
    GByteArray *init = kind == 'a' ? s->audio_init : s->video_init;
    GArray **chunks_slot = kind == 'a' ? &s->audio_chunks : &s->video_chunks;
    GArray *chunks = *chunks_slot;
    double *buffered_end = kind == 'a' ? &s->audio_end : &s->video_end;
    gboolean clears_all = start <= 0.001 && end + 0.05 >= *buffered_end;
    if (!bytes || bytes->len == 0) return TRUE;

    GByteArray *kept_bytes = g_byte_array_new();
    GArray *kept_chunks = g_array_new(FALSE, FALSE, sizeof(ns_mse_chunk));
    if (init && init->len > 0)
        g_byte_array_append(kept_bytes, init->data, init->len);
    gboolean removed = FALSE;
    gboolean retained_unknown = FALSE;
    double retained_start = -1.0;
    double retained_end = 0.0;
    if (chunks) {
        for (guint i = 0; i < chunks->len; i++) {
            ns_mse_chunk chunk = g_array_index(chunks, ns_mse_chunk, i);
            gboolean known = isfinite(chunk.start) && isfinite(chunk.end);
            gboolean drop = clears_all ||
                (known && chunk.start < end && chunk.end > start);
            if (drop) {
                removed = TRUE;
                continue;
            }
            if (chunk.offset > bytes->len ||
                chunk.length > bytes->len - chunk.offset) {
                removed = TRUE;
                continue;
            }
            chunk.offset = kept_bytes->len;
            g_byte_array_append(kept_bytes, bytes->data +
                                g_array_index(chunks, ns_mse_chunk, i).offset,
                                chunk.length);
            g_array_append_val(kept_chunks, chunk);
            if (known) {
                if (retained_start < 0.0 || chunk.start < retained_start)
                    retained_start = chunk.start;
                if (chunk.end > retained_end) retained_end = chunk.end;
            } else {
                retained_unknown = TRUE;
            }
        }
    } else if (clears_all) {
        removed = bytes->len > (init ? init->len : 0);
    }
    if (!removed && !clears_all) {
        g_byte_array_free(kept_bytes, TRUE);
        g_array_free(kept_chunks, TRUE);
        return TRUE;
    }
    g_byte_array_free(bytes, TRUE);
    if (chunks) g_array_free(chunks, TRUE);
    *bytes_slot = kept_bytes;
    *chunks_slot = kept_chunks;
    if (clears_all || (!retained_unknown && kept_chunks->len == 0))
        *buffered_end = 0.0;
    else if (!retained_unknown)
        *buffered_end = retained_end;
    ns_video *v = ns_video_for_mse_id(cache, stream_id);
    if (kind == 'a') {
        s->audio_gen++;
        s->audio_gen_start = retained_start;
        s->audio_dirty = kept_bytes->len > 0;
        s->audio_rebased = TRUE;
        s->audio_flush_us = 0;
        if (v) ns_video_audio_stop(cache, v);
    } else {
        gint64 now = g_get_monotonic_time();
        if (v && v->playing && v->base_us > 0)
            v->cur_time = (double)(now - v->base_us) / 1e6;
        s->video_gen++;
        s->video_gen_start = retained_start;
        s->video_dirty = kept_bytes->len > 0;
        s->video_flush_us = 0;
        if (v) {
            v->prev_tick_time = v->cur_time;
            v->last_emit_time = v->cur_time;
            v->base_us = v->playing
                       ? now - (gint64)(v->cur_time * 1e6) : 0;
            v->ended = FALSE;
            v->buf_sent = FALSE;
        }
    }
    return TRUE;
}

void
ns_video_cache_mse_eos(ns_video_cache *cache, guint stream_id)
{
    if (!cache || !stream_id) return;
    ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                           GUINT_TO_POINTER(stream_id));
    if (!s) return;
    s->eos = TRUE;
    ns_video_mse_sync(cache, s, ns_video_for_mse_id(cache, stream_id));
}

static void
on_msaudio_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    ns_video_cache *cache = pending->cache;
    ns_video *v = pending->video;
    if (!pending->dead && resp && !resp->error && resp->body &&
        resp->body->len > 0 && resp->body->len <= NS_VIDEO_MAX_BYTES)
        ns_video_materialize_audio(cache, v, resp->body->data,
                                   resp->body->len, 0, 0.0);
    g_clear_error(&err);
    if (resp) ns_response_free(resp);
    if (!pending->dead)
        g_ptr_array_remove_fast(cache->pending, pending);
    g_free(pending);
}

static void
on_video_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    ns_video_build_player(pending, resp);
    ns_video *video = pending->video;
    if (resp && !resp->error) {
        g_free(video->cors_allow_origin);
        video->cors_allow_origin = g_strdup(resp->cors_allow_origin);
    }
    g_clear_error(&err);
    ns_response_free(resp);
    pending->video->loaded = TRUE;
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static void
on_poster_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    if (resp && !resp->error && resp->body && resp->body->len > 0) {
        int w = 0, h = 0;
        ns_texture *tex = ns_image_decode_bytes(resp->body->data,
                                                resp->body->len, &w, &h);
        if (tex) {
            pending->video->poster_texture = tex;
            g_free(pending->video->poster_url);
            pending->video->poster_url = g_strdup(resp->final_url);
            if (pending->video->natural_width  <= 0) pending->video->natural_width  = w;
            if (pending->video->natural_height <= 0) pending->video->natural_height = h;
        }
    }
    g_clear_error(&err);
    ns_response_free(resp);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static void
on_track_fetched(GObject *src, GAsyncResult *result, gpointer user_data)
{
    (void)src;
    ns_pending *pending = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(result, &err);
    if (pending->dead) {
        ns_response_free(resp);
        g_clear_error(&err);
        g_free(pending);
        return;
    }
    ns_video *v = pending->video;
    if (resp && !resp->error && resp->body && resp->body->len > 0 &&
        resp->body->len <= NS_VIDEO_MAX_BYTES) {
        char *text = g_strndup((const char *)resp->body->data, resp->body->len);
        GPtrArray *cues = ns_vtt_parse(text);
        g_free(text);
        if (v->cues) g_ptr_array_free(v->cues, TRUE);
        v->cues = cues;
    }
    g_clear_error(&err);
    ns_response_free(resp);
    g_ptr_array_remove_fast(pending->cache->pending, pending);
    g_free(pending);
}

static const ns_node *
ns_video_pick_track(const ns_node *dom)
{
    for (const ns_node *c = dom->first_child; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT || !c->name) continue;
        if (strcmp(c->name, "track") != 0) continue;
        if (!ns_element_get_attr(c, "default")) continue;
        const char *kind = ns_element_get_attr(c, "kind");
        if (kind && *kind &&
            g_ascii_strcasecmp(kind, "subtitles") != 0 &&
            g_ascii_strcasecmp(kind, "captions") != 0)
            continue;
        const char *tsrc = ns_element_get_attr(c, "src");
        if (tsrc && *tsrc) return c;
    }
    return NULL;
}

static void
ns_video_discover_track(ns_video_cache *cache, ns_video *v, const ns_node *dom)
{
    if (!v || !dom || v->track_requested) return;
    const ns_node *track = ns_video_pick_track(dom);
    if (!track) return;
    const char *tsrc = ns_element_get_attr(track, "src");
    char *abs = ns_url_resolve(cache->base_url, tsrc);
    if (!abs) return;
    v->track_requested = TRUE;
    ns_pending *pt = g_new0(ns_pending, 1);
    pt->video = v;
    pt->cache = cache;
    g_ptr_array_add(cache->pending, pt);
    ns_net_fetch_async(abs, cache->base_url, NULL, on_track_fetched, pt);
    g_free(abs);
}

static gboolean
attr_present(const ns_node *n, const char *name)
{
    const char *v = n ? ns_element_get_attr(n, name) : NULL;
    return v != NULL;
}

static gboolean url_is_inline_video(const char *url);

gboolean
ns_video_url_is_inline(const char *url)
{
    return url_is_inline_video(url);
}

static gboolean
url_is_inline_video(const char *url)
{
    if (!url) return FALSE;
    if (g_str_has_prefix(url, "blob:")) return TRUE;
    gsize n = strcspn(url, "?#");
    static const char *const exts[] = {
        ".mpg", ".mpeg", ".m1v", ".mpeg1", ".mpg1",
#ifdef NS_HAVE_LIBAV
        ".mp4", ".m4v", ".webm", ".ogv",
#elif defined(NS_HAVE_OS_MP4)
        ".mp4", ".m4v",
#endif
        NULL,
    };
    for (int i = 0; exts[i]; i++) {
        gsize el = strlen(exts[i]);
        if (n >= el && g_ascii_strncasecmp(url + n - el, exts[i], el) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
ns_video_cache_start(ns_video_cache *cache, const ns_node *dom, ns_box *box,
                     const char *abs_url, const char *poster_abs)
{
    guint mse_id = ns_mse_url_id(abs_url);
    ns_video *previous = dom ? ns_video_cache_find_by_node(cache, dom) : NULL;
    if (mse_id && !previous)
        previous = ns_video_cache_find_mse_rollover(cache);
    char *key = mse_id ? g_strdup_printf("ndmse:%u", mse_id)
                       : ns_video_stream_key(abs_url);
    ns_video *v = g_hash_table_lookup(cache->by_url, key);
    if (v) {
        if (box) box->media->video = v;
        if (!mse_id)
            ns_video_note_stream_version(cache, v, dom, abs_url);
        g_free(key);
        return;
    }

    v = g_new0(ns_video, 1);
    v->url = g_strdup(abs_url);
    v->dom_node = dom;
    v->autoplay = attr_present(dom, "autoplay");
    v->loop = attr_present(dom, "loop");
    v->muted = attr_present(dom, "muted");
    v->volume = 1.0;
    v->cur_time = 0.0;
    v->seq = ++cache->next_seq;
    v->seen_gen = cache->discover_gen;
    v->mse_id = mse_id;
    if (mse_id && previous && previous->mse_id != mse_id)
        ns_video_inherit_node_playback(v, previous);
    if (dom && previous && previous->dom_node == dom)
        ns_video_release_node(cache, previous);
    g_hash_table_insert(cache->by_url, key, v);
    if (box) box->media->video = v;
    if (dom) ns_video_apply_node_state(cache, v, dom);
    if (dom) ns_video_discover_track(cache, v, dom);

    if (mse_id) {
        ns_mse_stream *s = g_hash_table_lookup(cache->mse_streams,
                                               GUINT_TO_POINTER(mse_id));
        if (s) ns_video_mse_sync(cache, s, v);
        return;
    }

    ns_pending *pv = g_new0(ns_pending, 1);
    pv->video = v;
    pv->cache = cache;
    g_ptr_array_add(cache->pending, pv);
    ns_net_fetch_async(abs_url, cache->base_url, NULL,
                       on_video_fetched, pv);

    const char *asrc = ns_element_get_attr(dom, "data-audio-src");
    if (asrc && *asrc &&
        (g_str_has_prefix(asrc, "blob:") ||
         g_str_has_prefix(asrc, "https://") ||
         g_str_has_prefix(asrc, "http://") ||
         g_str_has_prefix(asrc, "file://"))) {
        v->audio_url = g_strdup(asrc);
        ns_pending *pa = g_new0(ns_pending, 1);
        pa->video = v;
        pa->cache = cache;
        g_ptr_array_add(cache->pending, pa);
        ns_net_fetch_async(asrc, cache->base_url, NULL,
                           on_msaudio_fetched, pa);
    }

    if (poster_abs && *poster_abs) {
        ns_pending *pp = g_new0(ns_pending, 1);
        pp->video = v;
        pp->cache = cache;
        g_ptr_array_add(cache->pending, pp);
        ns_net_fetch_async(poster_abs, cache->base_url, NULL, on_poster_fetched, pp);
    }
}

static void
ns_video_discover_element(ns_video_cache *cache, const ns_node *node)
{
    if (node->kind == NS_NODE_ELEMENT && node->name &&
        strcmp(node->name, "video") == 0) {
        const char *src = ns_element_get_attr(node, NS_MEDIA_SRC_ATTR);
        if (!src || !*src) src = ns_media_select_source(node);
        if (src && *src) {
            char *abs = g_str_has_prefix(src, "blob:")
                ? g_strdup(src)
                : ns_url_resolve(cache->base_url, src);
            if (abs && url_is_inline_video(abs) &&
                (g_str_has_prefix(abs, "http://") ||
                 g_str_has_prefix(abs, "https://") ||
                 g_str_has_prefix(abs, "file://") ||
                 g_str_has_prefix(abs, "blob:"))) {
                char *skey = ns_video_stream_key(abs);
                ns_video *existing = g_hash_table_lookup(cache->by_url, skey);
                g_free(skey);
                if (existing) {
                    existing->seen_gen = cache->discover_gen;
                    existing->dom_node = node;
                    ns_video_apply_node_state(cache, existing, node);
                    ns_video_discover_track(cache, existing, node);
                    ns_video_note_stream_version(cache, existing, node, abs);
                } else if (!g_hash_table_contains(cache->requested, abs)) {
                    g_hash_table_add(cache->requested, g_strdup(abs));
                    const char *poster_raw =
                        ns_element_get_attr(node, "poster");
                    char *poster = (poster_raw && *poster_raw)
                        ? ns_url_resolve(cache->base_url, poster_raw) : NULL;
                    ns_video_cache_start(cache, node, NULL, abs, poster);
                    g_free(poster);
                }
            }
            g_free(abs);
        }
    }
}

static void
ns_video_release_detached(ns_video_cache *cache)
{
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (v->is_camera || v->seen_gen == cache->discover_gen) continue;
        if (!v->playing && !v->audio_opened) continue;
        ns_video_release_node(cache, v);
    }
}

static void
ns_video_discover_dom(ns_video_cache *cache, const ns_node *root)
{
    for (const ns_node *node = root; node;
         node = ns_node_next_in_subtree(node, root, TRUE))
        ns_video_discover_element(cache, node);
}

void
ns_video_cache_discover(ns_video_cache *cache, const ns_box *root,
                        const ns_node *doc, gint64 now_us)
{
    (void)now_us;
    if (!cache || !root) return;
    if (doc) cache->discover_gen++;

    GPtrArray *vids = g_ptr_array_new();
    ns_layout_collect_videos(root, vids);
    for (guint i = 0; i < vids->len; i++) {
        ns_box *box = g_ptr_array_index(vids, i);
        if (!box->media || !box->dom) continue;
        const char *stream = ns_element_get_attr(box->dom, NS_MEDIA_STREAM_ATTR);
        if (stream && g_strcmp0(stream, "camera") == 0) {
            char *ckey = g_strdup_printf("camera:%p", (const void *)box->dom);
            ns_video *cv = g_hash_table_lookup(cache->by_url, ckey);
            if (!cv) {
                cv = g_new0(ns_video, 1);
                cv->is_camera = TRUE;
                cv->dom_node = box->dom;
                cv->playing = TRUE;
                g_hash_table_insert(cache->by_url, g_strdup(ckey), cv);
            }
            box->media->video = cv;
            g_free(ckey);
            continue;
        }
        const char *src = ns_element_get_attr(box->dom, "src");
        if (!src || !*src)
            src = ns_element_get_attr(box->dom, NS_MEDIA_SRC_ATTR);
        if (!src || !*src)
            src = box->media->video_src;
        if (!src || !*src) continue;
        char *abs = g_str_has_prefix(src, "blob:") ? g_strdup(src)
                                                   : ns_url_resolve(cache->base_url, src);
        if (!abs) continue;
        if ((!g_str_has_prefix(abs, "http://") && !g_str_has_prefix(abs, "https://") &&
             !g_str_has_prefix(abs, "file://") && !g_str_has_prefix(abs, "blob:")) ||
            !url_is_inline_video(abs)) {
            g_free(abs);
            continue;
        }
        char *poster = NULL;
        if (box->media->video_poster)
            poster = ns_url_resolve(cache->base_url, box->media->video_poster);

        char *skey = ns_video_stream_key(abs);
        ns_video *existing = g_hash_table_lookup(cache->by_url, skey);
        g_free(skey);
        if (existing) {
            existing->seen_gen = cache->discover_gen;
            box->media->video = existing;
            if (box->dom) {
                existing->dom_node = box->dom;
                ns_video_apply_node_state(cache, existing, box->dom);
                ns_video_discover_track(cache, existing, box->dom);
            }
            ns_video_note_stream_version(cache, existing, box->dom, abs);
        } else if (!g_hash_table_contains(cache->requested, abs)) {
            g_hash_table_add(cache->requested, g_strdup(abs));
            ns_video_cache_start(cache, box->dom, box, abs, poster);
        }
        g_free(poster);
        g_free(abs);
    }
    if (doc) {
        ns_video_discover_dom(cache, doc);
        ns_video_release_detached(cache);
    }

    g_ptr_array_free(vids, TRUE);
}

typedef struct {
    ns_video   *v;
    const char *kind;
    double      value;
} ns_video_emit_rec;

static void
ns_video_queue_emit(GArray *q, ns_video *v, const char *kind, double value)
{
    ns_video_emit_rec rec = { v, kind, value };
    g_array_append_val(q, rec);
}

static void
ns_video_track_frame_size(ns_video *v, GArray *emits)
{
    if (!v->frame_texture) return;
    int w = ns_texture_get_width(v->frame_texture);
    int h = ns_texture_get_height(v->frame_texture);
    if (w <= 0 || h <= 0 ||
        (w == v->natural_width && h == v->natural_height))
        return;
    v->natural_width = w;
    v->natural_height = h;
    if (!v->meta_sent) return;
    ns_video_queue_emit(emits, v, "vwidth", (double)w);
    ns_video_queue_emit(emits, v, "vheight", (double)h);
}

static void
ns_video_note_tick_gap(ns_video *v, gint64 now_us)
{
    if (v->last_tick_us > 0 && now_us > v->last_tick_us) {
        double gap = (double)(now_us - v->last_tick_us) / 1e6;
        v->stall_peak_s *= exp(-gap / NS_VIDEO_STALL_DECAY_S);
        if (gap > v->stall_peak_s) v->stall_peak_s = MIN(gap, 10.0);
    }
    v->last_tick_us = now_us;
}

static double
ns_video_ahead_seconds(const ns_video *v)
{
    return CLAMP(v->stall_peak_s * 1.5, NS_VIDEO_AHEAD_MIN_S,
                 NS_VIDEO_AHEAD_MAX_S);
}

static guint
ns_video_ahead_frames(const ns_video *v)
{
    int w = v->frame_texture ? ns_texture_get_width(v->frame_texture) : 0;
    int h = v->frame_texture ? ns_texture_get_height(v->frame_texture) : 0;
    gsize bytes = w > 0 && h > 0 ? (gsize)w * (gsize)h * 4u : 0;
    gsize fit = bytes ? NS_VIDEO_AHEAD_BYTES / bytes : NS_VIDEO_LAYER_FRAMES;
    return (guint)CLAMP(fit, 2, NS_VIDEO_LAYER_FRAMES - 1);
}

static void
ns_video_prefetch(ns_video *v, double t)
{
    ns_video_player_prefetch(v->player, t + ns_video_ahead_seconds(v),
                             ns_video_ahead_frames(v));
}

static gboolean
ns_video_preroll(ns_video *v, gint64 now_us, gboolean *changed)
{
    if (v->prerolled || v->loop) return TRUE;
    if (!v->preroll_since_us) v->preroll_since_us = now_us;
    ns_texture *frame = ns_video_player_frame_at(v->player, v->cur_time,
                                                 FALSE, NULL);
    if (frame) {
        ns_texture_unref(v->frame_texture);
        v->frame_texture = ns_texture_ref(frame);
        *changed = TRUE;
    }
    ns_video_prefetch(v, v->cur_time);
    double ahead = -1.0;
    ns_texture *tex = NULL;
    double pts = 0.0;
    guint queued = 0;
    while (ns_video_player_upcoming(v->player, queued, &tex, &pts)) {
        ahead = pts;
        queued++;
    }
    gboolean ready = queued >= ns_video_ahead_frames(v) ||
                     ahead >= v->cur_time + ns_video_ahead_seconds(v) * 0.9 ||
                     now_us - v->preroll_since_us > NS_VIDEO_PREROLL_MAX_US;
    if (!ready) {
        v->base_us = now_us - (gint64)(v->cur_time * 1e6);
        v->prev_tick_time = v->cur_time;
        return FALSE;
    }
    v->prerolled = TRUE;
    v->base_us = now_us - (gint64)(v->cur_time * 1e6);
    return TRUE;
}

gboolean
ns_video_cache_tick(ns_video_cache *cache, gint64 now_us)
{
    if (!cache) return FALSE;
    gboolean changed = FALSE;
    GArray *emits = g_array_new(FALSE, FALSE, sizeof(ns_video_emit_rec));
    GHashTableIter it;
    gpointer key, val;
    guint opened_count = 0;
    ns_video *newest_audible = NULL;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *c = val;
        if (c->player || c->audio_opened) opened_count++;
        if (c->mse_id && c->playing && !c->muted && c->audio_opened &&
            (!newest_audible || c->seq > newest_audible->seq))
            newest_audible = c;
    }
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        gboolean superseded = opened_count >= 2 && newest_audible &&
            v != newest_audible && v->mse_id &&
            v->seq < newest_audible->seq;
        gboolean in_layout = cache->layout_note_gen != 0 &&
            v->layout_note_gen == cache->layout_note_gen;
        gboolean stale = !in_layout && v->last_paint_us > 0 &&
            now_us - v->last_paint_us > (gint64)3000000;
        if (opened_count >= 2 && (v->player || v->audio_opened) &&
            v->playing && (superseded || stale)) {
            ns_video_audio_stop(cache, v);
            v->playing = FALSE;
            opened_count--;
            continue;
        }
        if (v->is_camera) {
            ns_camera *cam = ns_camera_active();
            if (cam) {
                ns_texture *frame = ns_camera_next_frame(cam);
                if (frame) {
                    ns_texture_unref(v->frame_texture);
                    v->frame_texture = frame;
                    v->natural_width  = ns_texture_get_width(frame);
                    v->natural_height = ns_texture_get_height(frame);
                    changed = TRUE;
                }
            }
            continue;
        }
        if (v->mse_id) {
            ns_mse_stream *s = g_hash_table_lookup(
                cache->mse_streams, GUINT_TO_POINTER(v->mse_id));
            if (s) ns_video_mse_sync(cache, s, v);
        }
        ns_video_activate_pending(cache, v);
        if (!v->player) continue;

        if (!v->meta_sent && v->duration > 0.0) {
            v->meta_sent = TRUE;
            if (v->natural_width > 0)
                ns_video_queue_emit(emits, v, "vwidth", (double)v->natural_width);
            if (v->natural_height > 0)
                ns_video_queue_emit(emits, v, "vheight", (double)v->natural_height);
            ns_video_queue_emit(emits, v, "meta", v->duration);
        }
        ns_video_track_frame_size(v, emits);
        if (!v->buf_sent) {
            v->buf_sent = TRUE;
            double buffered_end = ns_video_buffered_end(cache, v);
            if (buffered_end <= 0.0) buffered_end = v->duration;
            double buffered_start = ns_video_buffered_start(cache, v);
            if (buffered_end > 0.0 &&
                (buffered_end != v->sent_buffered_end ||
                 buffered_start != v->sent_buffered_start)) {
                v->sent_buffered_end = buffered_end;
                v->sent_buffered_start = buffered_start;
                ns_video_queue_emit(emits, v, "bufstart", buffered_start);
                ns_video_queue_emit(emits, v, "buf", buffered_end);
            }
        }
        if (!v->playing) {
            v->last_tick_us = 0;
            continue;
        }
        ns_video_note_tick_gap(v, now_us);
        if (!ns_video_preroll(v, now_us, &changed)) continue;

        if (!v->audio_opened && !v->muted &&
            (v->has_audio || v->audio_file)) {
            ns_video_audio_start(cache, v);
            if (v->audio_opened && v->cur_time > 0)
                ns_video_emit_audio(cache, "seek %s %.3f", v->token,
                                    v->cur_time);
        }

        if (v->base_us == 0)
            v->base_us = now_us - (gint64)(v->cur_time * 1e6);
        double elapsed = (double)(now_us - v->base_us) / 1e6;
        if (g_getenv("NS_DBG_CLOCK") && v->mse_id)
            g_printerr("[clock] elapsed=%.2f prev=%.2f cur=%.2f edge=%.2f\n",
                       elapsed, v->prev_tick_time, v->cur_time,
                       ns_video_buffered_end(cache, v));
        if (elapsed - v->prev_tick_time > 1.0 && !v->audio_opened) {
            elapsed = v->prev_tick_time + 1.0;
            v->base_us = now_us - (gint64)(elapsed * 1e6);
            if (v->audio_opened)
                ns_video_emit_audio(cache, "seek %s %.3f", v->token, elapsed);
        }
        double t = elapsed;
        if (v->loop && v->duration > 0.0)
            t = fmod(elapsed, v->duration);

        if (v->mse_window_wanted) {
            v->base_us = now_us - (gint64)(v->cur_time * 1e6);
            v->prev_tick_time = v->cur_time;
            if (!v->stalled) {
                v->stalled = TRUE;
                ns_video_queue_emit(emits, v, "waiting", v->cur_time);
            }
            continue;
        }
        if (ns_video_stream_growing(cache, v)) {
            double edge = (v->mse_id ? ns_video_mse_window_edge(v)
                                     : ns_video_buffered_end(cache, v)) - 0.35;
            if (edge < v->prev_tick_time) edge = v->prev_tick_time;
            if (edge < 0) edge = 0;
            if (t > edge) {
                ns_video_refresh_growing_stream(cache, v, now_us);
                v->base_us = now_us - (gint64)(edge * 1e6);
                t = edge;
                if (!v->stall_since_us)
                    v->stall_since_us = now_us;
                if (!v->stalled &&
                    now_us - v->stall_since_us > G_GINT64_CONSTANT(1500000)) {
                    v->stalled = TRUE;
                    ns_video_queue_emit(emits, v, "waiting", edge);
                }
            } else {
                v->stall_since_us = 0;
            }
        }

        if (v->loop && t + 1e-3 < v->prev_tick_time)
            ns_video_audio_resync(cache, v);
        v->prev_tick_time = t;

        gboolean ended = FALSE;
        ns_texture *frame = ns_video_player_frame_at(v->player, t, v->loop,
                                                     &ended);
        if (frame) {
            ns_texture_unref(v->frame_texture);
            v->frame_texture = ns_texture_ref(frame);
            if (!ns_video_is_layered(cache, v)) changed = TRUE;
            if (v->stalled) {
                v->stalled = FALSE;
                ns_video_queue_emit(emits, v, "resumed", t);
            }
        }
        v->cur_time = t;
        if (!v->loop && ns_video_is_layered(cache, v))
            ns_video_prefetch(v, t);
        if (t - v->last_emit_time >= 0.20 || ended) {
            v->last_emit_time = t;
            ns_video_queue_emit(emits, v, "pos", v->cur_time);
        }
        if (ended) {
            if (ns_video_stream_growing(cache, v)) {
                v->prev_tick_time = v->cur_time;
                v->base_us = now_us - (gint64)(v->cur_time * 1e6);
                if (!v->stalled) {
                    v->stalled = TRUE;
                    ns_video_queue_emit(emits, v, "waiting", v->cur_time);
                }
                ns_video_refresh_growing_stream(cache, v, now_us);
                continue;
            }
            v->playing = FALSE;
            v->ended = TRUE;
            v->cur_time = v->duration;
            ns_video_audio_stop(cache, v);
            ns_video_queue_emit(emits, v, "ended", v->duration);
        }
    }
    for (guint ei = 0; ei < emits->len; ei++) {
        ns_video_emit_rec *rec =
            &g_array_index(emits, ns_video_emit_rec, ei);
        ns_video_emit_js(cache, rec->v, rec->kind, rec->value);
    }
    g_array_free(emits, TRUE);
    return changed;
}

gboolean
ns_video_cache_has_pending(const ns_video_cache *cache)
{
    return cache && cache->pending->len > 0;
}

gboolean
ns_video_cache_waiting_growth(const ns_video_cache *cache)
{
    if (!cache) return FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, ((ns_video_cache *)cache)->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (v->playing && v->stalled) return TRUE;
    }
    return FALSE;
}

void
ns_video_cache_set_layer_gen(ns_video_cache *cache, guint gen)
{
    if (cache) cache->layer_gen = gen;
}

guint
ns_video_cache_layer_gen(const ns_video_cache *cache)
{
    return cache ? cache->layer_gen : 0;
}

gboolean
ns_video_is_layered(const ns_video_cache *cache, const ns_video *v)
{
    return cache && v && cache->layer_gen != 0 &&
           v->layer_gen == cache->layer_gen && v->frame_texture;
}

guint
ns_video_layer_frames(const ns_video *v, ns_video_frame_ref *out, guint max)
{
    if (!v || !v->frame_texture || max == 0) return 0;
    out[0].tex = v->frame_texture;
    out[0].serial = ns_video_player_current_serial(v->player);
    out[0].deadline_us = v->base_us + (gint64)(v->cur_time * 1e6);
    guint n = 1;
    if (!v->playing || v->loop || v->base_us == 0) return n;
    for (guint i = 0; n < max; i++) {
        ns_texture *tex = NULL;
        double pts = 0.0;
        guint serial = ns_video_player_upcoming(v->player, i, &tex, &pts);
        if (!serial || !tex) break;
        out[n].tex = tex;
        out[n].serial = serial;
        out[n].deadline_us = v->base_us + (gint64)(pts * 1e6);
        n++;
    }
    return n;
}

gboolean
ns_video_cache_layer_frames_pending(ns_video_cache *cache)
{
    if (!cache) return FALSE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (!ns_video_is_layered(cache, v)) continue;
        ns_video_frame_ref frames[NS_VIDEO_LAYER_FRAMES];
        guint n = ns_video_layer_frames(v, frames, NS_VIDEO_LAYER_FRAMES);
        for (guint i = 0; i < n; i++)
            if (frames[i].serial > v->layer_sent_serial) return TRUE;
    }
    return FALSE;
}

void
ns_video_cache_foreach_layered(ns_video_cache *cache,
                               void (*fn)(ns_video *v, gpointer data),
                               gpointer data)
{
    if (!cache || !fn) return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val))
        if (ns_video_is_layered(cache, val)) fn(val, data);
}

gboolean
ns_video_cache_animating(const ns_video_cache *cache)
{
    if (!cache) return FALSE;
    if (cache->pending->len > 0) return TRUE;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, cache->by_url);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        ns_video *v = val;
        if (v->is_camera && ns_camera_active()) return TRUE;
        if (v->player && v->playing) return TRUE;
    }
    return FALSE;
}
