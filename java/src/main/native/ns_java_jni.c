/* Nordstjernen — JNI bridge from the Java API to the C embedding API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libnordstjernen.h"

#define NS_JNI_REPLACEMENT 0xFFFDu

static ns_browser *
as_browser(jlong handle)
{
    return (ns_browser *)(intptr_t)handle;
}

static char *
utf8_from_jstring(JNIEnv *env, jstring s)
{
    if (!s) return NULL;
    jsize n = (*env)->GetStringLength(env, s);
    const jchar *u = (*env)->GetStringChars(env, s, NULL);
    if (!u) return NULL;
    char *out = malloc((size_t)n * 3 + 1);
    size_t o = 0;
    for (jsize i = 0; out && i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n &&
            u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t)(u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = NS_JNI_REPLACEMENT;
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (out) out[o] = '\0';
    (*env)->ReleaseStringChars(env, s, u);
    return out;
}

static uint32_t
utf8_decode(const unsigned char *s, size_t left, size_t *used)
{
    unsigned char lead = s[0];
    uint32_t c;
    size_t more;
    *used = 1;
    if (lead < 0x80) return lead;
    if (lead >= 0xC2 && lead <= 0xDF) {
        c = lead & 0x1Fu;
        more = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        c = lead & 0x0Fu;
        more = 2;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        c = lead & 0x07u;
        more = 3;
    } else {
        return NS_JNI_REPLACEMENT;
    }
    if (more >= left) return NS_JNI_REPLACEMENT;
    for (size_t k = 1; k <= more; k++) {
        if ((s[k] & 0xC0) != 0x80) return NS_JNI_REPLACEMENT;
        c = (c << 6) | (s[k] & 0x3Fu);
    }
    if ((more == 2 && (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF))) ||
        (more == 3 && (c < 0x10000 || c > 0x10FFFF)))
        return NS_JNI_REPLACEMENT;
    *used = more + 1;
    return c;
}

static jstring
jstring_from_utf8(JNIEnv *env, const char *text)
{
    const unsigned char *s = (const unsigned char *)text;
    size_t len = strlen(text);
    if (len > (size_t)INT32_MAX) return NULL;
    jchar *u = malloc((len + 1) * sizeof(jchar));
    if (!u) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        size_t used;
        uint32_t c = utf8_decode(s + i, len - i, &used);
        i += used;
        if (c >= 0x10000) {
            c -= 0x10000;
            u[o++] = (jchar)(0xD800 + (c >> 10));
            u[o++] = (jchar)(0xDC00 + (c & 0x3FF));
        } else {
            u[o++] = (jchar)c;
        }
    }
    jstring out = (*env)->NewString(env, u, (jsize)o);
    free(u);
    return out;
}

static jstring
take_string(JNIEnv *env, char *owned)
{
    if (!owned) return NULL;
    jstring out = jstring_from_utf8(env, owned);
    free(owned);
    return out;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_Nordstjernen_nativeInit(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return ns_browser_init();
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_Nordstjernen_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    ns_browser_shutdown();
}

JNIEXPORT jlong JNICALL
Java_org_nordstjernen_Nordstjernen_nativeOpen(JNIEnv *env, jclass clazz,
                                              jstring url, jint viewport_width,
                                              jint settle_ms)
{
    (void)clazz;
    char *u = utf8_from_jstring(env, url);
    ns_browser *b = u ? ns_browser_open(u, viewport_width, settle_ms) : NULL;
    free(u);
    return (jlong)(intptr_t)b;
}

JNIEXPORT jintArray JNICALL
Java_org_nordstjernen_Nordstjernen_nativePageSize(JNIEnv *env, jclass clazz,
                                                  jlong handle)
{
    (void)clazz;
    int w = 0, h = 0;
    if (ns_browser_page_size(as_browser(handle), &w, &h) != 0) return NULL;
    jintArray arr = (*env)->NewIntArray(env, 2);
    if (!arr) return NULL;
    jint vals[2] = { w, h };
    (*env)->SetIntArrayRegion(env, arr, 0, 2, vals);
    return arr;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeRenderText(JNIEnv *env, jclass clazz,
                                                    jlong handle)
{
    (void)clazz;
    return take_string(env, ns_browser_render_text(as_browser(handle)));
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeTitle(JNIEnv *env, jclass clazz,
                                               jlong handle)
{
    (void)clazz;
    return take_string(env, ns_browser_title(as_browser(handle)));
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeLinkAt(JNIEnv *env, jclass clazz,
                                                jlong handle, jint x, jint y)
{
    (void)clazz;
    return take_string(env, ns_browser_link_at(as_browser(handle), x, y));
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeUrl(JNIEnv *env, jclass clazz,
                                             jlong handle)
{
    (void)clazz;
    return take_string(env, ns_browser_url(as_browser(handle)));
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeLinks(JNIEnv *env, jclass clazz,
                                               jlong handle)
{
    (void)clazz;
    return take_string(env, ns_browser_links(as_browser(handle)));
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_Nordstjernen_nativeRenderArgb(JNIEnv *env, jclass clazz,
                                                    jlong handle, jint scroll_x,
                                                    jint scroll_y, jint width,
                                                    jint height, jdouble scale,
                                                    jobject pixels)
{
    (void)clazz;
    if (width <= 0 || height <= 0 || !pixels) return -1;
    unsigned char *buf = (*env)->GetDirectBufferAddress(env, pixels);
    jlong capacity = (*env)->GetDirectBufferCapacity(env, pixels);
    jlong need = (jlong)width * 4 * (jlong)height;
    if (!buf || capacity < need) return -1;
    return ns_browser_render_argb32(as_browser(handle), scroll_x, scroll_y,
                                    width, height, scale, buf, width * 4);
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_Nordstjernen_nativeRenderImage(JNIEnv *env, jclass clazz,
                                                     jlong handle, jstring path)
{
    (void)clazz;
    char *p = utf8_from_jstring(env, path);
    int rc = p ? ns_browser_render_image(as_browser(handle), p) : -1;
    free(p);
    return rc;
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_Nordstjernen_nativeClose(JNIEnv *env, jclass clazz,
                                               jlong handle)
{
    (void)env; (void)clazz;
    ns_browser_close(as_browser(handle));
}
