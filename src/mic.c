/* Nordstjernen — microphone capture (SDL) for getUserMedia audio + Web Audio. */

#include "mic.h"

#include <math.h>
#include <string.h>

#if defined(NS_HAVE_SDL)

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <gmodule.h>

#define NS_MIC_RING 16384
#define NS_MIC_RATE 48000

typedef struct {
    void (SDLCALL *set_main_ready)(void);
    SDL_InitFlags (SDLCALL *was_init)(SDL_InitFlags flags);
    bool (SDLCALL *init_sub_system)(SDL_InitFlags flags);
    SDL_AudioStream *(SDLCALL *open_audio_device_stream)(SDL_AudioDeviceID devid,
                                                         const SDL_AudioSpec *spec,
                                                         SDL_AudioStreamCallback callback,
                                                         void *userdata);
    bool (SDLCALL *resume_audio_stream_device)(SDL_AudioStream *stream);
    void (SDLCALL *destroy_audio_stream)(SDL_AudioStream *stream);
    bool (SDLCALL *lock_audio_stream)(SDL_AudioStream *stream);
    bool (SDLCALL *unlock_audio_stream)(SDL_AudioStream *stream);
    int (SDLCALL *get_audio_stream_data)(SDL_AudioStream *stream, void *buf,
                                         int len);
} ns_sdl_api;

static ns_sdl_api g_sdl;
static int g_sdl_loaded;

static GModule *
ns_mic_open_sdl(void)
{
#if defined(_WIN32)
    static const char *const names[] = { "SDL3.dll" };
#elif defined(__APPLE__)
    static const char *const names[] = {
        "@executable_path/../Frameworks/libSDL3.0.dylib",
        NS_SDL_LIBDIR "/libSDL3.0.dylib",
        "libSDL3.0.dylib",
    };
#else
    static const char *const names[] = {
        NS_SDL_LIBDIR "/libSDL3.so.0",
        "libSDL3.so.0",
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
        { "SDL_OpenAudioDeviceStream", (gpointer *)&g_sdl.open_audio_device_stream },
        { "SDL_ResumeAudioStreamDevice", (gpointer *)&g_sdl.resume_audio_stream_device },
        { "SDL_DestroyAudioStream", (gpointer *)&g_sdl.destroy_audio_stream },
        { "SDL_LockAudioStream", (gpointer *)&g_sdl.lock_audio_stream },
        { "SDL_UnlockAudioStream", (gpointer *)&g_sdl.unlock_audio_stream },
        { "SDL_GetAudioStreamData", (gpointer *)&g_sdl.get_audio_stream_data },
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

static SDL_AudioStream *g_mic_dev;
static int g_mic_refs;
static float g_mic_ring[NS_MIC_RING];
static volatile guint g_mic_head;

static void SDLCALL
ns_mic_cb(void *user, SDL_AudioStream *stream, int additional_amount,
          int total_amount)
{
    (void)user;
    (void)additional_amount;
    (void)total_amount;
    float buf[1024];
    guint head = g_mic_head;
    int got;
    while ((got = g_sdl.get_audio_stream_data(stream, buf, (int)sizeof buf)) > 0) {
        int samples = got / (int)sizeof(float);
        for (int i = 0; i < samples; i++)
            g_mic_ring[(head++) % NS_MIC_RING] = buf[i];
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
    if (!(g_sdl.was_init(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
        !g_sdl.init_sub_system(SDL_INIT_AUDIO))
        return FALSE;
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, NS_MIC_RATE };
    g_mic_head = 0;
    memset(g_mic_ring, 0, sizeof g_mic_ring);
    g_mic_dev = g_sdl.open_audio_device_stream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
                                               &spec, ns_mic_cb, NULL);
    if (!g_mic_dev)
        return FALSE;
    g_sdl.resume_audio_stream_device(g_mic_dev);
    g_mic_refs++;
    return TRUE;
}

void
ns_mic_release(void)
{
    if (g_mic_refs > 0 && --g_mic_refs == 0 && g_mic_dev) {
        g_sdl.destroy_audio_stream(g_mic_dev);
        g_mic_dev = NULL;
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
    g_sdl.lock_audio_stream(g_mic_dev);
    guint head = g_mic_head;
    for (int i = 0; i < n; i++) {
        int idx = (int)head - n + i;
        float s = 0.0f;
        if (idx >= 0)
            s = g_mic_ring[((guint)idx) % NS_MIC_RING];
        int v = 128 + (int)(s * 128.0f);
        out[i] = (guint8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    g_sdl.unlock_audio_stream(g_mic_dev);
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
    g_sdl.lock_audio_stream(g_mic_dev);
    guint head = g_mic_head;
    for (int t = 0; t < win; t++) {
        int idx = (int)head - win + t;
        snap[t] = idx >= 0 ? g_mic_ring[((guint)idx) % NS_MIC_RING] : 0.0f;
    }
    g_sdl.unlock_audio_stream(g_mic_dev);
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
