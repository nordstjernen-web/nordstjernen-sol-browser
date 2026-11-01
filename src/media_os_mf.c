/* Nordstjernen — H.264 and AAC decoding through Windows Media Foundation transforms.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN

#include "media_os.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *ns_mf_startup_fn)(ULONG, DWORD);
typedef HRESULT (WINAPI *ns_mf_enum_fn)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
                                        const MFT_REGISTER_TYPE_INFO *,
                                        IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *ns_mf_media_type_fn)(IMFMediaType **);
typedef HRESULT (WINAPI *ns_mf_sample_fn)(IMFSample **);
typedef HRESULT (WINAPI *ns_mf_buffer_fn)(DWORD, IMFMediaBuffer **);
typedef HRESULT (WINAPI *ns_mf_dxgi_manager_fn)(UINT *, IMFDXGIDeviceManager **);
typedef HRESULT (WINAPI *ns_d3d11_create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE,
                                             HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL *, UINT,
                                             UINT, ID3D11Device **,
                                             D3D_FEATURE_LEVEL *,
                                             ID3D11DeviceContext **);

/* Defined here rather than linked from dxguid/mfuuid. */
static const GUID ns_MF_SA_D3D11_AWARE =
    { 0x206b4fc8, 0xfcf9, 0x4c51,
      { 0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0 } };
