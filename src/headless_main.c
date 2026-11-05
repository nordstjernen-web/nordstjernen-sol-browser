/* Nordstjernen — headless command line (--headless, --dump, --wpt, --act …) hosted by the renderer executable. */

#include "headless_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef NS_USE_WEB_TEXT_LIB
#include <web-text-lib/ns-pango-cache.h>
#endif

#include "bytecode_cache.h"
#include "cache.h"
#include "config.h"
#include "debuglog.h"
#include "engine.h"
#include "font.h"
#include "headless.h"
#include "history.h"
#include "i18n.h"
#include "media_os.h"
#include "net.h"
#include "security.h"
#include "spellcheck.h"
#include "threaddump.h"

static char *
headless_self_exe(const char *argv0)
{
#ifdef __linux__
    char *resolved = g_file_read_link("/proc/self/exe", NULL);
    if (resolved) return resolved;
#endif
#ifdef G_OS_WIN32
    wchar_t wexe[4096];
    DWORD n = GetModuleFileNameW(NULL, wexe, 4096);
    if (n > 0 && n < 4096)
        return g_utf16_to_utf8((const gunichar2 *)wexe, -1, NULL, NULL, NULL);
#endif
    return argv0 ? g_canonicalize_filename(argv0, NULL) : NULL;
}

#ifdef G_OS_WIN32
static gboolean
headless_fd_is_bound(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static void
headless_attach_parent_console(void)
{
    if (headless_fd_is_bound(_fileno(stdout)) &&
        headless_fd_is_bound(_fileno(stderr)))
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *fp;
    if (!headless_fd_is_bound(_fileno(stdout)))
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
    if (!headless_fd_is_bound(_fileno(stderr)))
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
    if (!headless_fd_is_bound(_fileno(stdin)))
        (void)freopen_s(&fp, "CONIN$",  "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
#endif

static GLogWriterOutput
headless_log_writer(GLogLevelFlags log_level, const GLogField *fields,
                    gsize n_fields, gpointer user_data)
{
    const char *message = NULL;
    const char *domain = NULL;
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value)
            message = fields[i].value;
        if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0 && fields[i].value)
            domain = fields[i].value;
    }
    if (message) {
        ns_dlog_level lvl = NS_DLOG_INFO;
        if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
            lvl = NS_DLOG_ERROR;
        else if (log_level & G_LOG_LEVEL_WARNING)
            lvl = NS_DLOG_WARN;
        ns_debug_log_emit(lvl, domain ? domain : "glib", "%s", message);
    }
    return g_log_writer_default(log_level, fields, n_fields, user_data);
}

static void
headless_add_output_dirs(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *p = NULL;
        if (g_str_has_prefix(argv[i], "--dump=")) {
            const char *v = argv[i] + 7;
            if      (g_str_has_prefix(v, "png:")) p = v + 4;
            else if (g_str_has_prefix(v, "pdf:")) p = v + 4;
            else if (g_str_has_prefix(v, "print:")) p = v + 6;
        } else if (g_str_has_prefix(argv[i], "--screenshot=")) {
            p = argv[i] + 13;
        }
        if (!p || !*p || *p == '-') continue;
        char *dir = g_path_get_dirname(p);
        if (dir && *dir) {
            g_mkdir_with_parents(dir, 0700);
            ns_security_add_writable_dir(dir);
        }
        g_free(dir);
    }
}

