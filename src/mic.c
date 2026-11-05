/* Nordstjernen — microphone capture (SDL) for getUserMedia audio + Web Audio. */

#include "mic.h"

#include <math.h>
#include <string.h>

#if defined(NS_HAVE_SDL)

#define SDL_MAIN_HANDLED
#include <SDL.h>
#ifdef main
#undef main
#endif
#include <gmodule.h>

#define NS_MIC_RING 16384

typedef struct {
    void (*set_main_ready)(void);
    Uint32 (*was_init)(Uint32 flags);
    int (*init_sub_system)(Uint32 flags);
    SDL_AudioDeviceID (*open_audio_device)(const char *device, int iscapture,
                                           const SDL_AudioSpec *desired,
                                           SDL_AudioSpec *obtained,
                                           int allowed_changes);
    void (*pause_audio_device)(SDL_AudioDeviceID dev, int pause_on);
    void (*close_audio_device)(SDL_AudioDeviceID dev);
    void (*lock_audio_device)(SDL_AudioDeviceID dev);
    void (*unlock_audio_device)(SDL_AudioDeviceID dev);
} ns_sdl_api;

static ns_sdl_api g_sdl;
static int g_sdl_loaded;

static GModule *
ns_mic_open_sdl(void)
{
#if defined(_WIN32)
    static const char *const names[] = { "SDL2.dll" };
#elif defined(__APPLE__)
    static const char *const names[] = {
        "@executable_path/../Frameworks/libSDL2-2.0.0.dylib",
        NS_SDL_LIBDIR "/libSDL2-2.0.0.dylib",
        "libSDL2-2.0.0.dylib",
    };
#else
    static const char *const names[] = {
        NS_SDL_LIBDIR "/libSDL2-2.0.so.0",
        "libSDL2-2.0.so.0",
    };
#endif
    for (gsize i = 0; i < G_N_ELEMENTS(names); i++) {
        GModule *m = g_module_open(names[i], G_MODULE_BIND_LAZY |
                                             G_MODULE_BIND_LOCAL);
        if (m) return m;
    }
    return NULL;
}

static gboolean
ns_mic_load_sdl(void)
{
    if (g_sdl_loaded) return g_sdl_loaded > 0;
    g_sdl_loaded = -1;
    GModule *m = ns_mic_open_sdl();
    if (!m) return FALSE;
    const struct { const char *name; gpointer *slot; } syms[] = {
        { "SDL_SetMainReady", (gpointer *)&g_sdl.set_main_ready },
        { "SDL_WasInit", (gpointer *)&g_sdl.was_init },
        { "SDL_InitSubSystem", (gpointer *)&g_sdl.init_sub_system },
        { "SDL_OpenAudioDevice", (gpointer *)&g_sdl.open_audio_device },
        { "SDL_PauseAudioDevice", (gpointer *)&g_sdl.pause_audio_device },
        { "SDL_CloseAudioDevice", (gpointer *)&g_sdl.close_audio_device },
        { "SDL_LockAudioDevice", (gpointer *)&g_sdl.lock_audio_device },
        { "SDL_UnlockAudioDevice", (gpointer *)&g_sdl.unlock_audio_device },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(syms); i++) {
        if (!g_module_symbol(m, syms[i].name, syms[i].slot) || !*syms[i].slot) {
            g_module_close(m);
            memset(&g_sdl, 0, sizeof g_sdl);
            return FALSE;
        }
    }
    g_module_make_resident(m);
    g_sdl.set_main_ready();
    g_sdl_loaded = 1;
    return TRUE;
}

static SDL_AudioDeviceID g_mic_dev;
static int g_mic_refs;
static float g_mic_ring[NS_MIC_RING];
static volatile guint g_mic_head;
static SDL_AudioSpec g_mic_spec;