static const GUID ns_IID_ID3D10Multithread =
    { 0x9b7e4e00, 0x342c, 0x4106,
      { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
static const GUID ns_IID_IMFDXGIBuffer =
    { 0xe7174cfa, 0x1c9e, 0x48b1,
      { 0x88, 0x66, 0x62, 0x62, 0x26, 0xbf, 0xc2, 0x58 } };

static struct {
    ns_mf_startup_fn    startup;
    ns_mf_enum_fn       enum_ex;
    ns_mf_media_type_fn create_media_type;
    ns_mf_sample_fn     create_sample;
    ns_mf_buffer_fn     create_memory_buffer;
    ns_mf_dxgi_manager_fn create_dxgi_manager;
} g_mf;

struct ns_os_vdec {
    IMFTransform *mft;
    uint8_t      *param_sets;
    size_t        param_sets_len;
    int           nal_len_size;
    int           need_param_sets;
    int           width;
    int           height;
    int           out_width;
    int           out_height;
    int           vis_width;
    int           vis_height;
    int           full_range;
    int           colorspace;
    int           output_ready;
    DWORD         out_buf_size;
    int           provides_samples;
    int           hw;
    int           hw_reported;
};

struct ns_os_adec {
    IMFTransform *mft;
    int           rate;
    int           channels;
    int           is_float;
    DWORD         out_buf_size;
    int           provides_samples;
};

static INIT_ONCE g_mf_once = INIT_ONCE_STATIC_INIT;
static int       g_mf_ok;

static int
mf_load(void)
{
    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
    if (!mfplat) return 0;
    g_mf.startup = (ns_mf_startup_fn)(void *)GetProcAddress(mfplat, "MFStartup");
    g_mf.enum_ex = (ns_mf_enum_fn)(void *)GetProcAddress(mfplat, "MFTEnumEx");
    g_mf.create_media_type =
        (ns_mf_media_type_fn)(void *)GetProcAddress(mfplat, "MFCreateMediaType");
    g_mf.create_sample =
        (ns_mf_sample_fn)(void *)GetProcAddress(mfplat, "MFCreateSample");
    g_mf.create_memory_buffer =
        (ns_mf_buffer_fn)(void *)GetProcAddress(mfplat, "MFCreateMemoryBuffer");
    g_mf.create_dxgi_manager = (ns_mf_dxgi_manager_fn)(void *)GetProcAddress(
        mfplat, "MFCreateDXGIDeviceManager");
    return g_mf.startup && g_mf.enum_ex && g_mf.create_media_type &&
           g_mf.create_sample && g_mf.create_memory_buffer;
}

static BOOL CALLBACK
mf_startup(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    if (!mf_load()) return TRUE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return TRUE;
    g_mf_ok = SUCCEEDED(g_mf.startup(MF_VERSION, MFSTARTUP_LITE));
    return TRUE;
}

static int
mf_ready(void)
{
    InitOnceExecuteOnce(&g_mf_once, mf_startup, NULL, NULL);
    if (!g_mf_ok) return 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
}

static IMFTransform *
mf_find_decoder(REFGUID category, REFGUID major, REFGUID subtype)
{
    MFT_REGISTER_TYPE_INFO in = { *major, *subtype };
    IMFActivate **acts = NULL;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                   MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(g_mf.enum_ex(*category, flags, &in, NULL, &acts, &count)))
        return NULL;
    IMFTransform *mft = NULL;
    for (UINT32 i = 0; i < count; i++) {
        if (!mft &&
            FAILED(IMFActivate_ActivateObject(acts[i], &IID_IMFTransform,
                                              (void **)&mft)))
            mft = NULL;
        IMFActivate_Release(acts[i]);
    }
    CoTaskMemFree(acts);
    return mft;
}

static int
mf_category_has(REFGUID category, REFGUID major, REFGUID subtype)
{
    if (!mf_ready()) return 0;
    MFT_REGISTER_TYPE_INFO in = { *major, *subtype };
    IMFActivate **acts = NULL;
    UINT32 count = 0;
    if (FAILED(g_mf.enum_ex(*category,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                         MFT_ENUM_FLAG_SORTANDFILTER,
                         &in, NULL, &acts, &count)))
        return 0;
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(acts[i]);
    CoTaskMemFree(acts);
    return count > 0;
}

void
ns_os_media_preinit(void)
{
    if (!mf_ready()) return;
    LoadLibraryW(L"msmpeg2vdec.dll");
    LoadLibraryW(L"msauddecmft.dll");
    ns_os_h264_available();
    ns_os_aac_available();
}

const char *
ns_os_media_backend(void)
{
    return "Media Foundation";
}

int
ns_os_h264_available(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = mf_category_has(&MFT_CATEGORY_VIDEO_DECODER, &MFMediaType_Video,
                                 &MFVideoFormat_H264);
    return cached;
}

int
ns_os_aac_available(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = mf_category_has(&MFT_CATEGORY_AUDIO_DECODER, &MFMediaType_Audio,
                                 &MFAudioFormat_AAC);
    return cached;
}

static void
mf_size(IMFAttributes *attrs, REFGUID key, int *w, int *h)
{
    UINT64 packed = 0;
    if (SUCCEEDED(IMFAttributes_GetUINT64(attrs, key, &packed))) {
        *w = (int)(packed >> 32);
        *h = (int)(packed & 0xffffffffu);
    }
}

static void
mf_stream_info(IMFTransform *mft, DWORD *out_size, int *provides)
{
    MFT_OUTPUT_STREAM_INFO info;
    memset(&info, 0, sizeof info);
    if (SUCCEEDED(IMFTransform_GetOutputStreamInfo(mft, 0, &info))) {
        *out_size = info.cbSize;
        *provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }
}

static int
vdec_pick_output(ns_os_vdec *d)
{
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->mft, 0, i, &t)))
            return 0;
        GUID sub;
        int ok = SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) &&
                 IsEqualGUID(&sub, &MFVideoFormat_NV12) &&
                 SUCCEEDED(IMFTransform_SetOutputType(d->mft, 0, t, 0));
        if (ok) {
            int w = d->width, h = d->height;
            mf_size((IMFAttributes *)t, &MF_MT_FRAME_SIZE, &w, &h);
            d->out_width = w;
            d->out_height = h;
            d->vis_width = w;
            d->vis_height = h;
            MFVideoArea area;
            UINT32 sz = 0;
            if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE,
                                               (UINT8 *)&area, sizeof area, &sz)) &&
                sz == sizeof area && area.Area.cx > 0 && area.Area.cy > 0) {
                d->vis_width = (int)area.Area.cx;
                d->vis_height = (int)area.Area.cy;
            } else if (d->width > 0 && d->height > 0 &&
                       d->width <= w && d->height <= h) {
                d->vis_width = d->width;
                d->vis_height = d->height;
            }
            UINT32 range = 0;
            d->full_range =
                SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_VIDEO_NOMINAL_RANGE,
                                                 &range)) &&
                range == MFNominalRange_0_255;
            UINT32 matrix = 0;
            d->colorspace = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_YUV_MATRIX, &matrix)))
                d->colorspace = matrix == MFVideoTransferMatrix_BT709 ? 709
                              : matrix == MFVideoTransferMatrix_BT601 ? 601 : 0;
            IMFMediaType_Release(t);
            mf_stream_info(d->mft, &d->out_buf_size, &d->provides_samples);
            d->output_ready = 1;
            return 1;
        }
        IMFMediaType_Release(t);
    }
}

