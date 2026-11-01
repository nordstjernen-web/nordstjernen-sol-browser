/* Nordstjernen — inline video decoding: MPEG-1 and libav-backed containers.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "video_decode.h"

#include <stdio.h>
#include <string.h>

#include "debuglog.h"
#include "media_os.h"
#include "pl_mpeg.h"
#ifdef NS_HAVE_OS_MP4
#include "video_mp4.h"
#endif

#ifdef NS_HAVE_LIBAV
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

#ifdef NS_HAVE_LIBAV
#define NS_LAV_MAX_THREADS 8

typedef struct {
    AVFormatContext   *fmt;
    AVIOContext       *avio;
    AVCodecContext    *vdec;
    ns_os_vdec        *osdec;
    enum AVCodecID     codec_id;
    gboolean           pkt_pending;
    struct SwsContext *sws;
    AVPacket          *pkt;
    AVFrame           *frame;
    guint8            *data;
    gsize              len;
    gsize              cap;
    gsize              pos;
    int                vstream;
    AVRational         time_base;
    gboolean           draining;
    gboolean           reopen_on_growth;
    gsize              opened_len;
    int64_t            last_dts;
    int64_t            resume_after_dts;
    double             last_out_pts;
    double             skip_until_pts;
    double             skip_before_pts;
    ns_os_frame        osframe;
    gboolean           os_produced;
} ns_lav;
#endif

struct ns_video_player {
    plm_t      *plm;
#ifdef NS_HAVE_LIBAV
    ns_lav     *lav;
#endif
#ifdef NS_HAVE_OS_MP4
    ns_mp4_player *mp4;
#endif
    int         width;
    int         height;
    int         has_audio;
    double      duration;
    double      demuxed_end;
    ns_texture *cur_tex;
    double      cur_time;
    guint       cur_serial;
    ns_texture *pending_tex;
    double      pending_time;
    guint       pending_serial;
    GArray     *ahead;
    guint       next_serial;
};

typedef struct ns_vframe {
    ns_texture *tex;
    double      pts;
    guint       serial;
} ns_vframe;

static void
ns_vframe_clear(gpointer p)
{
    ns_texture_unref(((ns_vframe *)p)->tex);
}

static void
ns_video_player_drop_ahead(ns_video_player *player)
{
    if (player->ahead) g_array_set_size(player->ahead, 0);
}

static gboolean
ns_video_player_take_ahead(ns_video_player *player)
{
    if (!player->ahead || player->ahead->len == 0) return FALSE;
    ns_vframe f = g_array_index(player->ahead, ns_vframe, 0);
    g_array_index(player->ahead, ns_vframe, 0).tex = NULL;
    g_array_remove_index(player->ahead, 0);
    ns_texture_unref(player->pending_tex);
    player->pending_tex = f.tex;
    player->pending_time = f.pts;
    player->pending_serial = f.serial;
    return TRUE;
}

static gboolean
bytes_are_mpeg1(const guint8 *bytes, gsize len)
{
    return bytes && len >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 &&
           bytes[2] == 0x01 && (bytes[3] == 0xBA || bytes[3] == 0xB3);
}

#ifdef NS_HAVE_LIBAV
static gboolean
bytes_are_matroska(const guint8 *bytes, gsize len)
{
    return bytes && len >= 4 && bytes[0] == 0x1A && bytes[1] == 0x45 &&
           bytes[2] == 0xDF && bytes[3] == 0xA3;
}

static gboolean
bytes_are_isobmff(const guint8 *bytes, gsize len)
{
    return bytes && len >= 12 && memcmp(bytes + 4, "ftyp", 4) == 0;
}

static gboolean
bytes_are_ogg(const guint8 *bytes, gsize len)
{
    return bytes && len >= 4 && memcmp(bytes, "OggS", 4) == 0;
}
#endif

#ifdef NS_HAVE_LIBAV
static int
ns_lav_read(void *opaque, uint8_t *buf, int sz)
{
    ns_lav *L = opaque;
    gsize avail = L->len - L->pos;
    if (avail == 0) return AVERROR_EOF;
    if ((gsize)sz > avail) sz = (int)avail;
    memcpy(buf, L->data + L->pos, (gsize)sz);
    L->pos += (gsize)sz;
    return sz;
}

static int64_t
ns_lav_seek(void *opaque, int64_t off, int whence)
{
    ns_lav *L = opaque;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return (int64_t)L->len;
    int64_t base = (whence == SEEK_CUR) ? (int64_t)L->pos
                 : (whence == SEEK_END) ? (int64_t)L->len : 0;
    int64_t np = base + off;
    if (np < 0 || (gsize)np > L->len) return -1;
    L->pos = (gsize)np;
    return np;
}

static void
ns_lav_free(ns_lav *L)
{
    if (!L) return;
    if (L->sws) sws_freeContext(L->sws);
    if (L->frame) av_frame_free(&L->frame);
    if (L->pkt) av_packet_free(&L->pkt);
    if (L->vdec) avcodec_free_context(&L->vdec);
    if (L->osdec) ns_os_vdec_free(L->osdec);
    ns_os_frame_clear(&L->osframe);
    if (L->fmt) avformat_close_input(&L->fmt);
    if (L->avio) { av_freep(&L->avio->buffer); avio_context_free(&L->avio); }
    g_free(L->data);
    g_free(L);
}

static void
ns_lav_enable_threads(AVCodecContext *ctx)
{
    ctx->thread_count = MIN((int)g_get_num_processors(), NS_LAV_MAX_THREADS);
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
}

static gboolean
ns_lav_container_described(AVFormatContext *fmt)
{
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) return FALSE;
    const AVCodecParameters *par = fmt->streams[vi]->codecpar;
    return par->codec_id != AV_CODEC_ID_NONE && par->width > 0 &&
           par->height > 0;
}

static const char *
ns_lav_hw_codec_name(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_VP9:  return "vp9";
    case AV_CODEC_ID_AV1:  return "av1";
    case AV_CODEC_ID_HEVC: return "hevc";
    default:               return NULL;
    }
}

static gboolean
ns_lav_open_hw_decoder(ns_lav *L, AVStream *vs)
{
    const char *hw = ns_lav_hw_codec_name(L->codec_id);
    if (!hw || g_getenv("NS_MEDIA_NO_OS_DECODER") ||
        !ns_os_hw_video_available(hw))
        return FALSE;
    L->osdec = ns_os_vdec_new_hw(hw, vs->codecpar->extradata,
                                 vs->codecpar->extradata_size > 0
                                     ? (size_t)vs->codecpar->extradata_size : 0,
                                 vs->codecpar->width, vs->codecpar->height);
    if (!L->osdec) return FALSE;
    ns_debug_log_emit(NS_DLOG_INFO, "media",
                      "%s %dx%d decoding through %s (%s)",
                      avcodec_get_name(L->codec_id), vs->codecpar->width,
                      vs->codecpar->height, ns_os_media_backend(),
                      ns_os_hw_video_decoder_name(hw));
    return TRUE;
}

static gboolean
ns_lav_open_decoder(ns_lav *L, AVStream *vs)
{
    if (ns_lav_open_hw_decoder(L, vs)) return TRUE;
    if (L->codec_id == AV_CODEC_ID_H264 && vs->codecpar->extradata &&
        vs->codecpar->extradata_size > 6 && vs->codecpar->extradata[0] == 1 &&
        !g_getenv("NS_MEDIA_NO_OS_DECODER")) {
        L->osdec = ns_os_vdec_new_h264(vs->codecpar->extradata,
                                       (size_t)vs->codecpar->extradata_size,
                                       vs->codecpar->width,
                                       vs->codecpar->height);
        if (L->osdec) {
            ns_debug_log_emit(NS_DLOG_INFO, "media",
                              "H.264 %dx%d decoding through %s",
                              vs->codecpar->width, vs->codecpar->height,
                              ns_os_media_backend());
            return TRUE;
        }
    }
    const AVCodec *dec = avcodec_find_decoder(L->codec_id);
    if (!dec) return FALSE;
    L->vdec = avcodec_alloc_context3(dec);
    if (!L->vdec) return FALSE;
    if (avcodec_parameters_to_context(L->vdec, vs->codecpar) < 0) return FALSE;
    ns_lav_enable_threads(L->vdec);
    return avcodec_open2(L->vdec, dec, NULL) >= 0;
}

static ns_lav *
ns_lav_open(const guint8 *bytes, gsize len, int *out_w, int *out_h,
            double *out_dur, double *out_end, int *out_has_audio)
{
    av_log_set_level(AV_LOG_QUIET);

    ns_lav *L = g_new0(ns_lav, 1);
    L->data = g_try_malloc(len);
    if (!L->data) { g_free(L); return NULL; }
    memcpy(L->data, bytes, len);
    L->len = len;
    L->cap = len;
    L->reopen_on_growth = bytes_are_isobmff(bytes, len);
    L->opened_len = len;
    L->last_dts = AV_NOPTS_VALUE;
    L->resume_after_dts = AV_NOPTS_VALUE;
    L->last_out_pts = -1.0;
    L->skip_until_pts = -1.0;
    L->skip_before_pts = -1.0;

    size_t bufsz = 32768;
    unsigned char *iobuf = av_malloc(bufsz);
    if (!iobuf) { ns_lav_free(L); return NULL; }
    L->avio = avio_alloc_context(iobuf, (int)bufsz, 0, L,
                                 ns_lav_read, NULL, ns_lav_seek);
    if (!L->avio) { av_free(iobuf); ns_lav_free(L); return NULL; }

    L->fmt = avformat_alloc_context();
    if (!L->fmt) { ns_lav_free(L); return NULL; }
    L->fmt->pb = L->avio;
    if (avformat_open_input(&L->fmt, NULL, NULL, NULL) < 0) {
        L->fmt = NULL;
        ns_lav_free(L);
        return NULL;
    }
    if (!ns_lav_container_described(L->fmt) &&
        avformat_find_stream_info(L->fmt, NULL) < 0) {
        ns_lav_free(L);
        return NULL;
    }

    L->vstream = av_find_best_stream(L->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (L->vstream < 0) { ns_lav_free(L); return NULL; }

    AVStream *vs = L->fmt->streams[L->vstream];
    enum AVCodecID id = vs->codecpar->codec_id;
    if (id != AV_CODEC_ID_VP9 && id != AV_CODEC_ID_VP8 &&
        id != AV_CODEC_ID_H264 && id != AV_CODEC_ID_HEVC &&
        id != AV_CODEC_ID_AV1 && id != AV_CODEC_ID_MPEG4 &&
        id != AV_CODEC_ID_THEORA) {
        ns_lav_free(L);
        return NULL;
    }
    L->codec_id = id;

    int w = vs->codecpar->width, h = vs->codecpar->height;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) { ns_lav_free(L); return NULL; }

    if (!ns_lav_open_decoder(L, vs)) { ns_lav_free(L); return NULL; }

    L->time_base = vs->time_base;
    L->pkt = av_packet_alloc();
    L->frame = av_frame_alloc();
    if (!L->pkt || !L->frame) { ns_lav_free(L); return NULL; }

    double dur = 0.0;
    if (L->fmt->duration > 0)
        dur = (double)L->fmt->duration / (double)AV_TIME_BASE;
    else if (vs->duration > 0)
        dur = (double)vs->duration * av_q2d(vs->time_base);

    double last_pts = 0.0;
    while (av_read_frame(L->fmt, L->pkt) >= 0) {
        if (L->pkt->stream_index == L->vstream &&
            L->pkt->pts != AV_NOPTS_VALUE) {
            double t = (double)L->pkt->pts * av_q2d(vs->time_base);
            if (t > last_pts) last_pts = t;
        }
        av_packet_unref(L->pkt);
    }
    av_seek_frame(L->fmt, L->vstream, 0, AVSEEK_FLAG_BACKWARD);

    *out_w = w;
    *out_h = h;
    *out_dur = dur;
    *out_end = last_pts;
    *out_has_audio =
        av_find_best_stream(L->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) >= 0;
    return L;
}

static gboolean
ns_lav_probe_chunk_range(const guint8 *init, gsize init_len,
                         const guint8 *chunk, gsize chunk_len,
                         double *out_start, double *out_end)
{
    if (!chunk || !chunk_len) return FALSE;
    gsize total = init_len + chunk_len;
    guint8 *joined = g_try_malloc(total);
    if (!joined) return FALSE;
    if (init_len) memcpy(joined, init, init_len);
    memcpy(joined + init_len, chunk, chunk_len);

    ns_lav probe = {0};
    probe.data = joined;
    probe.len = total;

    double start = G_MAXDOUBLE;
    double end = 0.0;
    unsigned char *iobuf = av_malloc(32768);
    if (!iobuf) { g_free(joined); return FALSE; }
    AVIOContext *avio = avio_alloc_context(iobuf, 32768, 0, &probe,
                                           ns_lav_read, NULL, ns_lav_seek);
    if (!avio) { av_free(iobuf); g_free(joined); return FALSE; }
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        g_free(joined);
        return FALSE;
    }
    fmt->pb = avio;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        g_free(joined);
        return FALSE;
    }
    AVPacket *pkt = av_packet_alloc();
    if (pkt) {
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->pts != AV_NOPTS_VALUE &&
                pkt->stream_index >= 0 &&
                pkt->stream_index < (int)fmt->nb_streams) {
                AVRational tb = fmt->streams[pkt->stream_index]->time_base;
                double t = ((double)pkt->pts +
                            (pkt->duration > 0 ? (double)pkt->duration : 0.0)) *
                           av_q2d(tb);
                double t0 = (double)pkt->pts * av_q2d(tb);
                if (t0 < start) start = t0;
                if (t > end) end = t;
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&fmt);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
    g_free(joined);
    if (start == G_MAXDOUBLE || end <= start) return FALSE;
    if (out_start) *out_start = start;
    if (out_end) *out_end = end;
    return TRUE;
}

static void
ns_lav_flush_decoder(ns_lav *L)
{
    if (L->osdec) ns_os_vdec_flush(L->osdec);
    else avcodec_flush_buffers(L->vdec);
    if (L->pkt_pending) {
        av_packet_unref(L->pkt);
        L->pkt_pending = FALSE;
    }
}

static void
ns_lav_forget_position(ns_lav *L)
{
    ns_lav_flush_decoder(L);
    L->draining = FALSE;
    L->last_dts = AV_NOPTS_VALUE;
    L->resume_after_dts = AV_NOPTS_VALUE;
    L->last_out_pts = -1.0;
    L->skip_until_pts = -1.0;
    L->skip_before_pts = -1.0;
}

static void
ns_lav_rewind(ns_lav *L)
{
    av_seek_frame(L->fmt, L->vstream, 0, AVSEEK_FLAG_BACKWARD);
    ns_lav_forget_position(L);
}

static gboolean
ns_lav_seek_to(ns_lav *L, double seconds)
{
    int64_t ts = (int64_t)(seconds / av_q2d(L->time_base));
    if (av_seek_frame(L->fmt, L->vstream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return FALSE;
    ns_lav_forget_position(L);
    return TRUE;
}

static ns_texture *
ns_lav_frame_to_texture(ns_lav *L, AVFrame *frame)
{
    int w = frame->width, h = frame->height;
    AVRational sar = frame->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
        w = (int)((double)w * sar.num / sar.den + 0.5);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return NULL;
    gsize stride = ((gsize)w * 4 + 63) & ~(gsize)63;
    gsize buf_len = stride * (gsize)h;
    guint8 *bgra = av_malloc(buf_len + 64);
    if (!bgra) return NULL;

    L->sws = sws_getCachedContext(L->sws, frame->width, frame->height,
                                  frame->format, w, h, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!L->sws) { av_free(bgra); return NULL; }

    uint8_t *dst[4] = { bgra, NULL, NULL, NULL };
    int dst_stride[4] = { (int)stride, 0, 0, 0 };
    sws_scale(L->sws, (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height, dst, dst_stride);

    GBytes *gb = g_bytes_new_with_free_func(bgra, buf_len, av_free, bgra);
    ns_texture *tex = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                     gb, stride);
    g_bytes_unref(gb);
    return tex;
}

static gboolean
ns_lav_follow_growth(ns_lav *L)
{
    if (!L->reopen_on_growth || L->len <= L->opened_len) return FALSE;
    L->opened_len = L->len;
    if (avio_seek(L->avio, 0, SEEK_SET) < 0) return FALSE;
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return FALSE;
    fmt->pb = L->avio;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) return FALSE;
    if (L->vstream >= (int)fmt->nb_streams ||
        fmt->streams[L->vstream]->codecpar->codec_id != L->codec_id) {
        avformat_close_input(&fmt);
        return FALSE;
    }
    avformat_close_input(&L->fmt);
    L->fmt = fmt;
    if (L->last_dts != AV_NOPTS_VALUE)
        av_seek_frame(fmt, L->vstream, L->last_dts, AVSEEK_FLAG_BACKWARD);
    if (L->draining) {
        ns_lav_flush_decoder(L);
        L->draining = FALSE;
        L->skip_until_pts = L->last_out_pts;
        L->resume_after_dts = AV_NOPTS_VALUE;
    } else {
        L->resume_after_dts = L->last_dts;
    }
    return TRUE;
}

static gboolean
ns_lav_packet_wanted(ns_lav *L, const AVPacket *pkt)
{
    if (pkt->stream_index != L->vstream) return FALSE;
    if (L->resume_after_dts != AV_NOPTS_VALUE) {
        if (pkt->dts != AV_NOPTS_VALUE && pkt->dts <= L->resume_after_dts)
            return FALSE;
        L->resume_after_dts = AV_NOPTS_VALUE;
    }
    return TRUE;
}

typedef enum {
    NS_LAV_FRAME,
    NS_LAV_AGAIN,
    NS_LAV_EOF
} ns_lav_result;

static ns_texture *
ns_lav_os_frame_to_texture(ns_lav *L, ns_os_frame *f)
{
    int w = f->width, h = f->height;
    enum AVPixelFormat src_fmt = f->format == NS_OS_PIX_NV12 ? AV_PIX_FMT_NV12
                               : f->format == NS_OS_PIX_I420 ? AV_PIX_FMT_YUV420P
                               : AV_PIX_FMT_BGRA;
    gsize stride = ((gsize)w * 4 + 63) & ~(gsize)63;
    gsize buf_len = stride * (gsize)h;
    guint8 *bgra = av_malloc(buf_len + 64);
    if (!bgra) return NULL;
    L->sws = sws_getCachedContext(L->sws, w, h, src_fmt, w, h, AV_PIX_FMT_BGRA,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!L->sws) { av_free(bgra); return NULL; }
    gboolean bt709 = f->colorspace ? f->colorspace == 709 : h >= 600;
    const int *coef = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    sws_setColorspaceDetails(L->sws, coef, f->full_range, coef, 1,
                             0, 1 << 16, 1 << 16);
    const uint8_t *src[4] = { f->data + f->offset[0], f->data + f->offset[1],
                              f->data + f->offset[2], NULL };
    int src_stride[4] = { f->stride[0], f->stride[1], f->stride[2], 0 };
    uint8_t *dst[4] = { bgra, NULL, NULL, NULL };
    int dst_stride[4] = { (int)stride, 0, 0, 0 };
    sws_scale(L->sws, src, src_stride, 0, h, dst, dst_stride);

    GBytes *gb = g_bytes_new_with_free_func(bgra, buf_len, av_free, bgra);
    ns_texture *tex = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                     gb, stride);
    g_bytes_unref(gb);
    return tex;
}

static gboolean
ns_lav_os_fallback(ns_lav *L)
{
    const AVCodec *dec = avcodec_find_decoder(L->codec_id);
    if (!dec) return FALSE;
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    if (!ctx) return FALSE;
    ns_lav_enable_threads(ctx);
    if (avcodec_parameters_to_context(ctx, L->fmt->streams[L->vstream]->codecpar) < 0 ||
        avcodec_open2(ctx, dec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return FALSE;
    }
    ns_debug_log_emit(NS_DLOG_WARN, "media",
                      "%s %s decoder failed; falling back to libavcodec",
                      ns_os_media_backend(), avcodec_get_name(L->codec_id));
    ns_os_vdec_free(L->osdec);
    L->osdec = NULL;
    L->vdec = ctx;
    double resume = L->skip_before_pts;
    if (resume <= 0.0 || !ns_lav_seek_to(L, resume))
        ns_lav_rewind(L);
    L->skip_before_pts = resume;
    return TRUE;
}

static ns_lav_result
ns_lav_receive(ns_video_player *player, ns_texture **out_tex, double *out_pts)
{
    ns_lav *L = player->lav;
    if (L->osdec) {
        ns_os_frame_clear(&L->osframe);
        ns_os_status st = ns_os_vdec_receive(L->osdec, &L->osframe);
        if (st == NS_OS_FRAME) {
            L->os_produced = TRUE;
            *out_pts = (double)L->osframe.pts_us / 1e6;
            *out_tex = NULL;
            return NS_LAV_FRAME;
        }
        if (st == NS_OS_ERROR && !L->os_produced && ns_lav_os_fallback(L))
            return NS_LAV_AGAIN;
        return (st == NS_OS_AGAIN && !L->draining) ? NS_LAV_AGAIN : NS_LAV_EOF;
    }
    int r = avcodec_receive_frame(L->vdec, L->frame);
    if (r == 0) {
        *out_pts = (L->frame->pts == AV_NOPTS_VALUE)
            ? player->pending_time
            : (double)L->frame->pts * av_q2d(L->time_base);
        *out_tex = NULL;
        return NS_LAV_FRAME;
    }
    return r == AVERROR(EAGAIN) ? NS_LAV_AGAIN : NS_LAV_EOF;
}

static gboolean
ns_lav_send(ns_lav *L, AVPacket *pkt)
{
    if (L->osdec) {
        if (!pkt) {
            ns_os_vdec_drain(L->osdec);
            return TRUE;
        }
        double t = (pkt->pts != AV_NOPTS_VALUE ? (double)pkt->pts
                                               : (double)pkt->dts) *
                   av_q2d(L->time_base);
        return ns_os_vdec_send(L->osdec, pkt->data, (size_t)pkt->size,
                               (int64_t)(t * 1e6)) >= 0;
    }
    return avcodec_send_packet(L->vdec, pkt) != AVERROR(EAGAIN);
}

static gboolean
ns_lav_next(ns_video_player *player, ns_texture **out_tex, double *out_pts)
{
    ns_lav *L = player->lav;
    int refused = 0;
    for (;;) {
        double t = 0.0;
        ns_texture *tex = NULL;
        ns_lav_result r = ns_lav_receive(player, &tex, &t);
        if (r == NS_LAV_FRAME) {
            gboolean seen = L->skip_until_pts >= 0.0 &&
                            t <= L->skip_until_pts + 1e-6;
            gboolean before_target = L->skip_before_pts >= 0.0 &&
                                     t < L->skip_before_pts - 0.05;
            if (seen || before_target) {
                ns_os_frame_clear(&L->osframe);
                if (!L->osdec) av_frame_unref(L->frame);
                continue;
            }
            L->skip_until_pts = -1.0;
            L->skip_before_pts = -1.0;
            if (L->osdec) {
                tex = ns_lav_os_frame_to_texture(L, &L->osframe);
                ns_os_frame_clear(&L->osframe);
            } else {
                tex = ns_lav_frame_to_texture(L, L->frame);
                av_frame_unref(L->frame);
            }
            *out_tex = tex;
            *out_pts = t;
            L->last_out_pts = t;
            return TRUE;
        }
        if (r == NS_LAV_EOF && ns_lav_follow_growth(L))
            continue;
        if (r == NS_LAV_EOF)
            return FALSE;

        if (L->pkt_pending) {
            if (ns_lav_send(L, L->pkt)) {
                av_packet_unref(L->pkt);
                L->pkt_pending = FALSE;
                refused = 0;
                continue;
            }
            if (++refused > 64) return FALSE;
            continue;
        }

        int rr;
        while ((rr = av_read_frame(L->fmt, L->pkt)) >= 0 &&
               !ns_lav_packet_wanted(L, L->pkt))
            av_packet_unref(L->pkt);

        if (rr < 0 && !L->draining && ns_lav_follow_growth(L))
            continue;
        if (rr >= 0) {
            if (L->pkt->dts != AV_NOPTS_VALUE) L->last_dts = L->pkt->dts;
            if (ns_lav_send(L, L->pkt)) av_packet_unref(L->pkt);
            else L->pkt_pending = TRUE;
        } else if (!L->draining) {
            ns_lav_send(L, NULL);
            L->draining = TRUE;
        } else {
            return FALSE;
        }
    }
}
#endif

ns_video_player *
ns_video_player_new(const guint8 *bytes, gsize len)
{
    if (!bytes || len < 8) return NULL;

    if (bytes_are_mpeg1(bytes, len)) {
        guint8 *copy = g_try_malloc(len);
        if (!copy) return NULL;
        memcpy(copy, bytes, len);
        plm_t *plm = plm_create_with_memory(copy, len, TRUE);
        if (!plm) { g_free(copy); return NULL; }

        plm_set_audio_enabled(plm, FALSE);
        plm_set_loop(plm, FALSE);

        int w = plm_get_width(plm);
        int h = plm_get_height(plm);
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            plm_destroy(plm);
            return NULL;
        }

        ns_video_player *player = g_new0(ns_video_player, 1);
        player->plm = plm;
        player->width = w;
        player->height = h;
        player->has_audio = plm_get_num_audio_streams(plm) > 0;
        player->duration = plm_get_duration(plm);
        player->demuxed_end = player->duration;
        player->cur_time = -1.0;
        player->pending_time = -1.0;
        return player;
    }

#ifdef NS_HAVE_LIBAV
    if (bytes_are_matroska(bytes, len) || bytes_are_isobmff(bytes, len) ||
        bytes_are_ogg(bytes, len)) {
        int w = 0, h = 0, has_audio = 0;
        double dur = 0.0;
        double demuxed_end = 0.0;
        ns_lav *L = ns_lav_open(bytes, len, &w, &h, &dur, &demuxed_end,
                                &has_audio);
        if (!L) return NULL;

        ns_video_player *player = g_new0(ns_video_player, 1);
        player->lav = L;
        player->width = w;
        player->height = h;
        player->has_audio = has_audio;
        player->duration = dur;
        player->demuxed_end = demuxed_end;
        player->cur_time = -1.0;
        player->pending_time = -1.0;
        return player;
    }
#endif
#ifdef NS_HAVE_OS_MP4
    {
        int w = 0, h = 0, has_audio = 0;
        double dur = 0.0, demuxed_end = 0.0;
        ns_mp4_player *mp = ns_mp4_player_new(bytes, len, &w, &h, &dur,
                                              &demuxed_end, &has_audio);
        if (mp) {
            ns_video_player *player = g_new0(ns_video_player, 1);
            player->mp4 = mp;
            player->width = w;
            player->height = h;
            player->has_audio = has_audio;
            player->duration = dur;
            player->demuxed_end = demuxed_end;
            player->cur_time = -1.0;
            player->pending_time = -1.0;
            return player;
        }
    }
#endif

    return NULL;
}

void
ns_video_player_free(ns_video_player *player)
{
    if (!player) return;
    ns_texture_unref(player->cur_tex);
    ns_texture_unref(player->pending_tex);
    if (player->ahead) g_array_free(player->ahead, TRUE);
    if (player->plm) plm_destroy(player->plm);
#ifdef NS_HAVE_LIBAV
    if (player->lav) ns_lav_free(player->lav);
#endif
#ifdef NS_HAVE_OS_MP4
    if (player->mp4) ns_mp4_player_free(player->mp4);
#endif
    g_free(player);
}

gboolean
ns_video_player_has_audio(const ns_video_player *player)
{
    return player ? player->has_audio != 0 : FALSE;
}

int
ns_video_player_width(const ns_video_player *player)
{
    return player ? player->width : 0;
}

int
ns_video_player_height(const ns_video_player *player)
{
    return player ? player->height : 0;
}

double
ns_video_player_duration(const ns_video_player *player)
{
    return player ? player->duration : 0.0;
}

double
ns_video_player_buffered_end(const ns_video_player *player)
{
    return player ? player->demuxed_end : 0.0;
}

void
ns_video_player_note_end(ns_video_player *player, double end)
{
    if (!player || end <= 0.0) return;
    if (end > player->demuxed_end) player->demuxed_end = end;
    if (end > player->duration) player->duration = end;
}

gboolean
ns_video_probe_chunk_range(const guint8 *init, gsize init_len,
                           const guint8 *chunk, gsize chunk_len,
                           double *out_start, double *out_end)
{
#ifdef NS_HAVE_LIBAV
    return ns_lav_probe_chunk_range(init, init_len, chunk, chunk_len,
                                    out_start, out_end);
#elif defined(NS_HAVE_OS_MP4)
    return ns_mp4_probe_chunk_range(init, init_len, chunk, chunk_len,
                                    out_start, out_end);
#else
    (void)init; (void)init_len; (void)chunk; (void)chunk_len;
    if (out_start) *out_start = 0.0;
    if (out_end) *out_end = 0.0;
    return FALSE;
#endif
}

gboolean
ns_video_codec_available(const char *codec)
{
#ifdef NS_HAVE_LIBAV
    if (!codec || !*codec) return FALSE;
    enum AVCodecID id = AV_CODEC_ID_NONE;
    if (g_str_has_prefix(codec, "avc1") || g_str_has_prefix(codec, "avc3"))
        id = AV_CODEC_ID_H264;
    else if (g_str_has_prefix(codec, "hvc1") || g_str_has_prefix(codec, "hev1"))
        id = AV_CODEC_ID_HEVC;
    else if (g_str_has_prefix(codec, "av01"))
        id = AV_CODEC_ID_AV1;
    else if (g_str_has_prefix(codec, "vp09") || g_str_has_prefix(codec, "vp9"))
        id = AV_CODEC_ID_VP9;
    else if (g_str_has_prefix(codec, "vp08") || g_str_has_prefix(codec, "vp8"))
        id = AV_CODEC_ID_VP8;
    else if (g_str_has_prefix(codec, "mp4a.40") ||
             g_str_has_prefix(codec, "mp4a.66") ||
             g_str_has_prefix(codec, "mp4a.67") ||
             g_str_has_prefix(codec, "mp4a.68"))
        id = AV_CODEC_ID_AAC;
    else if (g_str_has_prefix(codec, "mp4a.69") ||
             g_str_has_prefix(codec, "mp4a.6b") ||
             strstr(codec, "mp3"))
        id = AV_CODEC_ID_MP3;
    else if (strcmp(codec, "ac-3") == 0 || g_str_has_prefix(codec, "mp4a.a5"))
        id = AV_CODEC_ID_AC3;
    else if (strcmp(codec, "ec-3") == 0 || g_str_has_prefix(codec, "mp4a.a6"))
        id = AV_CODEC_ID_EAC3;
    else if (strstr(codec, "flac"))
        id = AV_CODEC_ID_FLAC;
    else if (strstr(codec, "alac"))
        id = AV_CODEC_ID_ALAC;
    else if (strstr(codec, "opus"))
        id = AV_CODEC_ID_OPUS;
    else if (strstr(codec, "vorbis"))
        id = AV_CODEC_ID_VORBIS;
    else if (strcmp(codec, "theora") == 0)
        id = AV_CODEC_ID_THEORA;
    if (id == AV_CODEC_ID_NONE) return FALSE;
    if (id == AV_CODEC_ID_H264 && ns_os_h264_available()) return TRUE;
    if (id == AV_CODEC_ID_AAC && ns_os_aac_available()) return TRUE;
    if (id == AV_CODEC_ID_HEVC && ns_os_hw_video_available("hevc")) return TRUE;
    if (id == AV_CODEC_ID_AV1 || id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_VP8) {
        const char *hw = id == AV_CODEC_ID_AV1 ? "av1"
                       : id == AV_CODEC_ID_VP9 ? "vp9" : "vp8";
        if (ns_os_hw_video_available(hw)) return TRUE;
        if (ns_os_h264_available() && !g_getenv("NS_MEDIA_SOFTWARE_CODECS"))
            return FALSE;
    }
    if (g_getenv("NS_MEDIA_OS_CODECS_ONLY") &&
        id != AV_CODEC_ID_OPUS && id != AV_CODEC_ID_VORBIS &&
        id != AV_CODEC_ID_MP3)
        return FALSE;
    return avcodec_find_decoder(id) != NULL;
#elif defined(NS_HAVE_OS_MP4)
    if (!codec) return FALSE;
    if (g_str_has_prefix(codec, "avc1") || g_str_has_prefix(codec, "avc3"))
        return ns_os_h264_available();
    if (g_str_has_prefix(codec, "mp4a.40"))
        return ns_os_aac_available();
    return FALSE;
#else
    (void)codec;
    return FALSE;
#endif
}

gboolean
ns_video_player_extend(ns_video_player *player, const guint8 *bytes, gsize len)
{
#ifdef NS_HAVE_LIBAV
    if (!player || !player->lav || !bytes) return FALSE;
    ns_lav *L = player->lav;
    if (len < L->len) return FALSE;
    if (len == L->len) return TRUE;
    gsize head = MIN(L->len, (gsize)4096);
    if (memcmp(bytes, L->data, head) != 0) return FALSE;
    if (L->len > 4096 &&
        memcmp(bytes + L->len - 4096, L->data + L->len - 4096, 4096) != 0)
        return FALSE;

    if (len > L->cap) {
        gsize cap = L->cap ? L->cap : len;
        while (cap < len) {
            if (cap > G_MAXSIZE / 2) { cap = len; break; }
            cap *= 2;
        }
        guint8 *grown = g_try_realloc(L->data, cap);
        if (!grown) return FALSE;
        L->data = grown;
        L->cap = cap;
    }
    memcpy(L->data + L->len, bytes + L->len, len - L->len);
    L->len = len;
    L->avio->eof_reached = 0;
    L->avio->error = 0;
    if (L->draining && !L->reopen_on_growth) {
        ns_lav_flush_decoder(L);
        L->draining = FALSE;
    }
    return TRUE;
#elif defined(NS_HAVE_OS_MP4)
    if (!player || !player->mp4 || !bytes) return FALSE;
    double end = 0.0;
    if (!ns_mp4_player_extend(player->mp4, bytes, len, &end)) return FALSE;
    ns_video_player_note_end(player, end);
    return TRUE;
#else
    (void)player; (void)bytes; (void)len;
    return FALSE;
#endif
}

static ns_texture *
ns_plm_frame_to_texture(ns_video_player *player, plm_frame_t *frame)
{
    int w = player->width, h = player->height;
    gsize stride = (gsize)w * 4;
    gsize buf_len = stride * (gsize)h;
    guint8 *bgra = g_try_malloc(buf_len);
    if (!bgra)
        return NULL;
    memset(bgra, 0xFF, buf_len);
    plm_frame_to_bgra(frame, bgra, (int)stride);

    GBytes *bytes = g_bytes_new_take(bgra, buf_len);
    ns_texture *tex = ns_texture_new(w, h, NS_TEXTURE_BGRA_PREMULTIPLIED,
                                     bytes, stride);
    g_bytes_unref(bytes);
    return tex;
}

static void
ns_video_backend_rewind(ns_video_player *player)
{
    if (player->plm) plm_rewind(player->plm);
#ifdef NS_HAVE_LIBAV
    else if (player->lav) ns_lav_rewind(player->lav);
#endif
#ifdef NS_HAVE_OS_MP4
    else if (player->mp4) ns_mp4_player_seek(player->mp4, 0.0);
#endif
}

static gboolean
ns_video_backend_next(ns_video_player *player, ns_texture **out_tex,
                      double *out_pts, gboolean *out_eof)
{
    *out_eof = FALSE;
    if (player->plm) {
        plm_frame_t *frame = plm_decode_video(player->plm);
        if (!frame) {
            *out_eof = plm_has_ended(player->plm);
            return FALSE;
        }
        *out_tex = ns_plm_frame_to_texture(player, frame);
        *out_pts = frame->time;
        return TRUE;
    }
#ifdef NS_HAVE_LIBAV
    if (player->lav) {
        if (ns_lav_next(player, out_tex, out_pts)) return TRUE;
        *out_eof = TRUE;
        return FALSE;
    }
#endif
#ifdef NS_HAVE_OS_MP4
    if (player->mp4) {
        if (ns_mp4_player_next(player->mp4, out_tex, out_pts)) return TRUE;
        *out_eof = TRUE;
        return FALSE;
    }
#endif
    *out_eof = TRUE;
    return FALSE;
}

ns_texture *
ns_video_player_frame_at(ns_video_player *player, double seconds,
                         gboolean loop, gboolean *out_ended)
{
    if (out_ended) *out_ended = FALSE;
    if (!player) return NULL;
    if (!player->plm
#ifdef NS_HAVE_LIBAV
        && !player->lav
#endif
#ifdef NS_HAVE_OS_MP4
        && !player->mp4
#endif
       ) return NULL;
    if (seconds < 0) seconds = 0;

    gboolean backward = seconds + 1e-4 < player->cur_time;
    gboolean far_forward = player->cur_time >= 0.0 &&
                           seconds > player->cur_time + 2.0;
    if (backward || far_forward) {
        gboolean sought = FALSE;
#ifdef NS_HAVE_LIBAV
        if (player->lav)
            sought = ns_lav_seek_to(player->lav, seconds);
#endif
#ifdef NS_HAVE_OS_MP4
        if (player->mp4)
            sought = ns_mp4_player_seek(player->mp4, seconds);
#endif
        if (!sought) {
            if (!backward) sought = TRUE;
            else ns_video_backend_rewind(player);
        }
        if (backward || !sought || far_forward) {
            player->cur_time = -1.0;
            ns_texture_clear(&player->pending_tex);
            player->pending_time = -1.0;
            ns_video_player_drop_ahead(player);
        }
    }
#ifdef NS_HAVE_LIBAV
    if (player->lav && player->cur_time < 0.0 && player->pending_time < 0.0 &&
        seconds > 0.0)
        player->lav->skip_before_pts = seconds;
#endif
#ifdef NS_HAVE_OS_MP4
    if (player->mp4 && player->cur_time < 0.0 && player->pending_time < 0.0 &&
        seconds > 0.0)
        ns_mp4_player_skip_before(player->mp4, seconds);
#endif

    gboolean changed = FALSE;
    for (;;) {
        if (player->pending_time >= 0.0) {
            if (player->pending_time <= seconds || player->cur_time < 0.0) {
                ns_texture_unref(player->cur_tex);
                player->cur_tex = player->pending_tex;
                player->cur_time = player->pending_time;
                player->cur_serial = player->pending_serial;
                player->pending_tex = NULL;
                player->pending_time = -1.0;
                changed = TRUE;
                continue;
            }
            break;
        }
        if (ns_video_player_take_ahead(player)) continue;

        ns_texture *tex = NULL;
        double pts = 0.0;
        gboolean eof = FALSE;
        if (!ns_video_backend_next(player, &tex, &pts, &eof)) {
            if (!loop && eof && out_ended)
                *out_ended = TRUE;
            break;
        }
        ns_texture_unref(player->pending_tex);
        player->pending_tex = tex;
        player->pending_time = pts < 0.0 ? 0.0 : pts;
        player->pending_serial = ++player->next_serial;
    }

    return changed ? player->cur_tex : NULL;
}

void
ns_video_player_prefetch(ns_video_player *player, double until,
                         guint max_frames)
{
    if (!player || player->pending_time < 0.0) return;
    if (!player->ahead) {
        player->ahead = g_array_new(FALSE, FALSE, sizeof(ns_vframe));
        g_array_set_clear_func(player->ahead, ns_vframe_clear);
    }
    while (player->ahead->len + 1 < max_frames) {
        double last = player->ahead->len > 0
            ? g_array_index(player->ahead, ns_vframe,
                            player->ahead->len - 1).pts
            : player->pending_time;
        if (last >= until) break;
        ns_vframe f = { NULL, 0.0, 0 };
        gboolean eof = FALSE;
        if (!ns_video_backend_next(player, &f.tex, &f.pts, &eof)) break;
        if (f.pts < 0.0) f.pts = 0.0;
        f.serial = ++player->next_serial;
        g_array_append_val(player->ahead, f);
    }
}

guint
ns_video_player_current_serial(const ns_video_player *player)
{
    return player ? player->cur_serial : 0;
}

guint
ns_video_player_upcoming(const ns_video_player *player, guint index,
                         ns_texture **out_tex, double *out_pts)
{
    if (!player || player->pending_time < 0.0 || !player->pending_tex)
        return 0;
    if (index == 0) {
        *out_tex = player->pending_tex;
        *out_pts = player->pending_time;
        return player->pending_serial;
    }
    if (!player->ahead || index - 1 >= player->ahead->len) return 0;
    const ns_vframe *f = &g_array_index(player->ahead, ns_vframe, index - 1);
    *out_tex = f->tex;
    *out_pts = f->pts;
    return f->serial;
}