static void
ns_mic_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    int channels = g_mic_spec.channels > 0 ? g_mic_spec.channels : 1;
    int frames = (int)((gsize)len / sizeof(float)) / channels;
    guint head = g_mic_head;
    for (int i = 0; i < frames; i++) {
        float acc = 0.0f;
        for (int c = 0; c < channels; c++) {
            float sample;
            memcpy(&sample, stream + (gsize)(i * channels + c) * sizeof(float),
                   sizeof sample);
            acc += sample;
        }
        g_mic_ring[head % NS_MIC_RING] = acc / (float)channels;
        head++;
    }
    g_mic_head = head;
}

void
ns_mic_preload(void)
{
    (void)ns_mic_load_sdl();
}

gboolean
ns_mic_acquire(void)
{
    if (g_mic_dev) { g_mic_refs++; return TRUE; }
    if (!ns_mic_load_sdl()) return FALSE;
    if (g_sdl.was_init(SDL_INIT_AUDIO) == 0 &&
        g_sdl.init_sub_system(SDL_INIT_AUDIO) != 0)
        return FALSE;
    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = ns_mic_cb;
    g_mic_dev = g_sdl.open_audio_device(NULL, 1, &want, &g_mic_spec,
                                        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                        SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!g_mic_dev)
        return FALSE;
    g_mic_head = 0;
    memset(g_mic_ring, 0, sizeof g_mic_ring);
    g_sdl.pause_audio_device(g_mic_dev, 0);
    g_mic_refs++;
    return TRUE;
}

void
ns_mic_release(void)
{
    if (g_mic_refs > 0 && --g_mic_refs == 0 && g_mic_dev) {
        g_sdl.close_audio_device(g_mic_dev);
        g_mic_dev = 0;
    }
}

void
ns_mic_fill_time_domain(guint8 *out, int n)
{
    if (!out || n <= 0) return;
    if (!g_mic_dev) {
        memset(out, 128, (gsize)n);
        return;
    }
    g_sdl.lock_audio_device(g_mic_dev);
    guint head = g_mic_head;
    for (int i = 0; i < n; i++) {
        int idx = (int)head - n + i;
        float s = 0.0f;
        if (idx >= 0)
            s = g_mic_ring[((guint)idx) % NS_MIC_RING];
        int v = 128 + (int)(s * 128.0f);
        out[i] = (guint8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    g_sdl.unlock_audio_device(g_mic_dev);
}

void
ns_mic_fill_frequency(guint8 *out, int n)
{
    if (!out || n <= 0) return;
    if (!g_mic_dev) {
        memset(out, 0, (gsize)n);
        return;
    }
    int win = n > NS_MIC_RING / 2 ? NS_MIC_RING : n * 2;
    float snap[NS_MIC_RING];
    g_sdl.lock_audio_device(g_mic_dev);
    guint head = g_mic_head;
    for (int t = 0; t < win; t++) {
        int idx = (int)head - win + t;
        snap[t] = idx >= 0 ? g_mic_ring[((guint)idx) % NS_MIC_RING] : 0.0f;
    }
    g_sdl.unlock_audio_device(g_mic_dev);
    for (int k = 0; k < n; k++) {
        double freq = (double)(k + 1) * M_PI / (double)n;
        double re = 0.0, im = 0.0;
        for (int t = 0; t < win; t++) {
            re += snap[t] * cos(freq * t);
            im -= snap[t] * sin(freq * t);
        }
        double mag = sqrt(re * re + im * im) / (double)win;
        int db = (int)(mag * 4.0 * 255.0);
        out[k] = (guint8)(db < 0 ? 0 : db > 255 ? 255 : db);
    }
}

#else

void     ns_mic_preload(void) {}
gboolean ns_mic_acquire(void) { return FALSE; }
void     ns_mic_release(void) {}
void ns_mic_fill_time_domain(guint8 *out, int n) {
    if (out && n > 0) memset(out, 128, (gsize)n);
}
void ns_mic_fill_frequency(guint8 *out, int n) {
    if (out && n > 0) memset(out, 0, (gsize)n);
}

#endif