/* One D3D11 device with video support, shared by every decoder in the
 * process. Handing its device manager to a D3D11-aware decoder (Microsoft's
 * H.264 MFT is one) makes it decode through DXVA on the GPU; its output
 * samples are then GPU surfaces, which IMF2DBuffer::Lock2D reads back. */
static INIT_ONCE             g_d3d_once = INIT_ONCE_STATIC_INIT;
static IMFDXGIDeviceManager *g_d3d_manager;

static BOOL CALLBACK
mf_d3d_startup(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    if (getenv("NS_MEDIA_NO_HW_DECODE") || !g_mf.create_dxgi_manager)
        return TRUE;
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    ns_d3d11_create_fn create = d3d11
        ? (ns_d3d11_create_fn)(void *)GetProcAddress(d3d11, "D3D11CreateDevice")
        : NULL;
    if (!create) return TRUE;
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    ID3D11Device *dev = NULL;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                        D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                        (UINT)(sizeof levels / sizeof levels[0]),
                        D3D11_SDK_VERSION, &dev, NULL, NULL);
    if (hr == E_INVALIDARG)
        hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels + 1,
                    (UINT)(sizeof levels / sizeof levels[0]) - 1,
                    D3D11_SDK_VERSION, &dev, NULL, NULL);
    if (FAILED(hr) || !dev) return TRUE;
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &ns_IID_ID3D10Multithread,
                                              (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    UINT token = 0;
    IMFDXGIDeviceManager *mgr = NULL;
    if (SUCCEEDED(g_mf.create_dxgi_manager(&token, &mgr)) &&
        SUCCEEDED(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev,
                                                   token)))
        g_d3d_manager = mgr;
    else if (mgr)
        IMFDXGIDeviceManager_Release(mgr);
    ID3D11Device_Release(dev);
    return TRUE;
}

static int
mf_use_d3d(IMFTransform *mft)
{
    InitOnceExecuteOnce(&g_d3d_once, mf_d3d_startup, NULL, NULL);
    if (!g_d3d_manager) return 0;
    IMFAttributes *attrs = NULL;
    UINT32 aware = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(mft, &attrs)) && attrs) {
        IMFAttributes_GetUINT32(attrs, &ns_MF_SA_D3D11_AWARE, &aware);
        IMFAttributes_Release(attrs);
    }
    if (!aware) return 0;
    return SUCCEEDED(IMFTransform_ProcessMessage(
        mft, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)g_d3d_manager));
}

ns_os_vdec *
ns_os_vdec_new_h264(const uint8_t *avcc, size_t avcc_len, int width, int height)
{
    if (!mf_ready()) return NULL;
    ns_os_vdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->width = width;
    d->height = height;
    if (!ns_os_avcc_parameter_sets(avcc, avcc_len, &d->param_sets,
                                   &d->param_sets_len, &d->nal_len_size)) {
        free(d);
        return NULL;
    }
    d->need_param_sets = 1;
    d->mft = mf_find_decoder(&MFT_CATEGORY_VIDEO_DECODER, &MFMediaType_Video,
                             &MFVideoFormat_H264);
    if (!d->mft) goto fail;
    d->hw = mf_use_d3d(d->mft);
    IMFMediaType *in = NULL;
    if (FAILED(g_mf.create_media_type(&in))) goto fail;
    IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    if (width > 0 && height > 0)
        IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE,
                               ((UINT64)(UINT32)width << 32) | (UINT32)height);
    IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE,
                           MFVideoInterlace_MixedInterlaceOrProgressive);
    HRESULT hr = IMFTransform_SetInputType(d->mft, 0, in, 0);
    if (FAILED(hr) && d->hw) {
        /* The GPU cannot take this stream (profile, size): decode it in
         * software instead. */
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_SET_D3D_MANAGER, 0);
        d->hw = 0;
        hr = IMFTransform_SetInputType(d->mft, 0, in, 0);
    }
    IMFMediaType_Release(in);
    if (FAILED(hr)) goto fail;
    vdec_pick_output(d);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
fail:
    ns_os_vdec_free(d);
    return NULL;
}

