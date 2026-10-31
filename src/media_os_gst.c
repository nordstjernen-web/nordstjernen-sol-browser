/* Nordstjernen — H.264 and AAC decoding through the system GStreamer (VA-API, V4L2 or software).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "media_os.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <stdlib.h>
#include <string.h>

#define NS_GST_INFLIGHT 32

struct ns_os_vdec {
    char       *desc;
    GstElement *pipeline;
    GstAppSrc  *src;
    GstAppSink *sink;
    GstCaps    *caps;
    int         inflight;
    int         draining;
};

struct ns_os_adec {
    GstElement *pipeline;
    GstAppSrc  *src;
    GstAppSink *sink;
    GstCaps    *caps;
};

static int
gst_ready(void)
{
    if (gst_is_initialized()) return 1;
    return gst_init_check(NULL, NULL, NULL);
}

static GList *
gst_decoders_for(const char *caps_str)
{
    GList *all = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    GstCaps *caps = gst_caps_from_string(caps_str);
    GList *hits = gst_element_factory_list_filter(all, caps, GST_PAD_SINK, FALSE);
    gst_caps_unref(caps);
    gst_plugin_feature_list_free(all);
    return hits;
}

static int
gst_has_decoder(const char *caps_str, int *cache)
{
    if (*cache >= 0) return *cache;
    if (!gst_ready()) return *cache = 0;
    GList *hits = gst_decoders_for(caps_str);
    *cache = hits != NULL;
    for (GList *l = hits; l; l = l->next) {
        GstPluginFeature *loaded = gst_plugin_feature_load(GST_PLUGIN_FEATURE(l->data));
        if (loaded) gst_object_unref(loaded);
    }
    gst_plugin_feature_list_free(hits);
    return *cache;
}

static int g_h264 = -1;
static int g_aac = -1;

typedef struct {
    const char *codec;
    const char *caps;
    const char *parser;
    char       *factory;
    int         probed;
} gst_hw_codec;

static gst_hw_codec g_hw_codecs[] = {
    { "vp9",  "video/x-vp9", "vp9parse", NULL, 0 },
    { "av1",  "video/x-av1", "av1parse", NULL, 0 },
    { "hevc", "video/x-h265", "h265parse", NULL, 0 },
};

static gst_hw_codec *
gst_hw_lookup(const char *codec)
{
    if (!codec) return NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(g_hw_codecs); i++)
        if (strcmp(g_hw_codecs[i].codec, codec) == 0) return &g_hw_codecs[i];
    return NULL;
}

static gboolean
gst_factory_is_hardware(GstElementFactory *f)
{
    const char *klass =
        gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
    return klass && strstr(klass, "Hardware") != NULL;
}

static gboolean
gst_element_exists(const char *name)
{
    GstElementFactory *f = gst_element_factory_find(name);
    if (!f) return FALSE;
    gst_object_unref(f);
    return TRUE;
}

static gst_hw_codec *
gst_hw_probe(const char *codec)
{
    gst_hw_codec *c = gst_hw_lookup(codec);
    if (!c) return NULL;
    if (c->probed) return c;
    c->probed = 1;
    if (getenv("NS_MEDIA_NO_OS_DECODER") || getenv("NS_MEDIA_NO_HW_DECODER") ||
        !gst_ready())
        return c;
    GList *hits = g_list_sort(gst_decoders_for(c->caps),
                              gst_plugin_feature_rank_compare_func);
    for (GList *l = hits; l && !c->factory; l = l->next) {
        GstElementFactory *f = l->data;
        if (strstr(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)), "alpha"))
            continue;
        if (!gst_factory_is_hardware(f) &&
            !getenv("NS_MEDIA_GST_SOFTWARE_DECODER"))
            continue;
        GstPluginFeature *loaded = gst_plugin_feature_load(GST_PLUGIN_FEATURE(f));
        if (!loaded) continue;
        c->factory = g_strdup(gst_plugin_feature_get_name(loaded));
        gst_object_unref(loaded);
    }
    gst_plugin_feature_list_free(hits);
    return c;
}

int
ns_os_hw_video_available(const char *codec)
{
    gst_hw_codec *c = gst_hw_probe(codec);
    return c && c->factory != NULL;
}

void
ns_os_media_preinit(void)
{
    if (getenv("NS_MEDIA_NO_OS_DECODER")) return;
    ns_os_h264_available();
    ns_os_aac_available();
    for (size_t i = 0; i < G_N_ELEMENTS(g_hw_codecs); i++)
        gst_hw_probe(g_hw_codecs[i].codec);
}

const char *
ns_os_media_backend(void)
{
    return "GStreamer";
}

int
ns_os_h264_available(void)
{
    return gst_has_decoder("video/x-h264", &g_h264);
}

int
ns_os_aac_available(void)
{
    return gst_has_decoder("audio/mpeg, mpegversion=(int)4", &g_aac);
}

static GstElement *
gst_build(const char *desc, GstCaps *caps, GstAppSrc **src, GstAppSink **sink)
{
    GError *err = NULL;
    GstElement *p = gst_parse_launch(desc, &err);
    if (!p) {
        if (err) g_error_free(err);
        return NULL;
    }
    if (err) g_error_free(err);
    GstElement *s = gst_bin_get_by_name(GST_BIN(p), "src");
    GstElement *k = gst_bin_get_by_name(GST_BIN(p), "sink");
    if (!s || !k) {
        if (s) gst_object_unref(s);
        if (k) gst_object_unref(k);
        gst_object_unref(p);
        return NULL;
    }
    *src = GST_APP_SRC(s);
    *sink = GST_APP_SINK(k);
    gst_app_src_set_caps(*src, caps);
    g_object_set(s, "format", GST_FORMAT_TIME, "is-live", FALSE, NULL);
    g_object_set(k, "sync", FALSE, "max-buffers", 32, NULL);
    if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(s);
        gst_object_unref(k);
        gst_object_unref(p);
        *src = NULL;
        *sink = NULL;
        return NULL;
    }
    return p;
}

static void
gst_teardown(GstElement **pipeline, GstAppSrc **src, GstAppSink **sink)
{
    if (*pipeline) {
        gst_element_set_state(*pipeline, GST_STATE_NULL);
        gst_object_unref(*pipeline);
    }
    if (*src) gst_object_unref(*src);
    if (*sink) gst_object_unref(*sink);
    *pipeline = NULL;
    *src = NULL;
    *sink = NULL;
}

static int
gst_failed(GstElement *pipeline)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    if (!bus) return 0;
    GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!msg) return 0;
    gst_message_unref(msg);
    return 1;
}

static const char ns_gst_video_desc[] =
    "appsrc name=src ! decodebin ! videoconvert ! "
    "video/x-raw,format=NV12 ! appsink name=sink";

static const char ns_gst_audio_desc[] =
    "appsrc name=src ! decodebin ! audioconvert ! "
    "audio/x-raw,format=F32LE,layout=interleaved ! appsink name=sink";

static int
vdec_start(ns_os_vdec *d)
{
    d->pipeline = gst_build(d->desc ? d->desc : ns_gst_video_desc, d->caps,
                            &d->src, &d->sink);
    d->inflight = 0;
    d->draining = 0;
    return d->pipeline != NULL;
}

ns_os_vdec *
ns_os_vdec_new_h264(const uint8_t *avcc, size_t avcc_len, int width, int height)
{
    if (!avcc || avcc_len < 7 || !ns_os_h264_available()) return NULL;
    ns_os_vdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    GstBuffer *cd = gst_buffer_new_memdup(avcc, avcc_len);
    d->caps = gst_caps_new_simple("video/x-h264",
                                  "stream-format", G_TYPE_STRING, "avc",
                                  "alignment", G_TYPE_STRING, "au",
                                  "codec_data", GST_TYPE_BUFFER, cd, NULL);
    gst_buffer_unref(cd);
    if (width > 0 && height > 0)
        gst_caps_set_simple(d->caps, "width", G_TYPE_INT, width,
                            "height", G_TYPE_INT, height, NULL);
    if (!vdec_start(d)) {
        ns_os_vdec_free(d);
        return NULL;
    }
    return d;
}

static GstCaps *
gst_hw_caps(const gst_hw_codec *c, const uint8_t *extradata,
            size_t extradata_len, int width, int height)
{
    GstCaps *caps = gst_caps_new_empty_simple(c->caps);
    if (strcmp(c->codec, "av1") == 0)
        gst_caps_set_simple(caps, "stream-format", G_TYPE_STRING, "obu-stream",
                            "alignment", G_TYPE_STRING, "tu", NULL);
    if (strcmp(c->codec, "hevc") == 0) {
        if (!extradata || extradata_len < 23 || extradata[0] != 1) {
            gst_caps_unref(caps);
            return NULL;
        }
        gst_caps_set_simple(caps, "stream-format", G_TYPE_STRING, "hvc1",
                            "alignment", G_TYPE_STRING, "au", NULL);
    }
    if (extradata && extradata_len > 0 && strcmp(c->codec, "vp9") != 0) {
        GstBuffer *cd = gst_buffer_new_memdup(extradata, extradata_len);
        gst_caps_set_simple(caps, "codec_data", GST_TYPE_BUFFER, cd, NULL);
        gst_buffer_unref(cd);
    }
    if (width > 0 && height > 0)
        gst_caps_set_simple(caps, "width", G_TYPE_INT, width,
                            "height", G_TYPE_INT, height, NULL);
    return caps;
}

ns_os_vdec *
ns_os_vdec_new_hw(const char *codec, const uint8_t *extradata,
                  size_t extradata_len, int width, int height)
{
    gst_hw_codec *c = gst_hw_probe(codec);
    if (!c || !c->factory) return NULL;
    GstCaps *caps = gst_hw_caps(c, extradata, extradata_len, width, height);
    if (!caps) return NULL;
    ns_os_vdec *d = calloc(1, sizeof *d);
    if (!d) {
        gst_caps_unref(caps);
        return NULL;
    }
    d->caps = caps;
    d->desc = g_strdup_printf(
        "appsrc name=src ! %s%s%s ! videoconvert ! "
        "video/x-raw,format=NV12 ! appsink name=sink",
        gst_element_exists(c->parser) ? c->parser : "",
        gst_element_exists(c->parser) ? " ! " : "", c->factory);
    if (!vdec_start(d)) {
        ns_os_vdec_free(d);
        return NULL;
    }
    return d;
}

const char *
ns_os_hw_video_decoder_name(const char *codec)
{
    gst_hw_codec *c = gst_hw_lookup(codec);
    return c ? c->factory : NULL;
}

int
ns_os_vdec_send(ns_os_vdec *d, const uint8_t *data, size_t len, int64_t pts_us)
{
    if (!d || !d->src || !data || !len) return 0;
    GstBuffer *buf = gst_buffer_new_memdup(data, len);
    GST_BUFFER_PTS(buf) = (GstClockTime)pts_us * GST_USECOND;
    if (gst_app_src_push_buffer(d->src, buf) != GST_FLOW_OK) return 0;
    d->inflight++;
    return 1;
}

static int
vdec_convert(GstSample *sample, ns_os_frame *out)
{
    GstVideoInfo info;
    GstVideoFrame frame;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!buf || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        !gst_video_frame_map(&frame, &info, buf, GST_MAP_READ))
        return 0;
    int ok = ns_os_frame_copy_nv12(out,
        GST_VIDEO_FRAME_PLANE_DATA(&frame, 0), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
        GST_VIDEO_FRAME_PLANE_DATA(&frame, 1), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1),
        GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info));
    gst_video_frame_unmap(&frame);
    if (!ok) return 0;
    out->full_range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255;
    out->colorspace = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? 709
                    : info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601 ? 601
                    : 0;
    out->pts_us = GST_BUFFER_PTS_IS_VALID(buf)
        ? (int64_t)(GST_BUFFER_PTS(buf) / GST_USECOND) : 0;
    return 1;
}

ns_os_status
ns_os_vdec_receive(ns_os_vdec *d, ns_os_frame *out)
{
    memset(out, 0, sizeof *out);
    if (!d || !d->sink) return NS_OS_ERROR;
    GstClockTime wait = d->draining ? 500 * GST_MSECOND
                      : d->inflight >= NS_GST_INFLIGHT ? 20 * GST_MSECOND : 0;
    GstSample *sample = gst_app_sink_try_pull_sample(d->sink, wait);
    if (!sample) return gst_failed(d->pipeline) ? NS_OS_ERROR : NS_OS_AGAIN;
    if (d->inflight > 0) d->inflight--;
    int ok = vdec_convert(sample, out);
    gst_sample_unref(sample);
    return ok ? NS_OS_FRAME : NS_OS_ERROR;
}

void
ns_os_vdec_drain(ns_os_vdec *d)
{
    if (!d || !d->src) return;
    gst_app_src_end_of_stream(d->src);
    d->draining = 1;
}

void
ns_os_vdec_flush(ns_os_vdec *d)
{
    if (!d) return;
    gst_teardown(&d->pipeline, &d->src, &d->sink);
    vdec_start(d);
}

void
ns_os_vdec_free(ns_os_vdec *d)
{
    if (!d) return;
    gst_teardown(&d->pipeline, &d->src, &d->sink);
    if (d->caps) gst_caps_unref(d->caps);
    g_free(d->desc);
    free(d);
}

ns_os_adec *
ns_os_adec_new_aac(const uint8_t *asc, size_t asc_len, int rate, int channels)
{
    if (!asc || !asc_len || !ns_os_aac_available()) return NULL;
    int cfg_rate = 0, cfg_ch = 0;
    if (ns_os_aac_config(asc, asc_len, &cfg_rate, &cfg_ch, NULL)) {
        if (rate <= 0) rate = cfg_rate;
        if (channels <= 0) channels = cfg_ch;
    }
    if (rate <= 0 || channels <= 0) return NULL;
    ns_os_adec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    GstBuffer *cd = gst_buffer_new_memdup(asc, asc_len);
    d->caps = gst_caps_new_simple("audio/mpeg",
                                  "mpegversion", G_TYPE_INT, 4,
                                  "stream-format", G_TYPE_STRING, "raw",
                                  "rate", G_TYPE_INT, rate,
                                  "channels", G_TYPE_INT, channels,
                                  "codec_data", GST_TYPE_BUFFER, cd, NULL);
    gst_buffer_unref(cd);
    d->pipeline = gst_build(ns_gst_audio_desc, d->caps, &d->src, &d->sink);
    if (!d->pipeline) {
        ns_os_adec_free(d);
        return NULL;
    }
    return d;
}

static int
adec_append(GstSample *sample, float **pcm, size_t *frames, size_t *cap,
            int *rate, int *channels)
{
    GstStructure *st = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    int r = 0, ch = 0;
    if (!gst_structure_get_int(st, "rate", &r) ||
        !gst_structure_get_int(st, "channels", &ch) || ch < 1 || ch > 8)
        return 0;
    if (*channels && (ch != *channels || r != *rate)) return 0;
    *rate = r;
    *channels = ch;
    GstMapInfo map;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    if (!buf || !gst_buffer_map(buf, &map, GST_MAP_READ)) return 0;
    size_t n = map.size / (sizeof(float) * (size_t)ch);
    size_t need = (*frames + n) * (size_t)ch;
    int ok = 1;
    if (need > *cap) {
        size_t ncap = *cap ? *cap * 2 : 8192;
        while (ncap < need) ncap *= 2;
        float *grown = realloc(*pcm, ncap * sizeof(float));
        if (grown) {
            *pcm = grown;
            *cap = ncap;
        } else {
            ok = 0;
        }
    }
    if (ok) {
        memcpy(*pcm + *frames * (size_t)ch, map.data, n * (size_t)ch * sizeof(float));
        *frames += n;
    }
    gst_buffer_unmap(buf, &map);
    return ok;
}

int
ns_os_adec_decode(ns_os_adec *d, const uint8_t *data, size_t len,
                  float **out_pcm, size_t *out_frames, int *out_rate,
                  int *out_channels)
{
    *out_pcm = NULL;
    *out_frames = 0;
    if (!d || !d->src) return 0;
    GstClockTime wait = 0;
    if (data && len) {
        gst_app_src_push_buffer(d->src, gst_buffer_new_memdup(data, len));
    } else {
        gst_app_src_end_of_stream(d->src);
        wait = 500 * GST_MSECOND;
    }
    float *pcm = NULL;
    size_t frames = 0, cap = 0;
    int rate = 0, ch = 0;
    GstSample *sample;
    while ((sample = gst_app_sink_try_pull_sample(d->sink, wait)) != NULL) {
        adec_append(sample, &pcm, &frames, &cap, &rate, &ch);
        gst_sample_unref(sample);
        wait = 0;
    }
    if (!frames) {
        free(pcm);
        return 0;
    }
    *out_pcm = pcm;
    *out_frames = frames;
    *out_rate = rate;
    *out_channels = ch;
    return 1;
}

void
ns_os_adec_flush(ns_os_adec *d)
{
    if (!d) return;
    gst_teardown(&d->pipeline, &d->src, &d->sink);
    d->pipeline = gst_build(ns_gst_audio_desc, d->caps, &d->src, &d->sink);
}

void
ns_os_adec_free(ns_os_adec *d)
{
    if (!d) return;
    gst_teardown(&d->pipeline, &d->src, &d->sink);
    if (d->caps) gst_caps_unref(d->caps);
    free(d);
}
