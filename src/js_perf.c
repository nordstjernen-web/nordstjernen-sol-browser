/* Nordstjernen — Performance API: performance.*, PerformanceObserver (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "js_internal.h"
#include "js_classid.h"
#include "net.h"

#include <math.h>
#include <string.h>

#define NS_PERF_ENTRY_CAP   256

typedef struct ns_perf_entry {
    char  *name;
    char  *type;
    char  *initiator_type;
    double start_time;
    double duration;
    gint64 transfer_size;
    gint64 encoded_size;
    gboolean has_timing;
    char  *next_hop_protocol;
    int    response_status;
    double fetch_start, domain_lookup_start, domain_lookup_end;
    double connect_start, connect_end, secure_connection_start;
    double request_start, response_start, response_end;
    gboolean render_blocking;
    gboolean preload_unclaimed;
    gconstpointer realm;
} ns_perf_entry;

void
ns_perf_entry_free(gpointer p)
{
    ns_perf_entry *e = p;
    if (!e) return;
    g_free(e->name);
    g_free(e->type);
    g_free(e->initiator_type);
    g_free(e->next_hop_protocol);
    g_free(e);
}

static ns_perf_entry *
ns_perf_entry_clone(const ns_perf_entry *e)
{
    if (!e) return NULL;
    ns_perf_entry *copy = g_new0(ns_perf_entry, 1);
    copy->name       = g_strdup(e->name ? e->name : "");
    copy->type       = g_strdup(e->type ? e->type : "");
    copy->initiator_type = g_strdup(e->initiator_type ? e->initiator_type : "");
    copy->start_time = e->start_time;
    copy->duration   = e->duration;
    copy->transfer_size = e->transfer_size;
    copy->encoded_size = e->encoded_size;
    copy->has_timing = e->has_timing;
    copy->next_hop_protocol = g_strdup(e->next_hop_protocol);
    copy->response_status = e->response_status;
    copy->fetch_start = e->fetch_start;
    copy->domain_lookup_start = e->domain_lookup_start;
    copy->domain_lookup_end = e->domain_lookup_end;
    copy->connect_start = e->connect_start;
    copy->connect_end = e->connect_end;
    copy->secure_connection_start = e->secure_connection_start;
    copy->request_start = e->request_start;
    copy->response_start = e->response_start;
    copy->response_end = e->response_end;
    copy->render_blocking = e->render_blocking;
    copy->realm = e->realm;
    return copy;
}

static gconstpointer
ns_perf_realm_key(const ns_js *js, gconstpointer timeline)
{
    if (!js) return timeline;
    return timeline == js->main_realm_ctx ? NULL : timeline;
}

static JSClassID ns_performance_class_id;

typedef struct ns_performance_data {
    gconstpointer realm;
    JSValue timing;
    JSValue navigation;
    JSValue event_counts;
} ns_performance_data;

static void
ns_performance_finalizer(JSRuntime *rt, JSValue val)
{
    ns_performance_data *d = JS_GetOpaque(val, ns_performance_class_id);
    if (!d) return;
    JS_FreeValueRT(rt, d->timing);
    JS_FreeValueRT(rt, d->navigation);
    JS_FreeValueRT(rt, d->event_counts);
    g_free(d);
}

static void
ns_performance_gc_mark(JSRuntime *rt, JSValueConst val,
                       JS_MarkFunc *mark_func)
{
    ns_performance_data *d = JS_GetOpaque(val, ns_performance_class_id);
    if (!d) return;
    JS_MarkValue(rt, d->timing, mark_func);
    JS_MarkValue(rt, d->navigation, mark_func);
    JS_MarkValue(rt, d->event_counts, mark_func);
}

static JSClassDef ns_performance_class = {
    .class_name = "Performance",
    .finalizer = ns_performance_finalizer,
    .gc_mark = ns_performance_gc_mark,
};

JSValue
ns_perf_new_performance_object(JSContext *ctx)
{
    ns_new_class_id(&ns_performance_class_id);
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!JS_IsRegisteredClass(rt, ns_performance_class_id))
        JS_NewClass(rt, ns_performance_class_id, &ns_performance_class);
    JSValue o = JS_NewObjectClass(ctx, ns_performance_class_id);
    if (JS_IsException(o)) return o;
    ns_performance_data *d = g_new0(ns_performance_data, 1);
    d->realm = ns_perf_realm_key(js_from_ctx(ctx), ctx);
    d->timing = JS_UNDEFINED;
    d->navigation = JS_UNDEFINED;
    d->event_counts = JS_UNDEFINED;
    JS_SetOpaque(o, d);
    return o;
}

void
ns_perf_set_performance_objects(JSContext *ctx, JSValueConst perf,
                                JSValue timing, JSValue navigation,
                                JSValue event_counts)
{
    ns_performance_data *d = ns_performance_class_id
        ? JS_GetOpaque(perf, ns_performance_class_id) : NULL;
    if (!d) {
        JS_FreeValue(ctx, timing);
        JS_FreeValue(ctx, navigation);
        JS_FreeValue(ctx, event_counts);
        return;
    }
    JS_FreeValue(ctx, d->timing);
    JS_FreeValue(ctx, d->navigation);
    JS_FreeValue(ctx, d->event_counts);
    d->timing = timing;
    d->navigation = navigation;
    d->event_counts = event_counts;
}

static ns_performance_data *
ns_perf_data_of(JSValueConst this_val)
{
    return ns_performance_class_id
        ? JS_GetOpaque(this_val, ns_performance_class_id) : NULL;
}

static gconstpointer
ns_perf_this_realm(const ns_js *js, JSValueConst this_val)
{
    ns_performance_data *d = ns_perf_data_of(this_val);
    return ns_perf_realm_key(js, d ? d->realm : NULL);
}

JSValue
ns_window_performance_time_origin_get(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_performance_data *d = ns_perf_data_of(this_val);
    if (!d) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewFloat64(ctx, ns_js_time_origin_real_ms(js_from_ctx(ctx),
                                                        d->realm));
}

JSValue
ns_window_performance_object_get(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic)
{
    (void)argc; (void)argv;
    ns_performance_data *d = ns_perf_data_of(this_val);
    if (!d) return JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValueConst v = magic == 0 ? d->timing
                   : magic == 1 ? d->navigation : d->event_counts;
    return JS_DupValue(ctx, v);
}

#define NS_TIMER_RESOLUTION_US 100

double
ns_perf_relative_ms(gint64 now_us, gint64 origin_us)
{
    now_us = (now_us / NS_TIMER_RESOLUTION_US) * NS_TIMER_RESOLUTION_US;
    origin_us = (origin_us / NS_TIMER_RESOLUTION_US) * NS_TIMER_RESOLUTION_US;
    if (now_us < origin_us) return 0;
    return (double)(now_us - origin_us) / 1000.0;
}

double
ns_perf_now_ms(const ns_js *js)
{
    gint64 origin = js ? js->time_origin_us : 0;
    return ns_perf_relative_ms(g_get_monotonic_time(), origin);
}

gint64
ns_js_time_origin_us(const ns_js *js, gconstpointer realm)
{
    if (!js) return 0;
    const ns_realm_origin *o = js->realm_origins && realm
        ? g_hash_table_lookup(js->realm_origins, realm) : NULL;
    return o ? o->origin_us : js->time_origin_us;
}

double
ns_js_time_origin_real_ms(const ns_js *js, gconstpointer realm)
{
    if (!js) return 0.0;
    const ns_realm_origin *o = js->realm_origins && realm
        ? g_hash_table_lookup(js->realm_origins, realm) : NULL;
    return o ? o->origin_real_ms : js->time_origin_real_ms;
}

double
ns_perf_realm_now_ms(JSContext *ctx)
{
    ns_js *js = js_from_ctx(ctx);
    return ns_perf_relative_ms(g_get_monotonic_time(),
                               ns_js_time_origin_us(js, ctx));
}

void
ns_js_start_frame_clock(ns_js *js, gconstpointer frame)
{
    if (!js || !frame) return;
    if (!js->realm_origins)
        js->realm_origins = g_hash_table_new_full(g_direct_hash,
                                                  g_direct_equal, NULL,
                                                  g_free);
    ns_realm_origin *o = g_new0(ns_realm_origin, 1);
    o->origin_us = (g_get_monotonic_time() / NS_TIMER_RESOLUTION_US) *
                   NS_TIMER_RESOLUTION_US;
    o->origin_real_ms = floor((double)g_get_real_time() / 100.0) / 10.0;
    g_hash_table_replace(js->realm_origins, (gpointer)frame, o);
}

void
ns_js_adopt_frame_clock(ns_js *js, gconstpointer frame, JSContext *ctx)
{
    if (!js || !ctx) return;
    ns_realm_origin *o = NULL;
    if (frame && js->realm_origins &&
        g_hash_table_steal_extended(js->realm_origins, frame, NULL,
                                    (gpointer *)&o)) {
        g_hash_table_replace(js->realm_origins, ctx, o);
        return;
    }
    ns_js_start_frame_clock(js, ctx);
}

void
ns_js_clear_frame_clocks(ns_js *js, gboolean destroy)
{
    if (!js || !js->realm_origins) return;
    if (destroy) {
        g_hash_table_destroy(js->realm_origins);
        js->realm_origins = NULL;
    } else {
        g_hash_table_remove_all(js->realm_origins);
    }
}

JSValue
ns_window_performance_now(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_performance_data *d = ns_perf_data_of(this_val);
    if (!d) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewFloat64(ctx, ns_perf_relative_ms(g_get_monotonic_time(),
        ns_js_time_origin_us(js_from_ctx(ctx), d->realm)));
}

static JSValue
ns_perf_entry_to_js(JSContext *ctx, const ns_perf_entry *entry)
{
    ns_perf_entry *e = ns_perf_entry_clone(entry);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e->name ? e->name : ""));
    JS_SetPropertyStr(ctx, o, "entryType",
                      JS_NewString(ctx, e->type ? e->type : ""));
    JS_SetPropertyStr(ctx, o, "startTime", JS_NewFloat64(ctx, e->start_time));
    JS_SetPropertyStr(ctx, o, "duration",  JS_NewFloat64(ctx, e->duration));
    if (e->type && strcmp(e->type, "longtask") == 0) {
        JSValue attribution = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, attribution, "name", JS_NewString(ctx, "unknown"));
        JS_SetPropertyStr(ctx, attribution, "entryType",
                          JS_NewString(ctx, "taskattribution"));
        JS_SetPropertyStr(ctx, attribution, "startTime", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, attribution, "duration", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, attribution, "containerType",
                          JS_NewString(ctx, "window"));
        JS_SetPropertyStr(ctx, attribution, "containerSrc", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, attribution, "containerId", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, attribution, "containerName", JS_NewString(ctx, ""));
        JSValue list = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, list, 0, attribution);
        JS_SetPropertyStr(ctx, o, "attribution", list);
    }
    if (e->type && strcmp(e->type, "long-animation-frame") == 0) {
        double end = e->start_time + e->duration;
        JS_SetPropertyStr(ctx, o, "renderStart", JS_NewFloat64(ctx, end));
        JS_SetPropertyStr(ctx, o, "styleAndLayoutStart", JS_NewFloat64(ctx, end));
        JS_SetPropertyStr(ctx, o, "blockingDuration",
                          JS_NewFloat64(ctx, MAX(0.0, e->duration - 50.0)));
        JS_SetPropertyStr(ctx, o, "firstUIEventTimestamp", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "styleDuration", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "forcedStyleDuration", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "scripts", JS_NewArray(ctx));
    }
    if (e->type && strcmp(e->type, "resource") == 0) {
        double end = e->start_time + e->duration;
        gboolean t = e->has_timing;
        JS_SetPropertyStr(ctx, o, "initiatorType",
                          JS_NewString(ctx, e->initiator_type
                                            ? e->initiator_type : "other"));
        JS_SetPropertyStr(ctx, o, "nextHopProtocol",
                          JS_NewString(ctx, e->next_hop_protocol
                                            ? e->next_hop_protocol : ""));
        JS_SetPropertyStr(ctx, o, "workerStart", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "redirectStart", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, o, "redirectEnd", JS_NewFloat64(ctx, 0));
        static const struct { const char *name; gsize off; } phases[] = {
            { "fetchStart", G_STRUCT_OFFSET(ns_perf_entry, fetch_start) },
            { "domainLookupStart",
              G_STRUCT_OFFSET(ns_perf_entry, domain_lookup_start) },
            { "domainLookupEnd",
              G_STRUCT_OFFSET(ns_perf_entry, domain_lookup_end) },
            { "connectStart", G_STRUCT_OFFSET(ns_perf_entry, connect_start) },
            { "connectEnd", G_STRUCT_OFFSET(ns_perf_entry, connect_end) },
            { "secureConnectionStart",
              G_STRUCT_OFFSET(ns_perf_entry, secure_connection_start) },
            { "requestStart", G_STRUCT_OFFSET(ns_perf_entry, request_start) },
            { "responseStart", G_STRUCT_OFFSET(ns_perf_entry, response_start) },
            { "responseEnd", G_STRUCT_OFFSET(ns_perf_entry, response_end) },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(phases); i++) {
            double v = t ? G_STRUCT_MEMBER(double, e, phases[i].off)
                     : i >= 7 ? end : e->start_time;
            JS_SetPropertyStr(ctx, o, phases[i].name, JS_NewFloat64(ctx, v));
        }
        JS_SetPropertyStr(ctx, o, "transferSize",
                          JS_NewInt64(ctx, e->transfer_size));
        JS_SetPropertyStr(ctx, o, "encodedBodySize",
                          JS_NewInt64(ctx, e->encoded_size));
        JS_SetPropertyStr(ctx, o, "decodedBodySize",
                          JS_NewInt64(ctx, e->encoded_size));
        JS_SetPropertyStr(ctx, o, "serverTiming", JS_NewArray(ctx));
        JS_SetPropertyStr(ctx, o, "responseStatus",
                          JS_NewInt32(ctx, e->response_status));
        JS_SetPropertyStr(ctx, o, "renderBlockingStatus",
                          JS_NewString(ctx, e->render_blocking
                                            ? "blocking" : "non-blocking"));
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, g, "PerformanceResourceTiming");
        JSValue proto = JS_IsObject(ctor)
            ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
        if (JS_IsObject(proto)) JS_SetPrototype(ctx, o, proto);
        JS_FreeValue(ctx, proto);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, g);
    }
    ns_perf_entry_free(e);
    ns_bind_fn(ctx, o, "toJSON", ns_own_data_props_toJSON, 0);
    return o;
}

static JSValue
ns_perf_build_navigation_entry(JSContext *ctx, ns_js *js)
{
    JSValue o = JS_NewObject(ctx);
    const char *url = js && js->current_url ? js->current_url : "";
    const ns_js_navigation_timing *t = js ? &js->navigation_timing : NULL;
    double complete = t ? t->load_event_end_ms : 0;
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, url));
    JS_SetPropertyStr(ctx, o, "entryType", JS_NewString(ctx, "navigation"));
    JS_SetPropertyStr(ctx, o, "startTime", JS_NewFloat64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "duration", JS_NewFloat64(ctx, complete));
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, "navigate"));
    JS_SetPropertyStr(ctx, o, "initiatorType", JS_NewString(ctx, "navigation"));
    JS_SetPropertyStr(ctx, o, "nextHopProtocol", JS_NewString(ctx, "h2"));
    JS_SetPropertyStr(ctx, o, "redirectCount", JS_NewInt32(ctx, 0));
    JS_SetPropertyStr(ctx, o, "workerStart", JS_NewFloat64(ctx, 0));
    const struct { const char *k; double v; } f[] = {
        {"unloadEventStart",0},{"unloadEventEnd",0},{"redirectStart",0},
        {"redirectEnd",0},{"fetchStart",0},
        {"domainLookupStart",t ? t->domain_lookup_start_ms : 0},
        {"domainLookupEnd",t ? t->domain_lookup_end_ms : 0},
        {"connectStart",t ? t->connect_start_ms : 0},
        {"connectEnd",t ? t->connect_end_ms : 0},
        {"secureConnectionStart",t ? t->secure_connection_start_ms : 0},
        {"requestStart",t ? t->request_start_ms : 0},
        {"responseStart",t ? t->response_start_ms : 0},
        {"responseEnd",t ? t->response_end_ms : 0},
        {"domLoading",t ? t->dom_loading_ms : 0},
        {"domInteractive",t ? t->dom_interactive_ms : 0},
        {"domContentLoadedEventStart",
         t ? t->dom_content_loaded_event_start_ms : 0},
        {"domContentLoadedEventEnd",
         t ? t->dom_content_loaded_event_end_ms : 0},
        {"domComplete",t ? t->dom_complete_ms : 0},
        {"loadEventStart",t ? t->load_event_start_ms : 0},
        {"loadEventEnd",complete},
    };
    for (gsize i = 0; i < G_N_ELEMENTS(f); i++)
        JS_SetPropertyStr(ctx, o, f[i].k, JS_NewFloat64(ctx, f[i].v));
    JS_SetPropertyStr(ctx, o, "transferSize", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "encodedBodySize", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "decodedBodySize", JS_NewInt64(ctx, 0));
    JS_SetPropertyStr(ctx, o, "serverTiming", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, o, "responseStatus", JS_NewInt32(ctx, 200));
    ns_bind_fn(ctx, o, "toJSON", ns_own_data_props_toJSON, 0);
    return o;
}

static JSValue
ns_perf_build_paint_entry(JSContext *ctx, const char *name, double start)
{
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    JS_SetPropertyStr(ctx, o, "entryType", JS_NewString(ctx, "paint"));
    JS_SetPropertyStr(ctx, o, "startTime", JS_NewFloat64(ctx, start));
    JS_SetPropertyStr(ctx, o, "duration", JS_NewFloat64(ctx, 0));
    ns_bind_fn(ctx, o, "toJSON", ns_own_data_props_toJSON, 0);
    return o;
}

static void ns_perf_observer_queue(ns_js *js, const ns_perf_entry *entry);

static gboolean
ns_perf_timing_allowed(const char *document_url, const char *url,
                       const char *tao)
{
    if (ns_url_same_origin(document_url, url)) return TRUE;
    if (!tao || !document_url) return FALSE;
    char *origin = ns_url_origin_from(document_url);
    gboolean allowed = FALSE;
    gchar **parts = g_strsplit(tao, ",", -1);
    for (int i = 0; parts[i] && !allowed; i++) {
        const char *v = g_strstrip(parts[i]);
        allowed = strcmp(v, "*") == 0 ||
                  (origin && strcmp(origin, "null") != 0 && strcmp(v, origin) == 0);
    }
    g_strfreev(parts);
    g_free(origin);
    return allowed;
}

static int
ns_perf_visible_status(const ns_perf_resource_info *info,
                       const struct ns_response *resp, const char *url,
                       const char *initiator)
{
    long status = resp ? resp->status : info->status;
    if (ns_url_same_origin(info->document_url, url) ||
        g_strcmp0(initiator, "iframe") == 0 ||
        g_strcmp0(initiator, "frame") == 0)
        return (int)status;
    if (!info->cors_mode || !resp || !resp->cors_allow_origin)
        return 0;
    char *doc_origin = ns_url_origin_from(info->document_url);
    const char *acao = resp->cors_allow_origin;
    gboolean cors_ok = strcmp(acao, "*") == 0 ||
        (doc_origin && g_ascii_strcasecmp(acao, doc_origin) == 0);
    g_free(doc_origin);
    return cors_ok ? (int)status : 0;
}

static void
ns_perf_set_phases(ns_perf_entry *e, const struct ns_response *resp,
                   const char *url, gint64 origin, double end)
{
    gboolean network = resp && resp->request_start_us > 0 &&
                       resp->next_hop_protocol;
    double fetch = network
        ? MAX(e->start_time, ns_perf_relative_ms(resp->request_start_us, origin))
        : e->start_time;
#define NS_PHASE(ms) MIN(end, fetch + (network && (ms) > 0 ? (ms) : 0))
    e->fetch_start = fetch;
    e->domain_lookup_start = fetch;
    e->domain_lookup_end = NS_PHASE(resp ? resp->domain_lookup_ms : 0);
    e->connect_start = e->domain_lookup_end;
    e->connect_end = MAX(e->connect_start,
                         NS_PHASE(resp ? MAX(resp->connect_ms, resp->tls_ms) : 0));
    if (g_str_has_prefix(url, "https:"))
        e->secure_connection_start = network && resp->tls_ms > 0
            ? MAX(e->connect_start, NS_PHASE(resp->connect_ms)) : fetch;
    e->request_start = MAX(e->connect_end,
                           NS_PHASE(resp ? resp->pretransfer_ms : 0));
    e->response_start = MAX(e->request_start,
                            network && resp->response_start_ms > 0
                                ? NS_PHASE(resp->response_start_ms) : end);
#undef NS_PHASE
}

static void
ns_perf_set_sizes(ns_perf_entry *e, const ns_perf_resource_info *info,
                  const struct ns_response *resp)
{
    e->encoded_size = resp ? (resp->body ? (gint64)resp->body->len : 0)
                           : info->body_size;
    const char *protocol = resp ? resp->next_hop_protocol
                                : info->next_hop_protocol;
    e->next_hop_protocol = g_strdup(protocol ? protocol : "");
    e->transfer_size = protocol && *protocol ? e->encoded_size + 300 : 0;
}

static gboolean
ns_perf_url_untimed(const char *url)
{
    return g_str_has_prefix(url, "data:") || g_str_has_prefix(url, "blob:") ||
           g_str_has_prefix(url, "about:");
}

static gboolean
ns_perf_resource_timing_allowed(const ns_perf_resource_info *info,
                                const struct ns_response *resp,
                                const char *url)
{
    char *tao = resp ? ns_net_raw_header_values(resp->raw_headers,
                                                "timing-allow-origin")
                     : g_strdup(info->timing_allow_origin);
    gboolean allowed = ns_perf_timing_allowed(info->document_url, url, tao);
    g_free(tao);
    return allowed;
}

static gboolean
ns_perf_resource_skipped(ns_js *js, const ns_perf_resource_info *info,
                         const char *url)
{
    if (ns_perf_url_untimed(url)) return TRUE;
    if (!info || info->preload) return FALSE;
    gconstpointer key = ns_perf_realm_key(js, info->timeline);
    for (guint i = 0; i < js->perf_entries->len; i++) {
        ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
        if (e && e->preload_unclaimed && e->realm == key &&
            g_strcmp0(e->name, url) == 0) {
            e->preload_unclaimed = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

void
ns_perf_add_resource_timed(ns_js *js, const ns_perf_resource_info *info,
                           const char *url, const char *initiator,
                           gint64 start_us, gint64 end_us,
                           const struct ns_response *resp)
{
    static const ns_perf_resource_info no_info = { 0 };
    if (!info) info = &no_info;
    if (!js || !js->perf_entries || !url ||
        ns_perf_resource_skipped(js, info, url))
        return;
    if (js->perf_entries->len >= NS_PERF_ENTRY_CAP)
        g_ptr_array_remove_index(js->perf_entries, 0);
    ns_perf_entry *e = g_new0(ns_perf_entry, 1);
    e->realm = ns_perf_realm_key(js, info->timeline);
    gint64 origin = ns_js_time_origin_us(js, e->realm);
    e->name = g_strdup(url);
    e->type = g_strdup("resource");
    e->initiator_type = g_strdup(initiator ? initiator : "other");
    e->render_blocking = info->render_blocking;
    e->preload_unclaimed = info->preload;
    e->start_time = ns_perf_relative_ms(start_us, origin);
    double end = ns_perf_relative_ms(MAX(end_us, start_us), origin);
    e->duration = end - e->start_time;
    e->response_status = ns_perf_visible_status(info, resp, url, initiator);
    e->has_timing = TRUE;
    e->fetch_start = e->start_time;
    e->response_end = end;
    if (ns_perf_resource_timing_allowed(info, resp, url)) {
        ns_perf_set_sizes(e, info, resp);
        ns_perf_set_phases(e, resp, url, origin, end);
    } else {
        e->next_hop_protocol = g_strdup("");
    }
    g_ptr_array_add(js->perf_entries, e);
    ns_perf_observer_queue(js, e);
}

gboolean
ns_perf_has_resource(ns_js *js, gconstpointer timeline, const char *url,
                     const char *initiator)
{
    if (!js || !js->perf_entries || !url) return FALSE;
    gconstpointer key = ns_perf_realm_key(js, timeline);
    for (guint i = 0; i < js->perf_entries->len; i++) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
        if (e && e->realm == key && g_strcmp0(e->type, "resource") == 0 &&
            g_strcmp0(e->name, url) == 0 &&
            g_strcmp0(e->initiator_type, initiator) == 0)
            return TRUE;
    }
    return FALSE;
}

void
ns_perf_move_timeline(ns_js *js, gconstpointer from, gconstpointer to)
{
    if (!js || !js->perf_entries || !from) return;
    gconstpointer key = ns_perf_realm_key(js, to);
    for (guint i = 0; i < js->perf_entries->len; i++) {
        ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
        if (e && e->realm == from) e->realm = key;
    }
}

static JSClassID ns_perf_observer_class_id;

static JSValue
ns_perf_records_to_array(JSContext *ctx, GPtrArray *records)
{
    JSValue arr = JS_NewArray(ctx);
    if (!records) return arr;
    for (guint i = 0; i < records->len; i++) {
        const ns_perf_entry *e = g_ptr_array_index(records, i);
        if (e) JS_SetPropertyUint32(ctx, arr, i, ns_perf_entry_to_js(ctx, e));
    }
    return arr;
}

static JSValue
ns_perf_entry_list_getEntries(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    JSValue out = JS_NewArray(ctx);
    if (!JS_IsObject(entries)) {
        JS_FreeValue(ctx, entries);
        return out;
    }
    uint32_t len = ns_js_array_length(ctx, entries);
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, entries, i);
        if (!JS_IsException(v)) JS_SetPropertyUint32(ctx, out, i, v);
    }
    JS_FreeValue(ctx, entries);
    return out;
}

static gboolean
ns_perf_js_entry_matches(JSContext *ctx, JSValueConst entry,
                         const char *name, const char *type)
{
    if (name) {
        JSValue v = JS_GetPropertyStr(ctx, entry, "name");
        const char *s = JS_ToCString(ctx, v);
        gboolean ok = s && strcmp(s, name) == 0;
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        if (!ok) return FALSE;
    }
    if (type) {
        JSValue v = JS_GetPropertyStr(ctx, entry, "entryType");
        const char *s = JS_ToCString(ctx, v);
        gboolean ok = s && strcmp(s, type) == 0;
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, v);
        if (!ok) return FALSE;
    }
    return TRUE;
}

static JSValue
ns_perf_entry_list_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *type = argc > 1 && JS_IsString(argv[1])
                         ? JS_ToCString(ctx, argv[1]) : NULL;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    JSValue out = JS_NewArray(ctx);
    if (!name || !JS_IsObject(entries)) {
        if (name) JS_FreeCString(ctx, name);
        if (type) JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, entries);
        return out;
    }
    uint32_t len = ns_js_array_length(ctx, entries);
    uint32_t oi = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, entries, i);
        if (JS_IsException(v)) continue;
        if (ns_perf_js_entry_matches(ctx, v, name, type))
            JS_SetPropertyUint32(ctx, out, oi++, v);
        else
            JS_FreeValue(ctx, v);
    }
    JS_FreeCString(ctx, name);
    if (type) JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, entries);
    return out;
}

static JSValue
ns_perf_entry_list_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    const char *type = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    JSValue entries = JS_GetPropertyStr(ctx, this_val, "_entries");
    JSValue out = JS_NewArray(ctx);
    if (!type || !JS_IsObject(entries)) {
        if (type) JS_FreeCString(ctx, type);
        JS_FreeValue(ctx, entries);
        return out;
    }
    uint32_t len = ns_js_array_length(ctx, entries);
    uint32_t oi = 0;
    for (uint32_t i = 0; i < len; i++) {
        JSValue v = JS_GetPropertyUint32(ctx, entries, i);
        if (JS_IsException(v)) continue;
        if (ns_perf_js_entry_matches(ctx, v, NULL, type))
            JS_SetPropertyUint32(ctx, out, oi++, v);
        else
            JS_FreeValue(ctx, v);
    }
    JS_FreeCString(ctx, type);
    JS_FreeValue(ctx, entries);
    return out;
}

static JSValue
ns_perf_entry_list_from_array(JSContext *ctx, JSValueConst entries)
{
    JSValue list = JS_NewObject(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global,
                                     "PerformanceObserverEntryList");
    JSValue proto = JS_IsObject(ctor)
        ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(proto)) JS_SetPrototype(ctx, list, proto);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    JS_SetPropertyStr(ctx, list, "_entries", JS_DupValue(ctx, entries));
    ns_bind_fn(ctx, list, "getEntries",       ns_perf_entry_list_getEntries,       0);
    ns_bind_fn(ctx, list, "getEntriesByName", ns_perf_entry_list_getEntriesByName, 2);
    ns_bind_fn(ctx, list, "getEntriesByType", ns_perf_entry_list_getEntriesByType, 1);
    return list;
}

void
ns_perf_install_entry_list(JSContext *ctx, JSValueConst global)
{
    JSValue ctor = JS_GetPropertyStr(ctx, global,
                                     "PerformanceObserverEntryList");
    JSValue proto = JS_IsObject(ctor)
        ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(proto)) {
        ns_bind_fn_if_not_callable(ctx, proto, "getEntries",
                                   ns_perf_entry_list_getEntries, 0);
        ns_bind_fn_if_not_callable(ctx, proto, "getEntriesByName",
                                   ns_perf_entry_list_getEntriesByName, 2);
        ns_bind_fn_if_not_callable(ctx, proto, "getEntriesByType",
                                   ns_perf_entry_list_getEntriesByType, 1);
    }
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, ctor);
}

static void
ns_perf_observer_free(ns_js *js, ns_perf_observer *o)
{
    if (!o) return;
    if (js && js->ctx) JS_FreeValue(js->ctx, o->cb);
    if (o->entry_types) g_ptr_array_free(o->entry_types, TRUE);
    if (o->records) g_ptr_array_free(o->records, TRUE);
    g_free(o);
}

static void
ns_perf_observer_finalizer(JSRuntime *rt, JSValue val)
{
    ns_perf_observer *o = JS_GetOpaque(val, ns_perf_observer_class_id);
    if (!o) return;
    ns_js *js = JS_GetRuntimeOpaque(rt);
    if (js && js->perf_observers)
        g_ptr_array_remove_fast(js->perf_observers, o);
    ns_perf_observer_free(js, o);
}

static JSClassDef ns_perf_observer_class = {
    "PerformanceObserver",
    .finalizer = ns_perf_observer_finalizer,
};

static ns_perf_observer *
ns_unwrap_perf_observer(JSValueConst v)
{
    return JS_GetOpaque(v, ns_perf_observer_class_id);
}

static gboolean
ns_perf_observer_wants(const ns_perf_observer *o, const char *type)
{
    if (!o || !o->entry_types || !type) return FALSE;
    for (guint i = 0; i < o->entry_types->len; i++) {
        const char *want = g_ptr_array_index(o->entry_types, i);
        if (want && strcmp(want, type) == 0) return TRUE;
    }
    return FALSE;
}

static void
ns_perf_observer_add_type(ns_perf_observer *o, const char *type)
{
    if (!o || !o->entry_types || !type || !*type) return;
    if (ns_perf_observer_wants(o, type)) return;
    g_ptr_array_add(o->entry_types, g_strdup(type));
}

static void
ns_perf_schedule_drain(ns_js *js);

static gboolean
ns_perf_observer_takes(ns_js *js, const ns_perf_observer *o,
                       const ns_perf_entry *entry)
{
    return o && !o->disconnected && JS_IsFunction(js->ctx, o->cb) &&
           o->realm == entry->realm && ns_perf_observer_wants(o, entry->type);
}

static void
ns_perf_observer_queue(ns_js *js, const ns_perf_entry *entry)
{
    if (!js || !js->ctx || !js->perf_observers || !entry || !entry->type) return;
    gboolean queued = FALSE;
    for (guint i = 0; i < js->perf_observers->len; i++) {
        ns_perf_observer *o = g_ptr_array_index(js->perf_observers, i);
        if (!ns_perf_observer_takes(js, o, entry)) continue;
        if (!o->records)
            o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
        if (o->records->len >= NS_PERF_ENTRY_CAP)
            g_ptr_array_remove_index(o->records, 0);
        g_ptr_array_add(o->records, ns_perf_entry_clone(entry));
        if (!o->pinned) {
            JS_DupValue(js->ctx, o->wrapper);
            o->pinned = TRUE;
        }
        queued = TRUE;
    }
    if (queued) ns_perf_schedule_drain(js);
}

static JSValue
ns_perf_drain_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    if (!js || !js->perf_observers) return JS_UNDEFINED;
    js->perf_drain_scheduled = FALSE;
    for (guint i = 0; i < js->perf_observers->len; i++) {
        ns_perf_observer *o = g_ptr_array_index(js->perf_observers, i);
        if (!o || o->disconnected || !o->records || o->records->len == 0)
            continue;
        if (!JS_IsFunction(ctx, o->cb)) continue;
        GPtrArray *records = o->records;
        o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
        JSValue wrapper = JS_DupValue(ctx, o->wrapper);
        JSValue arr = ns_perf_records_to_array(ctx, records);
        JSValue list = ns_perf_entry_list_from_array(ctx, arr);
        JSValueConst call_args[2] = { list, wrapper };
        JSValue ret = JS_Call(ctx, o->cb, wrapper, 2, call_args);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            if (js->log_cb) {
                const char *msg = JS_ToCString(ctx, ex);
                if (msg) {
                    char *line = g_strdup_printf("JS error in PerformanceObserver: %s", msg);
                    js->log_cb(line, js->log_user_data);
                    g_free(line);
                    JS_FreeCString(ctx, msg);
                }
            }
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, wrapper);
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, arr);
        g_ptr_array_free(records, TRUE);
    }
    return JS_UNDEFINED;
}

static void
ns_perf_schedule_drain(ns_js *js)
{
    if (!js || !js->ctx || js->perf_drain_scheduled) return;
    js->perf_drain_scheduled = TRUE;
    JS_EnqueueJob(js->ctx, ns_perf_drain_job, 0, NULL);
}

static void
ns_perf_observer_collect_types(JSContext *ctx, ns_perf_observer *o,
                               JSValueConst options)
{
    JSValue type = JS_GetPropertyStr(ctx, options, "type");
    if (JS_IsString(type)) {
        const char *s = JS_ToCString(ctx, type);
        if (s) {
            ns_perf_observer_add_type(o, s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, type);

    JSValue entry_types = JS_GetPropertyStr(ctx, options, "entryTypes");
    if (JS_IsObject(entry_types)) {
        uint32_t len = ns_js_array_length(ctx, entry_types);
        for (uint32_t i = 0; i < len; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, entry_types, i);
            const char *s = JS_ToCString(ctx, v);
            if (s) {
                ns_perf_observer_add_type(o, s);
                JS_FreeCString(ctx, s);
            }
            JS_FreeValue(ctx, v);
        }
    }
    JS_FreeValue(ctx, entry_types);
}

JSValue
ns_perf_observer_observe(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv)
{
    ns_perf_observer *o = ns_unwrap_perf_observer(this_val);
    ns_js *js = js_from_ctx(ctx);
    if (!o || argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "PerformanceObserver.observe: options required");
    if (!o->entry_types)
        o->entry_types = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_set_size(o->entry_types, 0);
    ns_perf_observer_collect_types(ctx, o, argv[0]);
    if (o->entry_types->len == 0)
        return JS_ThrowTypeError(ctx, "PerformanceObserver.observe: type or entryTypes required");
    o->disconnected = FALSE;
    if (!o->pinned) {
        JS_DupValue(ctx, o->wrapper);
        o->pinned = TRUE;
    }
    if (js && js->perf_entries && ns_js_get_bool_prop(ctx, argv[0], "buffered", NULL)) {
        for (guint i = 0; i < js->perf_entries->len; i++) {
            const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
            if (!e || e->realm != o->realm ||
                !ns_perf_observer_wants(o, e->type)) continue;
            if (!o->records)
                o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
            if (o->records->len >= NS_PERF_ENTRY_CAP)
                g_ptr_array_remove_index(o->records, 0);
            g_ptr_array_add(o->records, ns_perf_entry_clone(e));
        }
        if (o->records && o->records->len > 0)
            ns_perf_schedule_drain(js);
    }
    return JS_UNDEFINED;
}

void
ns_perf_observer_release(JSContext *ctx, ns_perf_observer *o)
{
    o->disconnected = TRUE;
    if (o->records) g_ptr_array_set_size(o->records, 0);
    if (o->entry_types) g_ptr_array_set_size(o->entry_types, 0);
    if (o->pinned) {
        o->pinned = FALSE;
        JS_FreeValue(ctx, o->wrapper);
    }
}

JSValue
ns_perf_observer_disconnect(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_perf_observer *o = ns_unwrap_perf_observer(this_val);
    if (o) ns_perf_observer_release(ctx, o);
    return JS_UNDEFINED;
}

JSValue
ns_perf_observer_takeRecords(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_perf_observer *o = ns_unwrap_perf_observer(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!o || !o->records) return arr;
    GPtrArray *records = o->records;
    o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
    JS_FreeValue(ctx, arr);
    arr = ns_perf_records_to_array(ctx, records);
    g_ptr_array_free(records, TRUE);
    return arr;
}

JSValue
ns_perf_observer_ctor(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv)
{
    if (!JS_IsObject(this_val))
        return JS_ThrowTypeError(ctx, "Failed to construct 'PerformanceObserver': "
            "Please use the 'new' operator, this DOM object constructor "
            "cannot be called as a function.");
    ns_js *js = js_from_ctx(ctx);
    ns_new_class_id(&ns_perf_observer_class_id);
    JS_NewClass(JS_GetRuntime(ctx), ns_perf_observer_class_id, &ns_perf_observer_class);
    JSValue proto = JS_GetPropertyStr(ctx, this_val, "prototype");
    JSValue obj;
    if (JS_IsObject(proto)) {
        obj = JS_NewObjectProtoClass(ctx, proto, ns_perf_observer_class_id);
        JSAtom obs_atom = JS_NewAtom(ctx, "observe");
        if (JS_HasProperty(ctx, proto, obs_atom) <= 0) {
            ns_bind_fn(ctx, proto, "observe",     ns_perf_observer_observe,     1);
            ns_bind_fn(ctx, proto, "disconnect",  ns_perf_observer_disconnect,  0);
            ns_bind_fn(ctx, proto, "takeRecords", ns_perf_observer_takeRecords, 0);
        }
        JS_FreeAtom(ctx, obs_atom);
    } else {
        obj = JS_NewObjectClass(ctx, ns_perf_observer_class_id);
        ns_bind_fn(ctx, obj, "observe",     ns_perf_observer_observe,     1);
        ns_bind_fn(ctx, obj, "disconnect",  ns_perf_observer_disconnect,  0);
        ns_bind_fn(ctx, obj, "takeRecords", ns_perf_observer_takeRecords, 0);
    }
    JS_FreeValue(ctx, proto);
    ns_perf_observer *o = g_new0(ns_perf_observer, 1);
    JSContext *cb_realm = argc >= 1 && JS_IsFunction(ctx, argv[0])
        ? JS_GetFunctionRealm(ctx, argv[0]) : NULL;
    if (!cb_realm && JS_HasException(ctx))
        JS_FreeValue(ctx, JS_GetException(ctx));
    o->realm = ns_perf_realm_key(js, cb_realm);
    o->entry_types = g_ptr_array_new_with_free_func(g_free);
    o->records = g_ptr_array_new_with_free_func(ns_perf_entry_free);
    o->cb = (argc >= 1 && JS_IsFunction(ctx, argv[0]))
        ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    o->wrapper = obj;
    JS_SetOpaque(obj, o);
    ns_bind_fn_if_not_callable(ctx, obj, "observe",
                               ns_perf_observer_observe, 1);
    ns_bind_fn_if_not_callable(ctx, obj, "disconnect",
                               ns_perf_observer_disconnect, 0);
    ns_bind_fn_if_not_callable(ctx, obj, "takeRecords",
                               ns_perf_observer_takeRecords, 0);
    if (js) {
        if (!js->perf_observers)
            js->perf_observers = g_ptr_array_new();
        g_ptr_array_add(js->perf_observers, o);
    }
    return obj;
}

JSValue
ns_perf_supported_entry_types(JSContext *ctx)
{
    static const char *types[] = {
        "long-animation-frame", "longtask", "mark", "measure", "navigation",
        "paint", "resource",
    };
    JSValue arr = JS_NewArray(ctx);
    for (guint i = 0; i < G_N_ELEMENTS(types); i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, types[i]));
    return arr;
}

static void
ns_perf_push(ns_js *js, gconstpointer realm, const char *type,
             const char *name, double start_time, double duration)
{
    if (!js || !js->perf_entries) return;
    if (js->perf_entries->len >= NS_PERF_ENTRY_CAP)
        g_ptr_array_remove_index(js->perf_entries, 0);
    ns_perf_entry *e = g_new0(ns_perf_entry, 1);
    e->realm      = realm;
    e->name       = g_strdup(name ? name : "");
    e->type       = g_strdup(type);
    e->start_time = start_time;
    e->duration   = duration;
    g_ptr_array_add(js->perf_entries, e);
    ns_perf_observer_queue(js, e);
}

#define NS_PERF_LONG_TASK_US (50 * 1000)

void
ns_perf_note_task(ns_js *js, gint64 start_us, gint64 end_us)
{
    if (!js || js->worker_host || !js->perf_entries || start_us <= 0 ||
        end_us - start_us < NS_PERF_LONG_TASK_US)
        return;
    double start = ns_perf_relative_ms(start_us, ns_js_time_origin_us(js, NULL));
    double duration = (double)(end_us - start_us) / 1000.0;
    ns_perf_push(js, NULL, "longtask", "self", start, duration);
    ns_perf_push(js, NULL, "long-animation-frame", "long-animation-frame",
                 start, duration);
}

JSValue
ns_window_performance_mark(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    gconstpointer realm = ns_perf_this_realm(js, this_val);
    double t = ns_perf_relative_ms(g_get_monotonic_time(),
                                   ns_js_time_origin_us(js, realm));
    ns_perf_push(js, realm, "mark", name, t, 0.0);
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "name",
                      JS_NewString(ctx, name ? name : ""));
    JS_SetPropertyStr(ctx, r, "entryType", JS_NewString(ctx, "mark"));
    JS_SetPropertyStr(ctx, r, "startTime", JS_NewFloat64(ctx, t));
    JS_SetPropertyStr(ctx, r, "duration",  JS_NewFloat64(ctx, 0.0));
    if (name) JS_FreeCString(ctx, name);
    return r;
}

static gboolean
ns_perf_lookup_mark(const ns_js *js, gconstpointer realm, const char *name,
                    double *out_time)
{
    if (!js || !js->perf_entries || !name) return FALSE;
    for (guint i = js->perf_entries->len; i > 0; i--) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i - 1);
        if (e && e->realm == realm && e->type && !strcmp(e->type, "mark") &&
            e->name && !strcmp(e->name, name)) {
            if (out_time) *out_time = e->start_time;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
ns_perf_resolve_time(JSContext *ctx, JSValueConst v, const ns_js *js,
                     gconstpointer realm, double fallback, double *out)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        *out = fallback;
        return TRUE;
    }
    if (JS_IsNumber(v)) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) < 0) return FALSE;
        *out = d;
        return TRUE;
    }
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (!s) return FALSE;
        gboolean ok = ns_perf_lookup_mark(js, realm, s, out);
        JS_FreeCString(ctx, s);
        if (!ok) *out = fallback;
        return TRUE;
    }
    *out = fallback;
    return TRUE;
}

JSValue
ns_window_performance_measure(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    gconstpointer realm = ns_perf_this_realm(js, this_val);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    double end_time = ns_perf_relative_ms(g_get_monotonic_time(),
                                          ns_js_time_origin_us(js, realm));
    double start_time = 0.0;
    double resolved_end = end_time;
    JSValue start_v = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue end_v   = argc > 2 ? argv[2] : JS_UNDEFINED;
    ns_perf_resolve_time(ctx, start_v, js, realm, 0.0, &start_time);
    ns_perf_resolve_time(ctx, end_v,   js, realm, end_time, &resolved_end);
    double duration = resolved_end - start_time;
    if (duration < 0) duration = 0;
    ns_perf_push(js, realm, "measure", name, start_time, duration);
    JSValue r = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, r, "name",
                      JS_NewString(ctx, name ? name : ""));
    JS_SetPropertyStr(ctx, r, "entryType", JS_NewString(ctx, "measure"));
    JS_SetPropertyStr(ctx, r, "startTime", JS_NewFloat64(ctx, start_time));
    JS_SetPropertyStr(ctx, r, "duration",  JS_NewFloat64(ctx, duration));
    if (name) JS_FreeCString(ctx, name);
    return r;
}

static void
ns_perf_clear(ns_js *js, gconstpointer realm, const char *type,
              const char *name)
{
    if (!js || !js->perf_entries) return;
    for (guint i = js->perf_entries->len; i > 0; i--) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i - 1);
        if (!e || e->realm != realm) continue;
        if (type && (!e->type || strcmp(e->type, type))) continue;
        if (name && (!e->name || strcmp(e->name, name))) continue;
        g_ptr_array_remove_index(js->perf_entries, i - 1);
    }
}

JSValue
ns_window_performance_clearMarks(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 && JS_IsString(argv[0])
                         ? JS_ToCString(ctx, argv[0]) : NULL;
    ns_perf_clear(js, ns_perf_this_realm(js, this_val), "mark", name);
    if (name) JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

JSValue
ns_window_performance_clearMeasures(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    const char *name = argc > 0 && JS_IsString(argv[0])
                         ? JS_ToCString(ctx, argv[0]) : NULL;
    ns_perf_clear(js, ns_perf_this_realm(js, this_val), "measure", name);
    if (name) JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

JSValue
ns_window_performance_clearResourceTimings(JSContext *ctx,
                                           JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    ns_perf_clear(js, ns_perf_this_realm(js, this_val), "resource", NULL);
    return JS_UNDEFINED;
}

JSValue
ns_window_performance_getEntries(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!js) return arr;
    gconstpointer realm = ns_perf_this_realm(js, this_val);
    uint32_t out = 0;
    JS_SetPropertyUint32(ctx, arr, out++,
                         ns_perf_build_navigation_entry(ctx, js));
    JS_SetPropertyUint32(ctx, arr, out++,
                         ns_perf_build_paint_entry(ctx, "first-paint", 60));
    JS_SetPropertyUint32(ctx, arr, out++,
        ns_perf_build_paint_entry(ctx, "first-contentful-paint", 65));
    if (js->perf_entries)
        for (guint i = 0; i < js->perf_entries->len; i++) {
            const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
            if (e && e->realm == realm)
                JS_SetPropertyUint32(ctx, arr, out++,
                                     ns_perf_entry_to_js(ctx, e));
        }
    return arr;
}

JSValue
ns_window_performance_getEntriesByName(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!js || !js->perf_entries || argc < 1) return arr;
    gconstpointer realm = ns_perf_this_realm(js, this_val);
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *type = argc > 1 && JS_IsString(argv[1])
                         ? JS_ToCString(ctx, argv[1]) : NULL;
    uint32_t out = 0;
    for (guint i = 0; i < js->perf_entries->len; i++) {
        const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
        if (!e || e->realm != realm) continue;
        if (name && (!e->name || strcmp(e->name, name))) continue;
        if (type && (!e->type || strcmp(e->type, type))) continue;
        JS_SetPropertyUint32(ctx, arr, out++, ns_perf_entry_to_js(ctx, e));
    }
    if (name) JS_FreeCString(ctx, name);
    if (type) JS_FreeCString(ctx, type);
    return arr;
}

JSValue
ns_window_performance_getEntriesByType(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    ns_js *js = js_from_ctx(ctx);
    JSValue arr = JS_NewArray(ctx);
    if (!js || argc < 1) return arr;
    gconstpointer realm = ns_perf_this_realm(js, this_val);
    const char *type = JS_ToCString(ctx, argv[0]);
    uint32_t out = 0;
    if (type && strcmp(type, "navigation") == 0) {
        JS_SetPropertyUint32(ctx, arr, out++,
                             ns_perf_build_navigation_entry(ctx, js));
    } else if (type && strcmp(type, "paint") == 0) {
        JS_SetPropertyUint32(ctx, arr, out++,
                             ns_perf_build_paint_entry(ctx, "first-paint", 60));
        JS_SetPropertyUint32(ctx, arr, out++,
            ns_perf_build_paint_entry(ctx, "first-contentful-paint", 65));
    }
    if (js->perf_entries)
        for (guint i = 0; i < js->perf_entries->len; i++) {
            const ns_perf_entry *e = g_ptr_array_index(js->perf_entries, i);
            if (!e || e->realm != realm) continue;
            if (type && (!e->type || strcmp(e->type, type))) continue;
            JS_SetPropertyUint32(ctx, arr, out++, ns_perf_entry_to_js(ctx, e));
        }
    if (type) JS_FreeCString(ctx, type);
    return arr;
}

JSValue
ns_window_performance_memory_get(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    ns_js *js = js_from_ctx(ctx);
    JSValue mem = JS_NewObject(ctx);
    int64_t used = 0, total = 0, limit = 128LL * 1024 * 1024;
    if (js && js->rt) {
        JSMemoryUsage u;
        memset(&u, 0, sizeof(u));
        JS_ComputeMemoryUsage(js->rt, &u);
        used = u.memory_used_size > 0 ? u.memory_used_size : 0;
        total = u.malloc_size > 0 ? u.malloc_size : used;
        if (u.malloc_limit > 0) limit = u.malloc_limit;
        if (total < used) total = used;
    }
    JS_SetPropertyStr(ctx, mem, "jsHeapSizeLimit", JS_NewFloat64(ctx, (double)limit));
    JS_SetPropertyStr(ctx, mem, "totalJSHeapSize", JS_NewFloat64(ctx, (double)total));
    JS_SetPropertyStr(ctx, mem, "usedJSHeapSize",  JS_NewFloat64(ctx, (double)used));
    return mem;
}