static IMFSample *
mf_sample_from(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen,
               int64_t pts_us)
{
    size_t total = alen + blen;
    if (total == 0 || total > 0x7fffffffu) return NULL;
    IMFMediaBuffer *buf = NULL;
    if (FAILED(g_mf.create_memory_buffer((DWORD)total, &buf))) return NULL;
    BYTE *p = NULL;
    if (FAILED(IMFMediaBuffer_Lock(buf, &p, NULL, NULL))) {
        IMFMediaBuffer_Release(buf);
        return NULL;
    }
    if (alen) memcpy(p, a, alen);
    if (blen) memcpy(p + alen, b, blen);
    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_SetCurrentLength(buf, (DWORD)total);
    IMFSample *s = NULL;
    if (FAILED(g_mf.create_sample(&s))) {
        IMFMediaBuffer_Release(buf);
        return NULL;
    }
    IMFSample_AddBuffer(s, buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_SetSampleTime(s, (LONGLONG)pts_us * 10);
    return s;
}

int
ns_os_vdec_send(ns_os_vdec *d, const uint8_t *data, size_t len, int64_t pts_us)
{
    if (!d || !d->mft || !data || !len) return 0;
    uint8_t *annexb = NULL;
    size_t alen = 0;
    if (!ns_os_avcc_to_annexb(data, len, d->nal_len_size, &annexb, &alen))
        return 0;
    IMFSample *s = d->need_param_sets
        ? mf_sample_from(d->param_sets, d->param_sets_len, annexb, alen, pts_us)
        : mf_sample_from(NULL, 0, annexb, alen, pts_us);
    free(annexb);
    if (!s) return 0;
    if (d->need_param_sets)
        IMFSample_SetUINT32(s, &MFSampleExtension_Discontinuity, TRUE);
    HRESULT hr = IMFTransform_ProcessInput(d->mft, 0, s, 0);
    IMFSample_Release(s);
    if (hr == MF_E_NOTACCEPTING) return -1;
    if (FAILED(hr)) return 0;
    d->need_param_sets = 0;
    return 1;
}

static IMFSample *
mf_output_sample(DWORD size)
{
    IMFSample *s = NULL;
    IMFMediaBuffer *buf = NULL;
    if (FAILED(g_mf.create_sample(&s))) return NULL;
    if (FAILED(g_mf.create_memory_buffer(size ? size : 4096, &buf))) {
        IMFSample_Release(s);
        return NULL;
    }
    IMFSample_AddBuffer(s, buf);
    IMFMediaBuffer_Release(buf);
    return s;
}

static int
vdec_convert(ns_os_vdec *d, IMFSample *s, ns_os_frame *out)
{
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return 0;
    int ok = 0;
    IMF2DBuffer *b2 = NULL;
    BYTE *base = NULL;
    LONG pitch = 0;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer,
                                                (void **)&b2)) &&
        SUCCEEDED(IMF2DBuffer_Lock2D(b2, &base, &pitch)) && pitch > 0) {
        ok = ns_os_frame_copy_nv12(out, base, (int)pitch,
                                   base + (size_t)pitch * (size_t)d->out_height,
                                   (int)pitch, d->vis_width, d->vis_height);
        IMF2DBuffer_Unlock2D(b2);
    } else {
        DWORD cur = 0;
        if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &base, NULL, &cur))) {
            int stride = d->out_width;
            size_t need = (size_t)stride * (size_t)d->out_height * 3 / 2;
            if (cur >= need)
                ok = ns_os_frame_copy_nv12(out, base, stride,
                                           base + (size_t)stride * (size_t)d->out_height,
                                           stride, d->vis_width, d->vis_height);
            IMFMediaBuffer_Unlock(buf);
        }
    }
    if (b2) IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(buf);
    if (ok) {
        out->full_range = d->full_range;
        out->colorspace = d->colorspace;
        LONGLONG t = 0;
        out->pts_us = SUCCEEDED(IMFSample_GetSampleTime(s, &t)) ? t / 10 : 0;
    }
    return ok;
}

