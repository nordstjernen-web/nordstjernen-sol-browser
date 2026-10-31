/* Nordstjernen — H.264 and AAC decoding through the operating system's own codecs.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_MEDIA_OS_H
#define NS_MEDIA_OS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ns_os_vdec ns_os_vdec;
typedef struct ns_os_adec ns_os_adec;

typedef enum {
    NS_OS_PIX_NV12,
    NS_OS_PIX_I420,
    NS_OS_PIX_BGRA
} ns_os_pix;

typedef struct {
    uint8_t  *data;
    ns_os_pix format;
    size_t    offset[3];
    int       stride[3];
    int       width;
    int       height;
    int       full_range;
    int       colorspace;
    int64_t   pts_us;
} ns_os_frame;

typedef enum {
    NS_OS_AGAIN = 0,
    NS_OS_FRAME = 1,
    NS_OS_ERROR = -1
} ns_os_status;

void ns_os_media_preinit(void);
const char *ns_os_media_backend(void);
int  ns_os_h264_available(void);
int  ns_os_aac_available(void);

ns_os_vdec  *ns_os_vdec_new_h264(const uint8_t *avcc, size_t avcc_len,
                                 int width, int height);
int          ns_os_hw_video_available(const char *codec);
const char  *ns_os_hw_video_decoder_name(const char *codec);
ns_os_vdec  *ns_os_vdec_new_hw(const char *codec, const uint8_t *extradata,
                               size_t extradata_len, int width, int height);
int          ns_os_vdec_send(ns_os_vdec *dec, const uint8_t *data, size_t len,
                             int64_t pts_us);
ns_os_status ns_os_vdec_receive(ns_os_vdec *dec, ns_os_frame *out);
void         ns_os_vdec_drain(ns_os_vdec *dec);
void         ns_os_vdec_flush(ns_os_vdec *dec);
void         ns_os_vdec_free(ns_os_vdec *dec);

ns_os_adec  *ns_os_adec_new_aac(const uint8_t *asc, size_t asc_len,
                                int rate, int channels);
int          ns_os_adec_decode(ns_os_adec *dec, const uint8_t *data,
                               size_t len, float **out_pcm,
                               size_t *out_frames, int *out_rate,
                               int *out_channels);
void         ns_os_adec_flush(ns_os_adec *dec);
void         ns_os_adec_free(ns_os_adec *dec);

void ns_os_frame_clear(ns_os_frame *frame);
int  ns_os_frame_copy_nv12(ns_os_frame *out, const uint8_t *y, int y_stride,
                           const uint8_t *uv, int uv_stride, int width,
                           int height);
int  ns_os_frame_copy_i420(ns_os_frame *out, const uint8_t *y, int y_stride,
                           const uint8_t *u, int u_stride, const uint8_t *v,
                           int v_stride, int width, int height);
int  ns_os_frame_copy_bgra(ns_os_frame *out, const uint8_t *src, int stride,
                           int width, int height);
int  ns_os_frame_to_bgra(const ns_os_frame *frame, uint8_t **out_bgra,
                         int *out_stride);
int  ns_os_avcc_parameter_sets(const uint8_t *avcc, size_t avcc_len,
                               uint8_t **out_annexb, size_t *out_len,
                               int *out_nal_len_size);
int  ns_os_avcc_to_annexb(const uint8_t *data, size_t len, int nal_len_size,
                          uint8_t **out, size_t *out_len);
int  ns_os_aac_config(const uint8_t *asc, size_t asc_len, int *out_rate,
                      int *out_channels, int *out_object_type);

#ifdef __cplusplus
}
#endif

#endif