static void
headless_report_perf(void)
{
    guint64 fetches = 0, bytes = 0, relayouts = 0;
    double sum_ms = 0, span_ms = 0, layout_ms = 0;
    ns_net_perf_snapshot(&fetches, &bytes, &sum_ms, &span_ms);
    ns_engine_layout_perf(&relayouts, &layout_ms);
#ifdef NS_USE_WEB_TEXT_LIB
    guint64 shape_hits = 0, shape_misses = 0, shape_skips = 0,
            shape_entries = 0;
    guint64 brk_hits = 0, brk_misses = 0, brk_skips = 0, brk_entries = 0;
    guint64 it_hits = 0, it_misses = 0, it_skips = 0, it_entries = 0;
    ns_pango_cache_get_stats(&shape_hits, &shape_misses, &shape_skips,
                             &shape_entries);
    ns_pango_break_cache_stats(&brk_hits, &brk_misses, &brk_skips,
                               &brk_entries);
    ns_pango_item_cache_stats(&it_hits, &it_misses, &it_skips, &it_entries);
    g_printerr("[net perf] fetches=%" G_GUINT64_FORMAT " bytes=%"
               G_GUINT64_FORMAT " net_span=%.1fms net_sum=%.1fms | "
               "relayouts=%" G_GUINT64_FORMAT " layout=%.1fms | "
               "shape_cache hits=%" G_GUINT64_FORMAT " misses=%"
               G_GUINT64_FORMAT " skipped=%" G_GUINT64_FORMAT " entries=%"
               G_GUINT64_FORMAT " | break_cache hits=%" G_GUINT64_FORMAT
               " misses=%" G_GUINT64_FORMAT " skipped=%" G_GUINT64_FORMAT
               " entries=%" G_GUINT64_FORMAT " | item_cache hits=%"
               G_GUINT64_FORMAT " misses=%" G_GUINT64_FORMAT " skipped=%"
               G_GUINT64_FORMAT " entries=%" G_GUINT64_FORMAT "\n",
               fetches, bytes, span_ms, sum_ms, relayouts, layout_ms,
               shape_hits, shape_misses, shape_skips, shape_entries,
               brk_hits, brk_misses, brk_skips, brk_entries,
               it_hits, it_misses, it_skips, it_entries);
#else
    g_printerr("[net perf] fetches=%" G_GUINT64_FORMAT " bytes=%"
               G_GUINT64_FORMAT " net_span=%.1fms net_sum=%.1fms | "
               "relayouts=%" G_GUINT64_FORMAT " layout=%.1fms\n",
               fetches, bytes, span_ms, sum_ms, relayouts, layout_ms);
#endif
}

static int
headless_run(ns_headless_opts *hopts)
{
    if (!g_getenv("NS_WEBGL_ALLOW"))
        g_setenv("NS_WEBGL_ALLOW", "1", TRUE);
    ns_net_init();
    ns_net_set_early_hints_handler(ns_engine_early_hints);
    ns_net_set_allow_file_urls(TRUE);
    if (hopts->debug_levels & (1u << NS_DLOG_NET))
        ns_net_set_log_fetches(TRUE);
    ns_cache_init();
    ns_bytecode_cache_init();
    ns_history_init();
    ns_font_init();
    ns_spell_init();
    char *file_url = NULL;
    if (hopts->url && !strstr(hopts->url, "://") &&
        !g_str_has_prefix(hopts->url, "about:") &&
        !g_str_has_prefix(hopts->url, "data:") &&
        g_file_test(hopts->url, G_FILE_TEST_EXISTS)) {
        char *abs_path = g_canonicalize_filename(hopts->url, NULL);
        file_url = g_filename_to_uri(abs_path, NULL, NULL);
        g_free(abs_path);
        if (file_url) hopts->url = file_url;
    }
    int rc = ns_headless_run(hopts);
    if (hopts->debug_levels & (1u << NS_DLOG_NET))
        headless_report_perf();
    g_free(file_url);
    ns_bytecode_cache_shutdown();
    ns_history_shutdown();
    ns_cache_shutdown();
    ns_net_shutdown();
    ns_config_shutdown();
    return rc;
}

static gboolean
headless_parse_int(const char *s, gint64 lo, gint64 hi, int *out)
{
    char *end = NULL;
    gint64 n = g_ascii_strtoll(s, &end, 10);
    if (end == s || *end != '\0' || n < lo || n >= hi) return FALSE;
    *out = (int)n;
    return TRUE;
}