static void
vdec_report_path(const ns_os_vdec *d, IMFSample *s)
{
    int gpu = 0;
    IMFMediaBuffer *buf = NULL;
    if (SUCCEEDED(IMFSample_GetBufferByIndex(s, 0, &buf))) {
        IUnknown *dxgi = NULL;
        if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &ns_IID_IMFDXGIBuffer,
                                                    (void **)&dxgi))) {
            gpu = 1;
            IUnknown_Release(dxgi);
        }
        IMFMediaBuffer_Release(buf);
    }
    if (getenv("NS_DBG_MEDIA"))
        fprintf(stderr, "[media] MF H.264 %dx%d: %s\n", d->vis_width,
                d->vis_height, gpu ? "GPU (DXVA)" : "CPU");
}

ns_os_status
ns_os_vdec_receive(ns_os_vdec *d, ns_os_frame *out)
{
    if (!d || !d->mft) return NS_OS_ERROR;
    memset(out, 0, sizeof *out);
    for (int attempt = 0; attempt < 4; attempt++) {
        if (!d->output_ready && !vdec_pick_output(d)) return NS_OS_AGAIN;
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        if (!d->provides_samples) {
            ob.pSample = mf_output_sample(d->out_buf_size);
            if (!ob.pSample) return NS_OS_ERROR;
        }
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(d->mft, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            d->output_ready = 0;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            return NS_OS_AGAIN;
        }
        if (FAILED(hr)) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            return NS_OS_ERROR;
        }
        int ok = ob.pSample && vdec_convert(d, ob.pSample, out);
        if (ok && !d->hw_reported) {
            d->hw_reported = 1;
            vdec_report_path(d, ob.pSample);
        }
        if (ob.pSample) IMFSample_Release(ob.pSample);
        return ok ? NS_OS_FRAME : NS_OS_ERROR;
    }
    return NS_OS_ERROR;
}

void
ns_os_vdec_drain(ns_os_vdec *d)
{
    if (d && d->mft)
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
}

void
ns_os_vdec_flush(ns_os_vdec *d)
{
    if (!d || !d->mft) return;
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    d->need_param_sets = 1;
}

void
ns_os_vdec_free(ns_os_vdec *d)
{
    if (!d) return;
    if (d->mft) {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->mft);
    }
    free(d->param_sets);
    free(d);
}

static int
adec_pick_output(ns_os_adec *d)
{
    IMFMediaType *pcm16 = NULL;
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->mft, 0, i, &t)))
            break;
        GUID sub;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub))) {
            if (IsEqualGUID(&sub, &MFAudioFormat_Float) &&
                SUCCEEDED(IMFTransform_SetOutputType(d->mft, 0, t, 0))) {
                d->is_float = 1;
                if (pcm16) IMFMediaType_Release(pcm16);
                pcm16 = t;
                goto chosen;
            }
            UINT32 bits = 0;
            if (!pcm16 && IsEqualGUID(&sub, &MFAudioFormat_PCM) &&
                SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE,
                                                 &bits)) && bits == 16) {
                pcm16 = t;
                continue;
            }
        }
        IMFMediaType_Release(t);
    }
    if (!pcm16 || FAILED(IMFTransform_SetOutputType(d->mft, 0, pcm16, 0))) {
        if (pcm16) IMFMediaType_Release(pcm16);
        return 0;
    }
    d->is_float = 0;
chosen:;
    UINT32 rate = 0, ch = 0;
    IMFMediaType_GetUINT32(pcm16, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_GetUINT32(pcm16, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
    IMFMediaType_Release(pcm16);
    if (rate) d->rate = (int)rate;
    if (ch) d->channels = (int)ch;
    mf_stream_info(d->mft, &d->out_buf_size, &d->provides_samples);
    return 1;
}

static int
aac_rate_index(int rate)
{
    static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                   24000, 22050, 16000, 12000, 11025, 8000,
                                   7350 };
    for (int i = 0; i < 13; i++)
        if (rates[i] == rate) return i;
    return -1;
}

