/* Nordstjernen — toolkit-independent offscreen GLES context for WebGL.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "glctx.h"

#if defined(NS_ENABLE_WEBGL) && defined(G_OS_WIN32)

#include <windows.h>
#include <epoxy/gl.h>
#include <epoxy/wgl.h>

struct ns_gl_context {
    HWND  window;
    HDC   dc;
    HGLRC rc;
};

static const wchar_t *
ns_gl_window_class(void)
{
    static gsize once = 0;
    static const wchar_t *name = L"NordstjernenGL";
    if (g_once_init_enter(&once)) {
        WNDCLASSW wc = { 0 };
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = name;
        RegisterClassW(&wc);
        g_once_init_leave(&once, 1);
    }
    return name;
}

static gboolean
ns_gl_set_pixel_format(HDC dc)
{
    PIXELFORMATDESCRIPTOR pfd = { 0 };
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int fmt = ChoosePixelFormat(dc, &pfd);
    return fmt != 0 && SetPixelFormat(dc, fmt, &pfd);
}

ns_gl_context *
ns_gl_context_create(void)
{
    HWND window = CreateWindowExW(0, ns_gl_window_class(), L"", WS_POPUP,
                                  0, 0, 1, 1, NULL, NULL,
                                  GetModuleHandleW(NULL), NULL);
    if (!window) return NULL;

    HDC dc = GetDC(window);
    if (!dc || !ns_gl_set_pixel_format(dc)) {
        if (dc) ReleaseDC(window, dc);
        DestroyWindow(window);
        return NULL;
    }

    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy)) {
        if (legacy) wglDeleteContext(legacy);
        ReleaseDC(window, dc);
        DestroyWindow(window);
        return NULL;
    }

#ifndef WGL_CONTEXT_FLAGS_ARB
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#endif
#ifndef WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB
#define WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB 0x00000004
#endif
#ifndef WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB
#define WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB 0x8256
#endif
#ifndef WGL_LOSE_CONTEXT_ON_RESET_ARB
#define WGL_LOSE_CONTEXT_ON_RESET_ARB 0x8252
#endif
    HGLRC rc = NULL;
    if (epoxy_has_wgl_extension(dc, "WGL_ARB_create_context_robustness")) {
        const int attrs[] = {
            WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB,
            WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB,
                WGL_LOSE_CONTEXT_ON_RESET_ARB,
            0,
        };
        rc = wglCreateContextAttribsARB(dc, NULL, attrs);
    } else {
        g_message("WebGL: the OpenGL driver lacks WGL_ARB_create_context_robustness, "
                  "so no WebGL context is created");
    }
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        if (rc) wglDeleteContext(rc);
        ReleaseDC(window, dc);
        DestroyWindow(window);
        return NULL;
    }

    ns_gl_context *c = g_new0(ns_gl_context, 1);
    c->window = window;
    c->dc = dc;
    c->rc = rc;
    return c;
}

gboolean
ns_gl_context_make_current(ns_gl_context *c)
{
    if (!c) return FALSE;
    if (wglGetCurrentContext() == c->rc && wglGetCurrentDC() == c->dc)
        return TRUE;
    return wglMakeCurrent(c->dc, c->rc) ? TRUE : FALSE;
}

void
ns_gl_context_release(ns_gl_context *c)
{
    if (!c) return;
    if (wglGetCurrentContext() == c->rc)
        wglMakeCurrent(NULL, NULL);
}

void
ns_gl_context_destroy(ns_gl_context *c)
{
    if (!c) return;
    if (wglGetCurrentContext() == c->rc)
        wglMakeCurrent(NULL, NULL);
    wglDeleteContext(c->rc);
    ReleaseDC(c->window, c->dc);
    DestroyWindow(c->window);
    g_free(c);
}

#elif defined(NS_ENABLE_WEBGL) && defined(NS_HAVE_CGL)

#include <OpenGL/OpenGL.h>
#include <epoxy/gl.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct ns_gl_context {
    CGLContextObj ctx;
    GLuint        default_vao;
};

ns_gl_context *
ns_gl_context_create(void)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,
        kCGLPFAColorSize,   (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize,   (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize,   (CGLPixelFormatAttribute)24,
        kCGLPFAStencilSize, (CGLPixelFormatAttribute)8,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    if (CGLChoosePixelFormat(attrs, &pix, &npix) != kCGLNoError || !pix)
        return NULL;
    CGLContextObj ctx = NULL;
    CGLError err = CGLCreateContext(pix, NULL, &ctx);
    CGLDestroyPixelFormat(pix);
    if (err != kCGLNoError || !ctx)
        return NULL;
    if (CGLSetCurrentContext(ctx) != kCGLNoError) {
        CGLDestroyContext(ctx);
        return NULL;
    }

    ns_gl_context *c = g_new0(ns_gl_context, 1);
    c->ctx = ctx;
    glGenVertexArrays(1, &c->default_vao);
    glBindVertexArray(c->default_vao);
    return c;
}

gboolean
ns_gl_context_make_current(ns_gl_context *c)
{
    if (!c) return FALSE;
    if (CGLGetCurrentContext() == c->ctx) return TRUE;
    return CGLSetCurrentContext(c->ctx) == kCGLNoError ? TRUE : FALSE;
}

void
ns_gl_context_release(ns_gl_context *c)
{
    if (!c) return;
    if (CGLGetCurrentContext() == c->ctx)
        CGLSetCurrentContext(NULL);
}

void
ns_gl_context_destroy(ns_gl_context *c)
{
    if (!c) return;
    if (CGLGetCurrentContext() == c->ctx) {
        if (c->default_vao) glDeleteVertexArrays(1, &c->default_vao);
        CGLSetCurrentContext(NULL);
    }
    CGLDestroyContext(c->ctx);
    g_free(c);
}

#pragma clang diagnostic pop

#elif defined(NS_ENABLE_WEBGL) && defined(NS_HAVE_EGL)

#define NS_GLCTX_BACKEND_EGL 1

#include <string.h>

#include <epoxy/egl.h>

struct ns_gl_context {
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
};

static EGLDisplay
ns_gl_shared_display(void)
{
    static gsize once = 0;
    static EGLDisplay shared = EGL_NO_DISPLAY;
    if (g_once_init_enter(&once)) {
        EGLDisplay d = EGL_NO_DISPLAY;
        const char *exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (exts && strstr(exts, "EGL_MESA_platform_surfaceless")) {
            PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
                (PFNEGLGETPLATFORMDISPLAYEXTPROC)
                    eglGetProcAddress("eglGetPlatformDisplayEXT");
            if (get_platform_display)
                d = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                         EGL_DEFAULT_DISPLAY, NULL);
        }
        if (d == EGL_NO_DISPLAY)
            d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (d != EGL_NO_DISPLAY) {
            EGLint major = 0, minor = 0;
            if (eglInitialize(d, &major, &minor))
                shared = d;
        }
        g_once_init_leave(&once, 1);
    }
    return shared;
}

static gboolean
ns_gl_choose_config(EGLDisplay d, EGLConfig *out)
{
    EGLint n = 0;
    const EGLint pbuffer_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    if (eglChooseConfig(d, pbuffer_attrs, out, 1, &n) && n >= 1)
        return TRUE;

    const EGLint any_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    return eglChooseConfig(d, any_attrs, out, 1, &n) && n >= 1;
}

ns_gl_context *
ns_gl_context_create(void)
{
    EGLDisplay d = ns_gl_shared_display();
    if (d == EGL_NO_DISPLAY) return NULL;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return NULL;

    EGLConfig config;
    if (!ns_gl_choose_config(d, &config)) return NULL;

#ifndef EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT
#define EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT 0x30BF
#endif
#ifndef EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT
#define EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT 0x3138
#endif
#ifndef EGL_LOSE_CONTEXT_ON_RESET_EXT
#define EGL_LOSE_CONTEXT_ON_RESET_EXT 0x31BF
#endif
    const char *egl_exts = eglQueryString(d, EGL_EXTENSIONS);
    if (!egl_exts || !strstr(egl_exts, "EGL_EXT_create_context_robustness")) {
        g_message("WebGL: the EGL driver lacks EGL_EXT_create_context_robustness, "
                  "so no WebGL context is created");
        return NULL;
    }

    EGLContext context = EGL_NO_CONTEXT;
    for (EGLint major = 3; major >= 2 && context == EGL_NO_CONTEXT; major--) {
        EGLint ra[] = {
            EGL_CONTEXT_MAJOR_VERSION, major,
            EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT, EGL_TRUE,
            EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT,
                EGL_LOSE_CONTEXT_ON_RESET_EXT,
            EGL_NONE,
        };
        context = eglCreateContext(d, config, EGL_NO_CONTEXT, ra);
    }
    if (context == EGL_NO_CONTEXT) {
        g_message("WebGL: the EGL driver refused a robust-access context, "
                  "so no WebGL context is created");
        return NULL;
    }

    EGLSurface surface = EGL_NO_SURFACE;
    if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        const EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        surface = eglCreatePbufferSurface(d, config, pb_attrs);
        if (surface == EGL_NO_SURFACE ||
            !eglMakeCurrent(d, surface, surface, context)) {
            if (surface != EGL_NO_SURFACE) eglDestroySurface(d, surface);
            eglDestroyContext(d, context);
            return NULL;
        }
    }

    ns_gl_context *c = g_new0(ns_gl_context, 1);
    c->display = d;
    c->context = context;
    c->surface = surface;
    return c;
}

gboolean
ns_gl_context_make_current(ns_gl_context *c)
{
    if (!c) return FALSE;
    if (eglGetCurrentContext() == c->context)
        return TRUE;
    return eglMakeCurrent(c->display, c->surface, c->surface, c->context)
               ? TRUE : FALSE;
}

void
ns_gl_context_release(ns_gl_context *c)
{
    if (!c) return;
    if (eglGetCurrentContext() == c->context)
        eglMakeCurrent(c->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void
ns_gl_context_destroy(ns_gl_context *c)
{
    if (!c) return;
    eglMakeCurrent(c->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (c->surface != EGL_NO_SURFACE) eglDestroySurface(c->display, c->surface);
    if (c->context != EGL_NO_CONTEXT) eglDestroyContext(c->display, c->context);
    g_free(c);
}

void
ns_gl_prepare_for_sandbox(void)
{
    const char *exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!exts || !strstr(exts, "EGL_MESA_platform_surfaceless"))
        (void)ns_gl_shared_display();
}

#elif defined(NS_ENABLE_WEBGL)

ns_gl_context *ns_gl_context_create(void) { return NULL; }
gboolean ns_gl_context_make_current(ns_gl_context *c) { (void)c; return FALSE; }
void ns_gl_context_release(ns_gl_context *c) { (void)c; }
void ns_gl_context_destroy(ns_gl_context *c) { (void)c; }

#endif

#ifndef NS_GLCTX_BACKEND_EGL
void
ns_gl_prepare_for_sandbox(void)
{
}
#endif
