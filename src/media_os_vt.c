/* Nordstjernen — H.264 and AAC decoding through VideoToolbox and AudioToolbox on Apple platforms.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "media_os.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NS_VT_QUEUE_MAX 16

struct ns_os_vdec {
    VTDecompressionSessionRef   session;
    CMVideoFormatDescriptionRef format;
    pthread_mutex_t             lock;
    ns_os_frame                 queue[NS_VT_QUEUE_MAX];
    int                         queued;
    int                         draining;
    int                         full_range;
};

struct ns_os_adec {
    AudioConverterRef converter;
    int               rate;
    int               channels;
    const uint8_t    *pending;
    UInt32            pending_len;
    AudioStreamPacketDescription desc;
};

void
ns_os_media_preinit(void)
{
}

const char *
ns_os_media_backend(void)
{
    return "VideoToolbox";
}

int
ns_os_h264_available(void)
{
    return 1;
}

int
ns_os_aac_available(void)
{
    return 1;
}

static pthread_once_t g_gpu_once = PTHREAD_ONCE_INIT;
static int            g_gpu_vp9;
static int            g_gpu_av1;

static int
vt_gpu_decodes(CMVideoCodecType type)
{
    if (__builtin_available(macOS 11.0, *))
        VTRegisterSupplementalVideoDecoderIfAvailable(type);
    return VTIsHardwareDecodeSupported(type) ? 1 : 0;
}

static void
vt_gpu_probe(void)
{
    if (getenv("NS_MEDIA_NO_HW_DECODER")) return;
    g_gpu_vp9 = vt_gpu_decodes(kCMVideoCodecType_VP9);
    g_gpu_av1 = vt_gpu_decodes(kCMVideoCodecType_AV1);
}

const char *
ns_os_gpu_decode_api(void)
{
    return "videotoolbox";
}

int
ns_os_gpu_decodes(const char *codec, int bit_depth)
{
    if (!codec || bit_depth > 10) return 0;
    pthread_once(&g_gpu_once, vt_gpu_probe);
    if (strcmp(codec, "vp9") == 0) return g_gpu_vp9;
    if (strcmp(codec, "av1") == 0) return g_gpu_av1;
    return 0;
}

static void
vt_queue_push(ns_os_vdec *d, ns_os_frame *f)
{
    pthread_mutex_lock(&d->lock);
    if (d->queued == NS_VT_QUEUE_MAX) {
        ns_os_frame_clear(f);
        pthread_mutex_unlock(&d->lock);
        return;
    }
    int at = d->queued;
    while (at > 0 && d->queue[at - 1].pts_us > f->pts_us) {
        d->queue[at] = d->queue[at - 1];
        at--;
    }
    d->queue[at] = *f;
    d->queued++;
    pthread_mutex_unlock(&d->lock);
}

static void
vt_output(void *refcon, void *source_refcon, OSStatus status,
          VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts,
          CMTime duration)
{
    (void)source_refcon; (void)flags; (void)duration;
    ns_os_vdec *d = refcon;
    if (status != noErr || !image) return;
    CVPixelBufferRef pb = (CVPixelBufferRef)image;
    if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return;
    ns_os_frame f;
    int w = (int)CVPixelBufferGetWidth(pb);
    int h = (int)CVPixelBufferGetHeight(pb);
    int ok = ns_os_frame_copy_nv12(&f,
        CVPixelBufferGetBaseAddressOfPlane(pb, 0),
        (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
        CVPixelBufferGetBaseAddressOfPlane(pb, 1),
        (int)CVPixelBufferGetBytesPerRowOfPlane(pb, 1), w, h);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (!ok) return;
    f.full_range = d->full_range;
    f.pts_us = CMTIME_IS_VALID(pts)
        ? (int64_t)(CMTimeGetSeconds(pts) * 1e6) : 0;
    vt_queue_push(d, &f);
}

static int
vt_session_create(ns_os_vdec *d)
{
    OSType fmt = d->full_range
        ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
        : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef fmt_num = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    const void *keys[] = { kCVPixelBufferPixelFormatTypeKey };
    const void *vals[] = { fmt_num };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(fmt_num);
    VTDecompressionOutputCallbackRecord cb = { vt_output, d };
    OSStatus st = VTDecompressionSessionCreate(NULL, d->format, NULL, attrs,
                                               &cb, &d->session);
    CFRelease(attrs);
    return st == noErr;
}

ns_os_vdec *
ns_os_vdec_new_h264(const uint8_t *avcc, size_t avcc_len, int width, int height)
{
    (void)width; (void)height;
    if (!avcc || avcc_len < 7 || avcc[0] != 1) return NULL;
    ns_os_vdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);

    const void *keys[] = { CFSTR("avcC") };
    CFDataRef data = CFDataCreate(NULL, avcc, (CFIndex)avcc_len);
    const void *vals[] = { data };
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, keys, vals, 1,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(data);
    const void *ext_keys[] = {
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms
    };
    const void *ext_vals[] = { atoms };
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ext_keys, ext_vals, 1,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    CFRelease(atoms);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_H264,
                                                 width, height, ext,
                                                 &d->format);
    CFRelease(ext);
    if (st != noErr || !vt_session_create(d)) {
        ns_os_vdec_free(d);
        return NULL;
    }
    return d;
}

int
ns_os_vdec_send(ns_os_vdec *d, const uint8_t *data, size_t len, int64_t pts_us)
{
    if (!d || !d->session || !data || !len) return 0;
    CMBlockBufferRef block = NULL;
    if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, len, NULL, NULL, 0, len,
                                           kCMBlockBufferAssureMemoryNowFlag,
                                           &block) != noErr)
        return 0;
    if (CMBlockBufferReplaceDataBytes(data, block, 0, len) != noErr) {
        CFRelease(block);
        return 0;
    }
    CMSampleTimingInfo timing = {
        kCMTimeInvalid, CMTimeMake(pts_us, 1000000), kCMTimeInvalid
    };
    CMSampleBufferRef sample = NULL;
    OSStatus st = CMSampleBufferCreateReady(NULL, block, d->format, 1, 1,
                                            &timing, 1, &len, &sample);
    CFRelease(block);
    if (st != noErr) return 0;
    st = VTDecompressionSessionDecodeFrame(
        d->session, sample, kVTDecodeFrame_EnableTemporalProcessing, NULL,
        NULL);
    CFRelease(sample);
    if (st == kVTInvalidSessionErr) {
        VTDecompressionSessionInvalidate(d->session);
        CFRelease(d->session);
        d->session = NULL;
        vt_session_create(d);
    }
    d->draining = 0;
    return st == noErr;
}

ns_os_status
ns_os_vdec_receive(ns_os_vdec *d, ns_os_frame *out)
{
    memset(out, 0, sizeof *out);
    if (!d) return NS_OS_ERROR;
    pthread_mutex_lock(&d->lock);
    int hold = d->draining ? 0 : 2;
    if (d->queued > hold) {
        *out = d->queue[0];
        memmove(d->queue, d->queue + 1,
                (size_t)(d->queued - 1) * sizeof d->queue[0]);
        d->queued--;
        pthread_mutex_unlock(&d->lock);
        return NS_OS_FRAME;
    }
    pthread_mutex_unlock(&d->lock);
    return NS_OS_AGAIN;
}

void
ns_os_vdec_drain(ns_os_vdec *d)
{
    if (!d || !d->session) return;
    VTDecompressionSessionFinishDelayedFrames(d->session);
    VTDecompressionSessionWaitForAsynchronousFrames(d->session);
    d->draining = 1;
}

void
ns_os_vdec_flush(ns_os_vdec *d)
{
    if (!d) return;
    if (d->session) {
        VTDecompressionSessionFinishDelayedFrames(d->session);
        VTDecompressionSessionWaitForAsynchronousFrames(d->session);
    }
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < d->queued; i++) ns_os_frame_clear(&d->queue[i]);
    d->queued = 0;
    d->draining = 0;
    pthread_mutex_unlock(&d->lock);
}

void
ns_os_vdec_free(ns_os_vdec *d)
{
    if (!d) return;
    if (d->session) {
        VTDecompressionSessionWaitForAsynchronousFrames(d->session);
        VTDecompressionSessionInvalidate(d->session);
        CFRelease(d->session);
    }
    if (d->format) CFRelease(d->format);
    for (int i = 0; i < d->queued; i++) ns_os_frame_clear(&d->queue[i]);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

static size_t
esds_put_len(uint8_t *p, size_t n)
{
    p[0] = (uint8_t)(0x80 | ((n >> 21) & 0x7f));
    p[1] = (uint8_t)(0x80 | ((n >> 14) & 0x7f));
    p[2] = (uint8_t)(0x80 | ((n >> 7) & 0x7f));
    p[3] = (uint8_t)(n & 0x7f);
    return 4;
}

static size_t
esds_cookie(const uint8_t *asc, size_t asc_len, uint8_t *out)
{
    size_t dsi = 1 + 4 + asc_len;
    size_t dcd = 1 + 4 + 13 + dsi;
    size_t es = 3 + dcd + 1 + 4 + 1;
    size_t p = 0;
    out[p++] = 0x03;
    p += esds_put_len(out + p, es);
    out[p++] = 0; out[p++] = 0; out[p++] = 0;
    out[p++] = 0x04;
    p += esds_put_len(out + p, dcd);
    out[p++] = 0x40;
    out[p++] = 0x15;
    memset(out + p, 0, 11);
    p += 11;
    out[p++] = 0x05;
    p += esds_put_len(out + p, asc_len);
    memcpy(out + p, asc, asc_len);
    p += asc_len;
    out[p++] = 0x06;
    p += esds_put_len(out + p, 1);
    out[p++] = 0x02;
    return p;
}

ns_os_adec *
ns_os_adec_new_aac(const uint8_t *asc, size_t asc_len, int rate, int channels)
{
    if (!asc || !asc_len || asc_len > 64) return NULL;
    int cfg_rate = 0, cfg_ch = 0;
    if (ns_os_aac_config(asc, asc_len, &cfg_rate, &cfg_ch, NULL)) {
        if (rate <= 0) rate = cfg_rate;
        if (channels <= 0) channels = cfg_ch;
    }
    if (rate <= 0 || channels <= 0 || channels > 8) return NULL;
    ns_os_adec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->rate = rate;
    d->channels = channels;

    AudioStreamBasicDescription in = {0};
    in.mFormatID = kAudioFormatMPEG4AAC;
    in.mSampleRate = rate;
    in.mChannelsPerFrame = (UInt32)channels;
    in.mFramesPerPacket = 1024;
    UInt32 sz = sizeof in;
    uint8_t cookie[128];
    size_t cookie_len = esds_cookie(asc, asc_len, cookie);
    AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, (UInt32)cookie_len,
                           cookie, &sz, &in);
    d->rate = (int)in.mSampleRate;
    d->channels = (int)in.mChannelsPerFrame;

    AudioStreamBasicDescription out = {0};
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    out.mSampleRate = in.mSampleRate;
    out.mChannelsPerFrame = in.mChannelsPerFrame;
    out.mBitsPerChannel = 32;
    out.mBytesPerFrame = 4 * in.mChannelsPerFrame;
    out.mFramesPerPacket = 1;
    out.mBytesPerPacket = out.mBytesPerFrame;
    if (AudioConverterNew(&in, &out, &d->converter) != noErr) {
        free(d);
        return NULL;
    }
    AudioConverterSetProperty(d->converter,
                              kAudioConverterDecompressionMagicCookie,
                              (UInt32)cookie_len, cookie);
    return d;
}

static OSStatus
adec_input(AudioConverterRef conv, UInt32 *io_packets, AudioBufferList *io_data,
           AudioStreamPacketDescription **out_desc, void *user)
{
    (void)conv;
    ns_os_adec *d = user;
    if (!d->pending) {
        *io_packets = 0;
        return 1;
    }
    io_data->mNumberBuffers = 1;
    io_data->mBuffers[0].mData = (void *)d->pending;
    io_data->mBuffers[0].mDataByteSize = d->pending_len;
    io_data->mBuffers[0].mNumberChannels = (UInt32)d->channels;
    d->desc.mStartOffset = 0;
    d->desc.mVariableFramesInPacket = 0;
    d->desc.mDataByteSize = d->pending_len;
    if (out_desc) *out_desc = &d->desc;
    *io_packets = 1;
    d->pending = NULL;
    return noErr;
}

int
ns_os_adec_decode(ns_os_adec *d, const uint8_t *data, size_t len,
                  float **out_pcm, size_t *out_frames, int *out_rate,
                  int *out_channels)
{
    *out_pcm = NULL;
    *out_frames = 0;
    if (!d || !data || !len) return 0;
    UInt32 cap = 4096;
    float *pcm = malloc((size_t)cap * (size_t)d->channels * sizeof(float));
    if (!pcm) return 0;
    d->pending = data;
    d->pending_len = (UInt32)len;
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = (UInt32)d->channels;
    list.mBuffers[0].mDataByteSize = cap * (UInt32)d->channels * sizeof(float);
    list.mBuffers[0].mData = pcm;
    UInt32 frames = cap;
    OSStatus st = AudioConverterFillComplexBuffer(d->converter, adec_input, d,
                                                  &frames, &list, NULL);
    d->pending = NULL;
    if ((st != noErr && st != 1) || frames == 0) {
        free(pcm);
        return 0;
    }
    *out_pcm = pcm;
    *out_frames = frames;
    *out_rate = d->rate;
    *out_channels = d->channels;
    return 1;
}

void
ns_os_adec_flush(ns_os_adec *d)
{
    if (d && d->converter) AudioConverterReset(d->converter);
}

void
ns_os_adec_free(ns_os_adec *d)
{
    if (!d) return;
    if (d->converter) AudioConverterDispose(d->converter);
    free(d);
}
