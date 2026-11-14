/* Nordstjernen — Web Audio graph rendering, offline and to the audio helper (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#include "js_internal.h"

#include <math.h>
#include <string.h>

#include <glib.h>

#define NS_WA_MAX_DEPTH 32
#define NS_WA_MAX_NODE_RENDERS 4096

typedef struct ns_wa_walk {
    int      depth;
    guint    renders_left;
    gboolean realtime;
    double   t0;
    int      channel;
    double   chunk;
} ns_wa_walk;

static void ns_wa_render(JSContext *ctx, JSValueConst node, uint32_t frames,
                         double rate, float *out, ns_wa_walk *walk);

static double
ns_wa_num(JSContext *ctx, JSValueConst obj, const char *name, double dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    double d = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToFloat64(ctx, &d, v) < 0)
        d = dflt;
    JS_FreeValue(ctx, v);
    return d;
}

static double
ns_wa_param(JSContext *ctx, JSValueConst node, const char *name, double dflt)
{
    JSValue p = JS_GetPropertyStr(ctx, node, name);
    double d = dflt;
    if (JS_IsObject(p)) d = ns_wa_num(ctx, p, "value", dflt);
    JS_FreeValue(ctx, p);
    return d;
}

static char *
ns_wa_str(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s = JS_ToCString(ctx, v);
    char *r = g_strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return r;
}

static float *
ns_wa_float32(JSContext *ctx, JSValueConst arr, uint32_t *count)
{
    *count = 0;
    if (!JS_IsObject(arr)) return NULL;
    size_t off = 0, blen = 0, bpe = 0, total = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, arr, &off, &blen, &bpe);
    if (JS_IsException(buf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
    JS_FreeValue(ctx, buf);
    if (!base || bpe != sizeof(float) || off + blen > total) return NULL;
    if ((off % sizeof(float)) != 0) return NULL;
    *count = (uint32_t)(blen / sizeof(float));
    return (float *)(void *)(base + off);
}

static void
ns_wa_window(JSContext *ctx, JSValueConst node, uint32_t frames, double rate,
             const ns_wa_walk *walk, uint32_t *first, uint32_t *last)
{
    double start = ns_wa_num(ctx, node, "_startTime", -1.0);
    double stop = ns_wa_num(ctx, node, "_stopTime", -1.0);
    *first = 0;
    *last = frames;
    if (walk->realtime) {
        if (start < 0) { *last = 0; return; }
        double f = ceil((start - walk->t0) * rate);
        *first = f <= 0 ? 0 : f >= frames ? frames : (uint32_t)f;
        if (stop >= 0) {
            double e = ceil((stop - walk->t0) * rate);
            uint32_t ue = e <= 0 ? 0 : e >= frames ? frames : (uint32_t)e;
            if (ue < *last) *last = ue;
        }
        if (*last < *first) *last = *first;
        return;
    }
    if (start > 0) {
        double f = start * rate;
        *first = f >= frames ? frames : (uint32_t)f;
    }
    if (stop >= 0) {
        double f = stop * rate;
        uint32_t e = f >= frames ? frames : (uint32_t)f;
        if (e < *last) *last = e;
    }
    if (*last < *first) *last = *first;
}

static void
ns_wa_oscillator(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out, const ns_wa_walk *walk)
{
    double freq = ns_wa_param(ctx, node, "frequency", 440.0);
    double detune = ns_wa_param(ctx, node, "detune", 0.0);
    double f = freq * pow(2.0, detune / 1200.0);
    if (!(f > 0) || f > rate * 0.5) f = f > 0 ? rate * 0.5 : 0.0;
    char *type = ns_wa_str(ctx, node, "type");
    uint32_t first, last;
    ns_wa_window(ctx, node, frames, rate, walk, &first, &last);
    double step = f / rate, phase = 0.0;
    if (walk->realtime) {
        double since = walk->t0 + first / rate -
                       ns_wa_num(ctx, node, "_startTime", 0.0);
        phase = since > 0 ? fmod(since * f, 1.0) : 0.0;
    }
    for (uint32_t i = first; i < last; i++) {
        double v;
        if (strcmp(type, "square") == 0)
            v = phase < 0.5 ? 1.0 : -1.0;
        else if (strcmp(type, "sawtooth") == 0)
            v = 2.0 * phase - 1.0;
        else if (strcmp(type, "triangle") == 0)
            v = phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
        else
            v = sin(2.0 * G_PI * phase);
        out[i] = (float)v;
        phase += step;
        if (phase >= 1.0) phase -= floor(phase);
    }
    g_free(type);
}

static void
ns_wa_buffer_source(JSContext *ctx, JSValueConst node, uint32_t frames,
                    double rate, float *out, const ns_wa_walk *walk)
{
    JSValue buf = JS_GetPropertyStr(ctx, node, "buffer");
    if (!JS_IsObject(buf)) { JS_FreeValue(ctx, buf); return; }
    double ratio = ns_wa_param(ctx, node, "playbackRate", 1.0);
    double brate = ns_wa_num(ctx, buf, "sampleRate", rate);
    if (!(ratio > 0)) ratio = 1.0;
    double step = ratio * (brate > 0 ? brate / rate : 1.0);
    if (!isfinite(step)) step = 1.0;
    JSValue loop_v = JS_GetPropertyStr(ctx, node, "loop");
    gboolean loop = JS_ToBool(ctx, loop_v);
    JS_FreeValue(ctx, loop_v);
    uint32_t first, last;
    ns_wa_window(ctx, node, frames, rate, walk, &first, &last);
    JSValue chans = JS_GetPropertyStr(ctx, buf, "_chans");
    uint32_t nch = JS_IsObject(chans) ? ns_js_array_length(ctx, chans) : 0;
    uint32_t pick = walk->realtime && nch > 0 && (uint32_t)walk->channel < nch
                  ? (uint32_t)walk->channel : (nch > 0 ? nch - 1 : 0);
    if (!walk->realtime) pick = 0;
    double start_time = walk->realtime
        ? ns_wa_num(ctx, node, "_startTime", 0.0) : 0.0;
    JSValue ch0 = JS_GetPropertyUint32(ctx, chans, pick);
    uint32_t n = 0;
    const float *src = ns_wa_float32(ctx, ch0, &n);
    if (src && n) {
        double pos = 0.0;
        if (walk->realtime) {
            double since = walk->t0 + first / rate - start_time;
            pos = since > 0 ? since * rate * step : 0.0;
            if (pos >= n && !loop) first = last;
        }
        for (uint32_t i = first; i < last; i++) {
            if (!(pos < n)) {
                if (!loop || !isfinite(pos)) break;
                pos = fmod(pos, (double)n);
            }
            out[i] = src[(uint32_t)pos];
            pos += step;
        }
    }
    JS_FreeValue(ctx, ch0);
    JS_FreeValue(ctx, chans);
    JS_FreeValue(ctx, buf);
}

static void
ns_wa_sum_inputs(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out, ns_wa_walk *walk)
{
    JSValue ins = JS_GetPropertyStr(ctx, node, "_inputs");
    if (!JS_IsObject(ins)) { JS_FreeValue(ctx, ins); return; }
    uint32_t n = ns_js_array_length(ctx, ins);
    if (!n) { JS_FreeValue(ctx, ins); return; }
    float *tmp = g_try_new0(float, frames);
    if (!tmp) { JS_FreeValue(ctx, ins); return; }
    walk->depth++;
    for (uint32_t i = 0; i < n && walk->renders_left > 0; i++) {
        JSValue src = JS_GetPropertyUint32(ctx, ins, i);
        if (JS_IsObject(src)) {
            memset(tmp, 0, frames * sizeof(float));
            ns_wa_render(ctx, src, frames, rate, tmp, walk);
            for (uint32_t j = 0; j < frames; j++) out[j] += tmp[j];
        }
        JS_FreeValue(ctx, src);
    }
    walk->depth--;
    g_free(tmp);
    JS_FreeValue(ctx, ins);
}

static void
ns_wa_compressor(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out)
{
    double threshold = ns_wa_param(ctx, node, "threshold", -24.0);
    double knee = ns_wa_param(ctx, node, "knee", 30.0);
    double ratio = ns_wa_param(ctx, node, "ratio", 12.0);
    double attack = ns_wa_param(ctx, node, "attack", 0.003);
    double release = ns_wa_param(ctx, node, "release", 0.25);
    if (ratio < 1.0) ratio = 1.0;
    if (knee < 0.0) knee = 0.0;
    double atk = attack > 0 ? exp(-1.0 / (attack * rate)) : 0.0;
    double rel = release > 0 ? exp(-1.0 / (release * rate)) : 0.0;
    double env = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = fabs((double)out[i]);
        double db = x > 1e-9 ? 20.0 * log10(x) : -180.0;
        double over = db - threshold;
        double reduction;
        if (knee > 0 && over > -knee * 0.5 && over < knee * 0.5) {
            double t = over + knee * 0.5;
            reduction = (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
        } else if (over <= 0) {
            reduction = 0.0;
        } else {
            reduction = over * (1.0 / ratio - 1.0);
        }
        double coeff = reduction < env ? atk : rel;
        env = coeff * env + (1.0 - coeff) * reduction;
        out[i] = (float)((double)out[i] * pow(10.0, env / 20.0));
    }
}

static void
ns_wa_biquad(JSContext *ctx, JSValueConst node, uint32_t frames,
             double rate, float *out)
{
    double f0 = ns_wa_param(ctx, node, "frequency", 350.0);
    double q = ns_wa_param(ctx, node, "Q", 1.0);
    double gain_db = ns_wa_param(ctx, node, "gain", 0.0);
    char *type = ns_wa_str(ctx, node, "type");
    if (!(f0 > 0)) f0 = 350.0;
    if (f0 > rate * 0.5) f0 = rate * 0.5;
    if (!(q > 0)) q = 1e-4;
    double w0 = 2.0 * G_PI * f0 / rate;
    double cw = cos(w0), sw = sin(w0), alpha = sw / (2.0 * q);
    double a0, a1, a2, b0, b1, b2;
    if (strcmp(type, "highpass") == 0) {
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "bandpass") == 0) {
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "notch") == 0) {
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "allpass") == 0) {
        b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "peaking") == 0) {
        double A = pow(10.0, gain_db / 40.0);
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    }
    g_free(type);
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = out[i];
        double y = (b0 / a0) * x + (b1 / a0) * x1 + (b2 / a0) * x2
                 - (a1 / a0) * y1 - (a2 / a0) * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        out[i] = (float)y;
    }
}

static void
ns_wa_delay(JSContext *ctx, JSValueConst node, uint32_t frames,
            double rate, float *out)
{
    double t = ns_wa_param(ctx, node, "delayTime", 0.0);
    if (!(t > 0)) return;
    uint32_t d = (uint32_t)(t * rate);
    if (d == 0 || d >= frames) {
        if (d >= frames) memset(out, 0, frames * sizeof(float));
        return;
    }
    for (uint32_t i = frames; i-- > d;) out[i] = out[i - d];
    memset(out, 0, d * sizeof(float));
}

static void
ns_wa_waveshaper(JSContext *ctx, JSValueConst node, uint32_t frames,
                 float *out)
{
    JSValue curve = JS_GetPropertyStr(ctx, node, "curve");
    uint32_t n = 0;
    const float *c = ns_wa_float32(ctx, curve, &n);
    if (c && n >= 2) {
        for (uint32_t i = 0; i < frames; i++) {
            double x = out[i];
            if (x < -1.0) x = -1.0;
            if (x > 1.0) x = 1.0;
            double pos = (x + 1.0) * 0.5 * (n - 1);
            uint32_t k = (uint32_t)pos;
            if (k >= n - 1) { out[i] = c[n - 1]; continue; }
            double frac = pos - k;
            out[i] = (float)(c[k] * (1.0 - frac) + c[k + 1] * frac);
        }
    }
    JS_FreeValue(ctx, curve);
}

static void
ns_wa_script_processor(JSContext *ctx, JSValueConst node, uint32_t frames,
                       float *out, const ns_wa_walk *walk)
{
    if (!walk->realtime) return;
    double size = ns_wa_num(ctx, node, "bufferSize", 1024.0);
    if (!(size >= frames)) return;
    if (ns_wa_num(ctx, node, "_rtChunk", -1.0) != walk->chunk) {
        double pos = ns_wa_num(ctx, node, "_rtPos", size);
        if (pos + frames > size) {
            JSValue outbuf = JS_GetPropertyStr(ctx, node, "_rtOut");
            JSValue chans = JS_GetPropertyStr(ctx, outbuf, "_chans");
            uint32_t nch = JS_IsObject(chans) ? ns_js_array_length(ctx, chans) : 0;
            for (uint32_t c = 0; c < nch; c++) {
                JSValue arr = JS_GetPropertyUint32(ctx, chans, c);
                uint32_t n = 0;
                float *data = ns_wa_float32(ctx, arr, &n);
                if (data) memset(data, 0, n * sizeof(float));
                JS_FreeValue(ctx, arr);
            }
            JS_FreeValue(ctx, chans);
            JSValue handler = JS_GetPropertyStr(ctx, node, "onaudioprocess");
            if (JS_IsFunction(ctx, handler)) {
                JSValue ev = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, "audioprocess"));
                JS_SetPropertyStr(ctx, ev, "target", JS_DupValue(ctx, node));
                JS_SetPropertyStr(ctx, ev, "outputBuffer", JS_DupValue(ctx, outbuf));
                JS_SetPropertyStr(ctx, ev, "inputBuffer",
                                  JS_GetPropertyStr(ctx, node, "_rtIn"));
                JS_SetPropertyStr(ctx, ev, "playbackTime",
                                  JS_NewFloat64(ctx, walk->t0));
                JSValue r = JS_Call(ctx, handler, node, 1, (JSValueConst *)&ev);
                if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
                JS_FreeValue(ctx, r);
                JS_FreeValue(ctx, ev);
            }
            JS_FreeValue(ctx, handler);
            JS_FreeValue(ctx, outbuf);
            pos = 0;
        }
        JS_SetPropertyStr(ctx, node, "_rtChunk", JS_NewFloat64(ctx, walk->chunk));
        JS_SetPropertyStr(ctx, node, "_rtChunkPos", JS_NewFloat64(ctx, pos));
        JS_SetPropertyStr(ctx, node, "_rtPos", JS_NewFloat64(ctx, pos + frames));
    }
    uint32_t at = (uint32_t)ns_wa_num(ctx, node, "_rtChunkPos", 0.0);
    JSValue outbuf = JS_GetPropertyStr(ctx, node, "_rtOut");
    JSValue chans = JS_GetPropertyStr(ctx, outbuf, "_chans");
    uint32_t nch = JS_IsObject(chans) ? ns_js_array_length(ctx, chans) : 0;
    if (nch) {
        uint32_t c = (uint32_t)walk->channel < nch ? (uint32_t)walk->channel : nch - 1;
        JSValue arr = JS_GetPropertyUint32(ctx, chans, c);
        uint32_t n = 0;
        const float *data = ns_wa_float32(ctx, arr, &n);
        for (uint32_t i = 0; data && i < frames && at + i < n; i++)
            out[i] = data[at + i];
        JS_FreeValue(ctx, arr);
    }
    JS_FreeValue(ctx, chans);
    JS_FreeValue(ctx, outbuf);
}

static void
ns_wa_render(JSContext *ctx, JSValueConst node, uint32_t frames,
             double rate, float *out, ns_wa_walk *walk)
{
    if (walk->depth > NS_WA_MAX_DEPTH || walk->renders_left == 0 ||
        !JS_IsObject(node))
        return;
    walk->renders_left--;
    char *kind = ns_wa_str(ctx, node, "_kind");

    if (strcmp(kind, "oscillator") == 0) {
        ns_wa_oscillator(ctx, node, frames, rate, out, walk);
    } else if (strcmp(kind, "buffersource") == 0) {
        ns_wa_buffer_source(ctx, node, frames, rate, out, walk);
    } else if (strcmp(kind, "scriptprocessor") == 0) {
        ns_wa_script_processor(ctx, node, frames, out, walk);
    } else if (strcmp(kind, "constant") == 0) {
        double v = ns_wa_param(ctx, node, "offset", 1.0);
        uint32_t first, last;
        ns_wa_window(ctx, node, frames, rate, walk, &first, &last);
        for (uint32_t i = first; i < last; i++) out[i] = (float)v;
    } else {
        ns_wa_sum_inputs(ctx, node, frames, rate, out, walk);
        if (strcmp(kind, "gain") == 0) {
            double g = ns_wa_param(ctx, node, "gain", 1.0);
            for (uint32_t i = 0; i < frames; i++) out[i] = (float)(out[i] * g);
        } else if (strcmp(kind, "compressor") == 0) {
            ns_wa_compressor(ctx, node, frames, rate, out);
        } else if (strcmp(kind, "biquad") == 0) {
            ns_wa_biquad(ctx, node, frames, rate, out);
        } else if (strcmp(kind, "delay") == 0) {
            ns_wa_delay(ctx, node, frames, rate, out);
        } else if (strcmp(kind, "waveshaper") == 0) {
            ns_wa_waveshaper(ctx, node, frames, out);
        } else if (strcmp(kind, "stereopanner") == 0 ||
                   strcmp(kind, "panner") == 0) {
            double g = 1.0 - fabs(ns_wa_param(ctx, node, "pan", 0.0)) * 0.5;
            for (uint32_t i = 0; i < frames; i++) out[i] = (float)(out[i] * g);
        }
    }
    g_free(kind);
}

gboolean
ns_webaudio_render_offline(JSContext *ctx, JSValueConst destination,
                           uint32_t frames, double rate, float *out)
{
    if (!ctx || !out || !frames || !(rate > 0)) return FALSE;
    memset(out, 0, frames * sizeof(float));
    if (!JS_IsObject(destination)) return FALSE;
    ns_wa_walk walk = { 0, NS_WA_MAX_NODE_RENDERS, FALSE, 0.0, 0, 0.0 };
    ns_wa_render(ctx, destination, frames, rate, out, &walk);
    for (uint32_t i = 0; i < frames; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
    return TRUE;
}

#define NS_WA_RT_CHUNK      256u
#define NS_WA_RT_LEAD_S     0.12
#define NS_WA_RT_BEHIND_S   0.25
#define NS_WA_RT_MAX_RATE   24000.0
#define NS_WA_RT_LINE_BYTES 2400u

typedef struct ns_wa_rt {
    JSContext *ctx;
    JSValue    obj;
    double     rate;
    char       token[32];
    gboolean   running;
    gboolean   closed;
    gint64     start_us;
    double     base_frames;
    double     frames_done;
    double     chunks;
} ns_wa_rt;

static ns_wa_rt *
ns_wa_rt_find(ns_js *js, JSValueConst obj)
{
    for (guint i = 0; js->webaudio_rt && i < js->webaudio_rt->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_rt, i);
        if (JS_VALUE_GET_PTR(rt->obj) == JS_VALUE_GET_PTR(obj)) return rt;
    }
    return NULL;
}

static double
ns_wa_rt_elapsed(const ns_wa_rt *rt, gint64 now)
{
    if (!rt->running) return rt->base_frames;
    return rt->base_frames + (double)(now - rt->start_us) * rt->rate / 1e6;
}

static void
ns_wa_rt_free_one(gpointer data)
{
    ns_wa_rt *rt = data;
    JS_FreeValue(rt->ctx, rt->obj);
    g_free(rt);
}

void
ns_webaudio_rt_add(ns_js *js, JSContext *ctx, JSValueConst obj, double rate,
                   gboolean running)
{
    static guint serial;
    if (!js || !JS_IsObject(obj)) return;
    if (!js->webaudio_rt)
        js->webaudio_rt = g_ptr_array_new_with_free_func(ns_wa_rt_free_one);
    ns_wa_rt *rt = g_new0(ns_wa_rt, 1);
    rt->ctx = ctx;
    ns_js_pin_realm(ctx);
    rt->obj = JS_DupValue(ctx, obj);
    rt->rate = rate > 0 ? rate : 44100.0;
    g_snprintf(rt->token, sizeof rt->token, "wa%u", ++serial);
    rt->running = running;
    rt->start_us = g_get_monotonic_time();
    g_ptr_array_add(js->webaudio_rt, rt);
}

void
ns_webaudio_rt_set_state(ns_js *js, JSValueConst obj, const char *state)
{
    ns_wa_rt *rt = js ? ns_wa_rt_find(js, obj) : NULL;
    if (!rt || rt->closed) return;
    gint64 now = g_get_monotonic_time();
    gboolean run = strcmp(state, "running") == 0;
    if (run && !rt->running) {
        rt->running = TRUE;
        rt->start_us = now;
        if (rt->frames_done < rt->base_frames) rt->frames_done = rt->base_frames;
    } else if (!run && rt->running) {
        rt->base_frames = ns_wa_rt_elapsed(rt, now);
        rt->running = FALSE;
    }
    if (strcmp(state, "closed") == 0) {
        rt->closed = TRUE;
        if (js->audio_cb) {
            char *cmd = g_strdup_printf("stop %s", rt->token);
            js->audio_cb(cmd, js->audio_user_data);
            g_free(cmd);
        }
    }
    JS_SetPropertyStr(rt->ctx, rt->obj, "state", JS_NewString(rt->ctx, state));
}

static gboolean
ns_wa_rt_has_inputs(const ns_wa_rt *rt)
{
    JSValue dest = JS_GetPropertyStr(rt->ctx, rt->obj, "destination");
    JSValue ins = JS_GetPropertyStr(rt->ctx, dest, "_inputs");
    gboolean any = JS_IsObject(ins) && ns_js_array_length(rt->ctx, ins) > 0;
    JS_FreeValue(rt->ctx, ins);
    JS_FreeValue(rt->ctx, dest);
    return any;
}

gboolean
ns_webaudio_rt_busy(ns_js *js)
{
    for (guint i = 0; js && js->webaudio_rt && i < js->webaudio_rt->len; i++) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_rt, i);
        if (rt->running && !rt->closed && ns_wa_rt_has_inputs(rt)) return TRUE;
    }
    return FALSE;
}

static void
ns_wa_rt_emit(ns_js *js, const ns_wa_rt *rt, double tx_rate,
              const gint16 *pcm, gsize frames)
{
    gsize bytes = frames * 2 * sizeof(gint16);
    const guint8 *p = (const guint8 *)pcm;
    for (gsize off = 0; off < bytes; off += NS_WA_RT_LINE_BYTES) {
        gsize n = MIN(NS_WA_RT_LINE_BYTES, bytes - off);
        gchar *b64 = g_base64_encode(p + off, n);
        char *cmd = g_strdup_printf("pcm %s %d 2 %s", rt->token, (int)tx_rate, b64);
        js->audio_cb(cmd, js->audio_user_data);
        g_free(cmd);
        g_free(b64);
    }
}

static gint16
ns_wa_s16(float v)
{
    if (v > 1.0f) v = 1.0f;
    else if (v < -1.0f) v = -1.0f;
    return (gint16)lrintf(v * 32767.0f);
}

static void
ns_wa_rt_render(ns_js *js, ns_wa_rt *rt, gint64 now)
{
    JSContext *ctx = rt->ctx;
    double elapsed = ns_wa_rt_elapsed(rt, now);
    JS_SetPropertyStr(ctx, rt->obj, "currentTime",
                      JS_NewFloat64(ctx, elapsed / rt->rate));
    if (!rt->running || rt->closed) return;
    if (rt->frames_done < elapsed - NS_WA_RT_BEHIND_S * rt->rate)
        rt->frames_done = floor(elapsed);
    double target = elapsed + NS_WA_RT_LEAD_S * rt->rate;
    if (rt->frames_done >= target) return;
    if (!ns_wa_rt_has_inputs(rt)) {
        rt->frames_done = target;
        return;
    }
    guint chunks = (guint)ceil((target - rt->frames_done) / NS_WA_RT_CHUNK);
    gboolean halve = rt->rate > NS_WA_RT_MAX_RATE;
    double tx_rate = halve ? rt->rate / 2 : rt->rate;
    gsize cap = (gsize)chunks * NS_WA_RT_CHUNK;
    gint16 *pcm = g_new0(gint16, cap * 2);
    gsize out_frames = 0;
    gboolean audible = FALSE;
    float left[NS_WA_RT_CHUNK], right[NS_WA_RT_CHUNK];
    JSValue dest = JS_GetPropertyStr(ctx, rt->obj, "destination");
    for (guint c = 0; c < chunks; c++) {
        ns_wa_walk walk = { 0, NS_WA_MAX_NODE_RENDERS, TRUE,
                            rt->frames_done / rt->rate, 0, rt->chunks };
        memset(left, 0, sizeof left);
        ns_wa_render(ctx, dest, NS_WA_RT_CHUNK, rt->rate, left, &walk);
        walk.depth = 0;
        walk.renders_left = NS_WA_MAX_NODE_RENDERS;
        walk.channel = 1;
        memset(right, 0, sizeof right);
        ns_wa_render(ctx, dest, NS_WA_RT_CHUNK, rt->rate, right, &walk);
        rt->chunks += 1;
        rt->frames_done += NS_WA_RT_CHUNK;
        guint step = halve ? 2 : 1;
        for (guint i = 0; i + step <= NS_WA_RT_CHUNK; i += step) {
            float l = halve ? (left[i] + left[i + 1]) * 0.5f : left[i];
            float r = halve ? (right[i] + right[i + 1]) * 0.5f : right[i];
            if (l != 0.0f || r != 0.0f) audible = TRUE;
            pcm[out_frames * 2] = ns_wa_s16(l);
            pcm[out_frames * 2 + 1] = ns_wa_s16(r);
            out_frames++;
        }
    }
    JS_FreeValue(ctx, dest);
    if (audible && js->audio_cb) ns_wa_rt_emit(js, rt, tx_rate, pcm, out_frames);
    g_free(pcm);
}

double
ns_webaudio_schedule_floor(ns_js *js, JSContext *ctx, JSValueConst audio_ctx)
{
    const ns_wa_rt *rt = js ? ns_wa_rt_find(js, audio_ctx) : NULL;
    if (!rt || rt->closed || !(rt->rate > 0))
        return ns_wa_num(ctx, audio_ctx, "currentTime", 0.0);
    double elapsed = ns_wa_rt_elapsed(rt, g_get_monotonic_time());
    double next = rt->frames_done < elapsed - NS_WA_RT_BEHIND_S * rt->rate
                ? floor(elapsed) : rt->frames_done;
    return next / rt->rate;
}

typedef struct {
    ns_js    *js;
    ns_wa_rt *rt;
    gint64    now;
} ns_wa_rt_pass;

static void
ns_wa_rt_render_pass(gpointer data)
{
    ns_wa_rt_pass *pass = data;
    ns_wa_rt_render(pass->js, pass->rt, pass->now);
}

gboolean
ns_webaudio_rt_pump(ns_js *js)
{
    if (!js || !js->webaudio_rt) return FALSE;
    gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < js->webaudio_rt->len; ) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_rt, i);
        if (rt->closed) {
            g_ptr_array_remove_index(js->webaudio_rt, i);
            continue;
        }
        ns_wa_rt_pass pass = { js, rt, now };
        ns_js_run_in_realm(js, rt->ctx, ns_wa_rt_render_pass, &pass);
        i++;
    }
    return js->webaudio_rt->len > 0;
}

void
ns_webaudio_rt_free(ns_js *js)
{
    if (!js || !js->webaudio_rt) return;
    for (guint i = 0; i < js->webaudio_rt->len && js->audio_cb; i++) {
        ns_wa_rt *rt = g_ptr_array_index(js->webaudio_rt, i);
        char *cmd = g_strdup_printf("stop %s", rt->token);
        js->audio_cb(cmd, js->audio_user_data);
        g_free(cmd);
    }
    g_clear_pointer(&js->webaudio_rt, g_ptr_array_unref);
}