static int
headless_parse(int argc, char **argv, ns_headless_opts *hopts)
{
    gboolean dump_set = FALSE;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (g_str_has_prefix(a, "--proxy=")) {
            const char *pxy = a + 8;
            ns_net_set_proxy_override(pxy);
            if (*pxy) {
                g_setenv("NS_HTTP_PROXY", pxy, TRUE);
                g_setenv("NS_HTTPS_PROXY", pxy, TRUE);
            }
        } else if (g_str_has_prefix(a, "--dump=")) {
            const char *v = a + 7;
            dump_set = TRUE;
            if      (g_strcmp0(v, "text")   == 0) hopts->dump = NS_DUMP_TEXT;
            else if (g_strcmp0(v, "dom")    == 0) hopts->dump = NS_DUMP_DOM;
            else if (g_strcmp0(v, "layout") == 0) hopts->dump = NS_DUMP_LAYOUT;
            else if (g_strcmp0(v, "none")   == 0) hopts->dump = NS_DUMP_NONE;
            else if (g_str_has_prefix(v, "png:")) { hopts->dump = NS_DUMP_PNG; hopts->out_path = v + 4; }
            else if (g_str_has_prefix(v, "pdf:")) { hopts->dump = NS_DUMP_PDF; hopts->out_path = v + 4; }
            else if (g_str_has_prefix(v, "print:")) { hopts->dump = NS_DUMP_PRINT; hopts->out_path = v + 6; }
        } else if (g_str_has_prefix(a, "--viewport=")) {
            const char *spec = a + 11;
            char *end = NULL;
            gint64 w = g_ascii_strtoll(spec, &end, 10);
            gboolean ok = end != spec && w > 0 && w < 100000;
            gint64 h = 0;
            if (ok && (*end == 'x' || *end == 'X')) {
                const char *hs = end + 1;
                h = g_ascii_strtoll(hs, &end, 10);
                ok = end != hs && *end == '\0' && h > 0 && h < 100000;
            } else if (ok) {
                ok = *end == '\0';
            }
            if (!ok) {
                fprintf(stderr,
                        "nordstjernen: --viewport wants WIDTH or WIDTHxHEIGHT, "
                        "got '%s'\n", spec);
                return 2;
            }
            hopts->viewport_width = (int)w;
            if (h > 0) hopts->viewport_height = (int)h;
        } else if (g_str_has_prefix(a, "--viewport-height=")) {
            headless_parse_int(a + 18, 1, 100000, &hopts->viewport_height);
        } else if (g_str_has_prefix(a, "--settle-ms=")) {
            headless_parse_int(a + 12, 0, 600000, &hopts->settle_ms);
        } else if (g_str_has_prefix(a, "--time-ms=")) {
            headless_parse_int(a + 10, 0, 600000, &hopts->time_ms);
        } else if (g_strcmp0(a, "--wpt") == 0) {
            hopts->wpt = TRUE;
        } else if (g_strcmp0(a, "--timing") == 0) {
            hopts->timing = TRUE;
        } else if (g_str_has_prefix(a, "--wpt-timeout-ms=")) {
            headless_parse_int(a + 17, 1, 600000, &hopts->wpt_timeout_ms);
        } else if (g_str_has_prefix(a, "--act=")) {
            hopts->actions = a + 6;
        } else if (g_str_has_prefix(a, "--eval=")) {
            hopts->eval = a + 7;
        } else if (g_str_has_prefix(a, "--inspect=")) {
            hopts->inspect = a + 10;
        } else if (g_str_has_prefix(a, "--inspect-at=")) {
            hopts->inspect_at = a + 13;
        } else if (g_strcmp0(a, "--debug") == 0) {
            hopts->debug_levels = ns_headless_debug_mask("all");
        } else if (g_str_has_prefix(a, "--debug=")) {
            hopts->debug_levels = ns_headless_debug_mask(a + 8);
        } else if (g_str_has_prefix(a, "--url=")) {
            hopts->url = a + 6;
        } else if (a[0] != '-' && !hopts->url) {
            hopts->url = a;
        }
    }
    if (!dump_set && (hopts->inspect || hopts->inspect_at || hopts->wpt))
        hopts->dump = NS_DUMP_NONE;
    return 0;
}

int
ns_headless_main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    headless_attach_parent_console();
#endif
    char *self_exe = headless_self_exe(argc > 0 ? argv[0] : NULL);
    ns_i18n_init(self_exe);
    ns_config_init();
    ns_security_win32_mitigations_init(FALSE);
    headless_add_output_dirs(argc, argv);
    ns_os_media_preinit();
    ns_security_landlock_scope_abstract_sockets();
    ns_security_sandbox_init(self_exe);
    ns_security_seccomp_init();
    ns_thread_dump_install_signal("nordstjernen");
    ns_debug_log_init();
    g_log_set_writer_func(headless_log_writer, NULL, NULL);

    ns_headless_opts hopts = {
        .dump = NS_DUMP_TEXT,
        .settle_ms = 200,
        .time_ms = 1000,
    };
    int rc = headless_parse(argc, argv, &hopts);
    if (rc == 0)
        rc = headless_run(&hopts);
    g_free(self_exe);
    if (!ns_net_idle()) {
        fflush(NULL);
        _exit(rc);
    }
    return rc;
}
