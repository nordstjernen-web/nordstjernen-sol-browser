/* Nordstjernen — inline video playback and poster cache.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_VIDEO_H
#define NS_VIDEO_H

#include <glib.h>

#include "texture.h"

G_BEGIN_DECLS

typedef struct ns_box ns_box;
typedef struct ns_node ns_node;

typedef struct ns_video {
    char        *url;
    int          natural_width;
    int          natural_height;
    ns_texture  *poster_texture;
    char        *poster_url;
    ns_texture  *frame_texture;
    gboolean     loaded;
    gboolean     load_events_fired;
    gboolean     failed;

    void        *player;
    const void  *dom_node;
    gboolean     is_camera;
    gboolean     autoplay;
    gboolean     loop;
    gboolean     muted;
    gboolean     has_audio;
    gboolean     audio_opened;
    gboolean     audio_fetching;
    char        *audio_url;
    char        *audio_file;
    gsize        audio_file_len;
    guint        audio_file_gen;
    double       audio_timeline_start;
    char        *pending_audio_file;
    gsize        pending_audio_file_len;
    guint        pending_audio_file_gen;
    double       pending_audio_start;
    GByteArray  *mse_window;
    guint        mse_window_gen;
    guint        mse_window_serial;
    gboolean     mse_window_ranged;
    gboolean     mse_window_wanted;
    double       mse_window_start;
    double       mse_window_end;
    gint64       last_paint_us;
    guint        layout_note_gen;
    guint        seen_gen;
    char        *token;
    gboolean     playing;
    gboolean     ended;
    double       volume;
    gboolean     meta_sent;
    gboolean     buf_sent;
    double       sent_buffered_end;
    double       sent_buffered_start;
    guint        seq;
    gint64       base_us;
    gint64       last_refresh_us;
    gboolean     stalled;
    gint64       stall_since_us;
    guint        mse_id;
    double       cur_time;
    double       prev_tick_time;
    double       last_emit_time;
    double       duration;
    GPtrArray   *cues;
    gboolean     track_requested;
    gboolean     sampled;
    char        *cors_allow_origin;
    guint        layer_gen;
    double       layer_x, layer_y, layer_w, layer_h;
    guint        layer_sent_serial;
    gboolean     prerolled;
    gint64       preroll_since_us;
    gint64       last_tick_us;
    double       stall_peak_s;
} ns_video;

#define NS_VIDEO_LAYER_FRAMES 48

typedef struct ns_video_frame_ref {
    ns_texture *tex;
    guint       serial;
    gint64      deadline_us;
} ns_video_frame_ref;

typedef struct ns_video_cache ns_video_cache;
typedef void (*ns_video_js_cb)(const void *dom_node, const char *kind,
                               double value, gpointer user_data);
typedef void (*ns_video_audio_cb)(const char *command, gpointer user_data);

ns_video_cache *ns_video_cache_new(void);
void            ns_video_cache_free(ns_video_cache *cache);
ns_texture     *ns_video_cache_frame_for_node(ns_video_cache *cache,
                                              const void *dom_node,
                                              const char **out_url,
                                              const char **out_cors);
void            ns_video_cache_set_base(ns_video_cache *cache,
                                        const char *base_url);
void            ns_video_cache_set_js_cb(ns_video_cache *cache,
                                         ns_video_js_cb cb, gpointer user_data);
void            ns_video_cache_set_audio_cb(ns_video_cache *cache,
                                            ns_video_audio_cb cb,
                                            gpointer user_data);

void     ns_video_cache_discover(ns_video_cache *cache, const ns_box *root,
                                 const ns_node *doc, gint64 now_us);
void     ns_video_cache_note_layout(ns_video_cache *cache, const ns_box *root);
gboolean ns_video_cache_tick(ns_video_cache *cache, gint64 now_us);
gboolean ns_video_cache_animating(const ns_video_cache *cache);
gboolean ns_video_cache_waiting_growth(const ns_video_cache *cache);
gboolean ns_video_cache_mse_append(ns_video_cache *cache, guint stream_id,
                                   char kind, const guint8 *data, gsize len);
void     ns_video_cache_mse_eos(ns_video_cache *cache, guint stream_id);
double   ns_video_cache_mse_buffered(ns_video_cache *cache, guint stream_id,
                                     char kind, int index, double *start);
gboolean ns_video_cache_mse_remove(ns_video_cache *cache, guint stream_id,
                                   char kind, double start, double end);
gsize    ns_video_cache_mse_bytes(ns_video_cache *cache, guint stream_id,
                                  char kind);
gboolean ns_video_cache_has_pending(const ns_video_cache *cache);
void     ns_video_cache_set_layer_gen(ns_video_cache *cache, guint gen);
guint    ns_video_cache_layer_gen(const ns_video_cache *cache);
gboolean ns_video_is_layered(const ns_video_cache *cache, const ns_video *v);
gboolean ns_video_cache_layer_frames_pending(ns_video_cache *cache);
guint    ns_video_layer_frames(const ns_video *v, ns_video_frame_ref *out,
                               guint max);
void     ns_video_cache_foreach_layered(ns_video_cache *cache,
                                        void (*fn)(ns_video *v, gpointer data),
                                        gpointer data);

gboolean ns_video_url_is_inline(const char *url);
gboolean ns_video_cache_seek_node(ns_video_cache *cache, const void *dom_node,
                                  double seconds, gint64 now_us);
gboolean ns_video_cache_set_node_playing(ns_video_cache *cache,
                                         const void *dom_node, gboolean play,
                                         gint64 now_us);
gboolean ns_video_cache_set_node_muted(ns_video_cache *cache,
                                       const void *dom_node, gboolean muted);
gboolean ns_video_cache_set_node_volume(ns_video_cache *cache,
                                        const void *dom_node, double volume);
gboolean ns_video_cache_toggle(ns_video_cache *cache, ns_video *v, gint64 now_us);
gboolean ns_video_toggle(ns_video *v, gint64 now_us);
void     ns_video_play(ns_video *v, gint64 now_us);
void     ns_video_pause(ns_video *v, gint64 now_us);
void     ns_video_note_painted(ns_video *v);
const char *ns_video_active_cue_text(const ns_video *v);

G_END_DECLS

#endif
