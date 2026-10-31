/* Nordstjernen — pixel conversion and bitstream helpers shared by the OS codec backends.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "media_os.h"

#include <stdlib.h>
#include <string.h>

#define NS_OS_MAX_DIM 16384

typedef struct {
    int y_off;
    int y_mul;
    int rv;
    int gu;
    int gv;
    int bu;
} ns_yuv_coeffs;

static void
yuv_coeffs(int height, int full_range, int colorspace, ns_yuv_coeffs *c)
{
    int hd = colorspace ? colorspace == 709 : height >= 600;
    c->y_off = full_range ? 0 : 16;
    c->y_mul = full_range ? 256 : 298;
    if (hd) {
        c->rv = full_range ? 403 : 459;
        c->gu = full_range ? 48 : 55;
        c->gv = full_range ? 120 : 136;
        c->bu = full_range ? 475 : 541;
    } else {
        c->rv = full_range ? 359 : 409;
        c->gu = full_range ? 88 : 100;
        c->gv = full_range ? 183 : 208;
        c->bu = full_range ? 454 : 516;
    }
}

static inline uint8_t
clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static inline void
put_pixel(uint8_t *px, int y, int u, int v, const ns_yuv_coeffs *c)
{
    int yy = (y - c->y_off) * c->y_mul;
    u -= 128;
    v -= 128;
    px[0] = clamp8((yy + c->bu * u + 128) >> 8);
    px[1] = clamp8((yy - c->gu * u - c->gv * v + 128) >> 8);
    px[2] = clamp8((yy + c->rv * v + 128) >> 8);
    px[3] = 255;
}

static int
plane_copy(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride,
           int row_bytes, int rows)
{
    if (!src) return 0;
    for (int r = 0; r < rows; r++)
        memcpy(dst + (size_t)r * (size_t)dst_stride,
               src + (size_t)r * (size_t)src_stride, (size_t)row_bytes);
    return 1;
}

static int
frame_alloc(ns_os_frame *out, ns_os_pix format, int width, int height)
{
    memset(out, 0, sizeof *out);
    if (width <= 0 || height <= 0 || width > NS_OS_MAX_DIM ||
        height > NS_OS_MAX_DIM)
        return 0;
    int cw = (width + 1) / 2, ch = (height + 1) / 2;
    size_t total;
    if (format == NS_OS_PIX_BGRA) {
        out->stride[0] = ((width * 4) + 63) & ~63;
        total = (size_t)out->stride[0] * (size_t)height;
    } else if (format == NS_OS_PIX_NV12) {
        out->stride[0] = (width + 63) & ~63;
        out->stride[1] = out->stride[0];
        out->offset[1] = (size_t)out->stride[0] * (size_t)height;
        total = out->offset[1] + (size_t)out->stride[1] * (size_t)ch;
    } else {
        out->stride[0] = (width + 63) & ~63;
        out->stride[1] = out->stride[2] = (cw + 63) & ~63;
        out->offset[1] = (size_t)out->stride[0] * (size_t)height;
        out->offset[2] = out->offset[1] + (size_t)out->stride[1] * (size_t)ch;
        total = out->offset[2] + (size_t)out->stride[2] * (size_t)ch;
    }
    out->data = malloc(total);
    if (!out->data) return 0;
    out->format = format;
    out->width = width;
    out->height = height;
    return 1;
}

void
ns_os_frame_clear(ns_os_frame *frame)
{
    if (!frame) return;
    free(frame->data);
    memset(frame, 0, sizeof *frame);
}

int
ns_os_frame_copy_nv12(ns_os_frame *out, const uint8_t *y, int y_stride,
                      const uint8_t *uv, int uv_stride, int width, int height)
{
    if (!frame_alloc(out, NS_OS_PIX_NV12, width, height)) return 0;
    int cw = (width + 1) / 2, ch = (height + 1) / 2;
    if (plane_copy(out->data, out->stride[0], y, y_stride, width, height) &&
        plane_copy(out->data + out->offset[1], out->stride[1], uv, uv_stride,
                   cw * 2, ch))
        return 1;
    ns_os_frame_clear(out);
    return 0;
}

int
ns_os_frame_copy_i420(ns_os_frame *out, const uint8_t *y, int y_stride,
                      const uint8_t *u, int u_stride, const uint8_t *v,
                      int v_stride, int width, int height)
{
    if (!frame_alloc(out, NS_OS_PIX_I420, width, height)) return 0;
    int cw = (width + 1) / 2, ch = (height + 1) / 2;
    if (plane_copy(out->data, out->stride[0], y, y_stride, width, height) &&
        plane_copy(out->data + out->offset[1], out->stride[1], u, u_stride,
                   cw, ch) &&
        plane_copy(out->data + out->offset[2], out->stride[2], v, v_stride,
                   cw, ch))
        return 1;
    ns_os_frame_clear(out);
    return 0;
}

int
ns_os_frame_copy_bgra(ns_os_frame *out, const uint8_t *src, int stride,
                      int width, int height)
{
    if (!frame_alloc(out, NS_OS_PIX_BGRA, width, height)) return 0;
    if (plane_copy(out->data, out->stride[0], src, stride, width * 4, height))
        return 1;
    ns_os_frame_clear(out);
    return 0;
}

int
ns_os_frame_to_bgra(const ns_os_frame *f, uint8_t **out_bgra, int *out_stride)
{
    ns_os_frame dst;
    if (!f || !f->data || !frame_alloc(&dst, NS_OS_PIX_BGRA, f->width, f->height))
        return 0;
    if (f->format == NS_OS_PIX_BGRA) {
        plane_copy(dst.data, dst.stride[0], f->data, f->stride[0],
                   f->width * 4, f->height);
    } else {
        ns_yuv_coeffs c;
        yuv_coeffs(f->height, f->full_range, f->colorspace, &c);
        int nv12 = f->format == NS_OS_PIX_NV12;
        for (int row = 0; row < f->height; row++) {
            const uint8_t *ys = f->data + (size_t)row * (size_t)f->stride[0];
            const uint8_t *us = f->data + f->offset[1] +
                                (size_t)(row / 2) * (size_t)f->stride[1];
            const uint8_t *vs = nv12 ? us + 1
                              : f->data + f->offset[2] +
                                (size_t)(row / 2) * (size_t)f->stride[2];
            uint8_t *px = dst.data + (size_t)row * (size_t)dst.stride[0];
            for (int col = 0; col < f->width; col++) {
                int ci = nv12 ? (col / 2) * 2 : col / 2;
                put_pixel(px + col * 4, ys[col], us[ci], vs[ci], &c);
            }
        }
    }
    *out_bgra = dst.data;
    *out_stride = dst.stride[0];
    return 1;
}

static int
annexb_put(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *nal,
           size_t nal_len)
{
    size_t need = *len + 4 + nal_len;
    if (need > *cap) {
        size_t ncap = *cap ? *cap * 2 : 256;
        while (ncap < need) ncap *= 2;
        uint8_t *grown = realloc(*buf, ncap);
        if (!grown) return 0;
        *buf = grown;
        *cap = ncap;
    }
    static const uint8_t start[4] = { 0, 0, 0, 1 };
    memcpy(*buf + *len, start, 4);
    memcpy(*buf + *len + 4, nal, nal_len);
    *len = need;
    return 1;
}

int
ns_os_avcc_parameter_sets(const uint8_t *avcc, size_t avcc_len,
                          uint8_t **out_annexb, size_t *out_len,
                          int *out_nal_len_size)
{
    *out_annexb = NULL;
    *out_len = 0;
    if (!avcc || avcc_len < 7 || avcc[0] != 1) return 0;
    if (out_nal_len_size) *out_nal_len_size = (avcc[4] & 3) + 1;
    uint8_t *buf = NULL;
    size_t len = 0, cap = 0, pos = 5;
    for (int set = 0; set < 2; set++) {
        if (pos >= avcc_len) break;
        int count = set == 0 ? (avcc[pos] & 0x1f) : avcc[pos];
        pos++;
        for (int i = 0; i < count; i++) {
            if (pos + 2 > avcc_len) goto bad;
            size_t n = ((size_t)avcc[pos] << 8) | avcc[pos + 1];
            pos += 2;
            if (pos + n > avcc_len) goto bad;
            if (!annexb_put(&buf, &len, &cap, avcc + pos, n)) goto bad;
            pos += n;
        }
    }
    if (!len) goto bad;
    *out_annexb = buf;
    *out_len = len;
    return 1;
bad:
    free(buf);
    return 0;
}

int
ns_os_avcc_to_annexb(const uint8_t *data, size_t len, int nal_len_size,
                     uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (nal_len_size < 1 || nal_len_size > 4) return 0;
    uint8_t *buf = NULL;
    size_t blen = 0, cap = 0, pos = 0;
    while (pos + (size_t)nal_len_size <= len) {
        size_t n = 0;
        for (int i = 0; i < nal_len_size; i++)
            n = (n << 8) | data[pos + (size_t)i];
        pos += (size_t)nal_len_size;
        if (n > len - pos) break;
        if (!annexb_put(&buf, &blen, &cap, data + pos, n)) {
            free(buf);
            return 0;
        }
        pos += n;
    }
    if (!blen) return 0;
    *out = buf;
    *out_len = blen;
    return 1;
}

typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         bit;
} ns_bits;

static unsigned
bits_read(ns_bits *b, int n)
{
    unsigned v = 0;
    while (n-- > 0) {
        size_t byte = b->bit >> 3;
        unsigned bitv = byte < b->len ? (b->p[byte] >> (7 - (b->bit & 7))) & 1u : 0u;
        v = (v << 1) | bitv;
        b->bit++;
    }
    return v;
}

int
ns_os_aac_config(const uint8_t *asc, size_t asc_len, int *out_rate,
                 int *out_channels, int *out_object_type)
{
    static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                   24000, 22050, 16000, 12000, 11025, 8000,
                                   7350 };
    if (!asc || asc_len < 2) return 0;
    ns_bits b = { asc, asc_len, 0 };
    int aot = (int)bits_read(&b, 5);
    if (aot == 31) aot = 32 + (int)bits_read(&b, 6);
    unsigned idx = bits_read(&b, 4);
    int rate = idx == 15 ? (int)bits_read(&b, 24) : idx < 13 ? rates[idx] : 0;
    int ch = (int)bits_read(&b, 4);
    if (rate <= 0) return 0;
    if (out_rate) *out_rate = rate;
    if (out_channels) *out_channels = ch == 7 ? 8 : ch;
    if (out_object_type) *out_object_type = aot;
    return 1;
}

#ifndef NS_MEDIA_OS_GST

int ns_os_hw_video_available(const char *codec) { (void)codec; return 0; }
const char *ns_os_hw_video_decoder_name(const char *codec) { (void)codec; return NULL; }

ns_os_vdec *
ns_os_vdec_new_hw(const char *codec, const uint8_t *extradata,
                  size_t extradata_len, int width, int height)
{
    (void)codec; (void)extradata; (void)extradata_len; (void)width; (void)height;
    return NULL;
}

#endif

#ifdef NS_MEDIA_OS_NONE

void ns_os_media_preinit(void) { }
const char *ns_os_media_backend(void) { return "none"; }
int ns_os_h264_available(void) { return 0; }
int ns_os_aac_available(void) { return 0; }

ns_os_vdec *
ns_os_vdec_new_h264(const uint8_t *avcc, size_t avcc_len, int width, int height)
{
    (void)avcc; (void)avcc_len; (void)width; (void)height;
    return NULL;
}

int
ns_os_vdec_send(ns_os_vdec *dec, const uint8_t *data, size_t len, int64_t pts_us)
{
    (void)dec; (void)data; (void)len; (void)pts_us;
    return 0;
}

ns_os_status
ns_os_vdec_receive(ns_os_vdec *dec, ns_os_frame *out)
{
    (void)dec; (void)out;
    return NS_OS_ERROR;
}

void ns_os_vdec_drain(ns_os_vdec *dec) { (void)dec; }
void ns_os_vdec_flush(ns_os_vdec *dec) { (void)dec; }
void ns_os_vdec_free(ns_os_vdec *dec) { (void)dec; }

ns_os_adec *
ns_os_adec_new_aac(const uint8_t *asc, size_t asc_len, int rate, int channels)
{
    (void)asc; (void)asc_len; (void)rate; (void)channels;
    return NULL;
}

int
ns_os_adec_decode(ns_os_adec *dec, const uint8_t *data, size_t len,
                  float **out_pcm, size_t *out_frames, int *out_rate,
                  int *out_channels)
{
    (void)dec; (void)data; (void)len; (void)out_pcm; (void)out_frames;
    (void)out_rate; (void)out_channels;
    return 0;
}

void ns_os_adec_flush(ns_os_adec *dec) { (void)dec; }
void ns_os_adec_free(ns_os_adec *dec) { (void)dec; }

#endif