ns_os_adec *
ns_os_adec_new_aac(const uint8_t *asc, size_t asc_len, int rate, int channels)
{
    if (!mf_ready() || !asc || asc_len == 0 || asc_len > 64) return NULL;
    int cfg_rate = 0, cfg_ch = 0, aot = 0;
    uint8_t core_asc[2];
    if (ns_os_aac_config(asc, asc_len, &cfg_rate, &cfg_ch, &aot)) {
        rate = cfg_rate;
        if (cfg_ch > 0) channels = cfg_ch;
        int idx = aac_rate_index(cfg_rate);
        if ((aot == 5 || aot == 29) && idx >= 0 && cfg_ch > 0) {
            core_asc[0] = (uint8_t)((2 << 3) | (idx >> 1));
            core_asc[1] = (uint8_t)(((idx & 1) << 7) | (cfg_ch << 3));
            asc = core_asc;
            asc_len = sizeof core_asc;
        }
    }
    if (rate <= 0 || channels <= 0 || channels > 6) return NULL;
    ns_os_adec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->rate = rate;
    d->channels = channels;
    d->mft = mf_find_decoder(&MFT_CATEGORY_AUDIO_DECODER, &MFMediaType_Audio,
                             &MFAudioFormat_AAC);
    if (!d->mft) goto fail;

    uint8_t user[12 + 64];
    memset(user, 0, 12);
    memcpy(user + 12, asc, asc_len);
    IMFMediaType *in = NULL;
    if (FAILED(g_mf.create_media_type(&in))) goto fail;
    IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    IMFMediaType_SetUINT32(in, &MF_MT_AAC_PAYLOAD_TYPE, 0);
    IMFMediaType_SetBlob(in, &MF_MT_USER_DATA, user, (UINT32)(12 + asc_len));
    HRESULT hr = IMFTransform_SetInputType(d->mft, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr) || !adec_pick_output(d)) goto fail;
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
fail:
    ns_os_adec_free(d);
    return NULL;
}

static int
adec_append(ns_os_adec *d, IMFSample *s, float **pcm, size_t *frames,
            size_t *cap)
{
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return 0;
    BYTE *p = NULL;
    DWORD len = 0;
    int ok = 0;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
        size_t bps = d->is_float ? 4 : 2;
        size_t n = len / (bps * (size_t)d->channels);
        size_t need = (*frames + n) * (size_t)d->channels;
        if (need > *cap) {
            size_t ncap = *cap ? *cap * 2 : 8192;
            while (ncap < need) ncap *= 2;
            float *grown = realloc(*pcm, ncap * sizeof(float));
            if (grown) {
                *pcm = grown;
                *cap = ncap;
            }
        }
        if (need <= *cap) {
            float *dst = *pcm + *frames * (size_t)d->channels;
            size_t count = n * (size_t)d->channels;
            if (d->is_float) {
                memcpy(dst, p, count * sizeof(float));
            } else {
                for (size_t i = 0; i < count; i++) {
                    int16_t v;
                    memcpy(&v, p + i * 2, sizeof v);
                    dst[i] = (float)v / 32768.0f;
                }
            }
            *frames += n;
            ok = 1;
        }
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_Release(buf);
    return ok;
}

int
ns_os_adec_decode(ns_os_adec *d, const uint8_t *data, size_t len,
                  float **out_pcm, size_t *out_frames, int *out_rate,
                  int *out_channels)
{
    *out_pcm = NULL;
    *out_frames = 0;
    if (!d || !d->mft) return 0;
    HRESULT hr;
    if (data && len) {
        IMFSample *in = mf_sample_from(data, len, NULL, 0, 0);
        if (!in) return 0;
        hr = IMFTransform_ProcessInput(d->mft, 0, in, 0);
        IMFSample_Release(in);
        if (FAILED(hr)) return 0;
    } else {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
    float *pcm = NULL;
    size_t frames = 0, cap = 0;
    for (int guard = 0; guard < 64; guard++) {
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        if (!d->provides_samples) {
            ob.pSample = mf_output_sample(d->out_buf_size ? d->out_buf_size : 65536);
            if (!ob.pSample) break;
        }
        DWORD status = 0;
        hr = IMFTransform_ProcessOutput(d->mft, 0, 1, &ob, &status);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (!adec_pick_output(d)) break;
            continue;
        }
        if (FAILED(hr)) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            break;
        }
        if (ob.pSample) {
            adec_append(d, ob.pSample, &pcm, &frames, &cap);
            IMFSample_Release(ob.pSample);
        }
    }
    *out_pcm = pcm;
    *out_frames = frames;
    *out_rate = d->rate;
    *out_channels = d->channels;
    return frames > 0;
}

void
ns_os_adec_flush(ns_os_adec *d)
{
    if (!d || !d->mft) return;
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
}

void
ns_os_adec_free(ns_os_adec *d)
{
    if (!d) return;
    if (d->mft) {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->mft);
    }
    free(d);
}
