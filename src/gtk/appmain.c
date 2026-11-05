/* Nordstjernen — application entry point: process bootstrap, headless render
 * driver dispatch, and the out-of-process (IPC) GTK browser shell. */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <glib-unix.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#if defined(G_OS_WIN32) && defined(NS_HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

#ifdef NS_USE_WEB_TEXT_LIB
#include <web-text-lib/ns-pango-cache.h>
#endif

#include "config.h"
#include "debuglog.h"
#include "history.h"
#include "i18n.h"
#include "mcp.h"
#include "proc_limits.h"
#include "procview.h"
#include "procwindow.h"
#include "remotectl.h"
#include "security.h"
#include "threaddump.h"
#include "version.h"
#include "watchdog.h"

static char *g_self_exe;

const char *
ns_app_self_exe(void)
{
    return g_self_exe;
}

static void
init_self_exe(const char *argv0)
{
#ifdef __linux__
    char *resolved = g_file_read_link("/proc/self/exe", NULL);
    if (resolved) { g_self_exe = resolved; return; }
#endif
#ifdef __APPLE__
    {
        uint32_t size = 0;
        _NSGetExecutablePath(NULL, &size);
        if (size > 0 && size <= 32768) {
            char *raw = g_malloc(size);
            if (_NSGetExecutablePath(raw, &size) == 0) {
                char *real = realpath(raw, NULL);
                if (real) {
                    g_self_exe = g_strdup(real);
                    free(real);
                } else {
                    g_self_exe = g_strdup(raw);
                }
            }
            g_free(raw);
            if (g_self_exe) return;
        }
    }
#endif
#ifdef G_OS_WIN32
    {
        DWORD cap = MAX_PATH;
        wchar_t *buf = g_new(wchar_t, cap);
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        while (n >= cap && cap < 32768) {
            cap *= 2;
            buf = g_renew(wchar_t, buf, cap);
            n = GetModuleFileNameW(NULL, buf, cap);
        }
        if (n > 0 && n < cap)
            g_self_exe = g_utf16_to_utf8((gunichar2 *)buf, -1, NULL, NULL, NULL);
        g_free(buf);
        if (g_self_exe) return;
    }
#endif
    if (argv0) {
        if (g_path_is_absolute(argv0))
            g_self_exe = g_strdup(argv0);
        else
            g_self_exe = g_find_program_in_path(argv0);
        if (!g_self_exe) g_self_exe = g_strdup(argv0);
    }
}

static GLogWriterOutput
ns_log_writer(GLogLevelFlags log_level,
              const GLogField *fields, gsize n_fields,
              gpointer user_data)
{
    (void)user_data;
    const char *captured = NULL;
    const char *captured_domain = NULL;
    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value) {
            const char *m = fields[i].value;
            captured = m;
            if (strstr(m, "win32 session dbus binary not found") ||
                strstr(m, "but sizes must be >= 0") ||
                strstr(m, "Baselines must be inside the widget size") ||
                strstr(m, "without a current allocation") ||
                strstr(m, "No IM module matching GTK_IM_MODULE="))
                return G_LOG_WRITER_HANDLED;
        }
        if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0 && fields[i].value)
            captured_domain = fields[i].value;
    }
    if (captured) {
        ns_dlog_level lvl = NS_DLOG_INFO;
        if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL))
            lvl = NS_DLOG_ERROR;
        else if (log_level & G_LOG_LEVEL_WARNING)
            lvl = NS_DLOG_WARN;
        ns_debug_log_emit(lvl, captured_domain ? captured_domain : "glib",
                          "%s", captured);
    }
    return g_log_writer_default(log_level, fields, n_fields, user_data);
}

#ifdef G_OS_WIN32

__declspec(dllimport) HRESULT __stdcall
SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);

static void
ns_win32_set_app_id(void)
{
    (void)SetCurrentProcessExplicitAppUserModelID(L"Nordstjernen.Browser");
}

static void
ns_win32_use_fontconfig_backend(const char *dir)
{
#ifdef NS_HAVE_FONTCONFIG
    if (dir && !g_getenv("FONTCONFIG_FILE")) {
        char *conf = g_build_filename(dir, "etc", "fonts", "fonts.conf", NULL);
        if (g_file_test(conf, G_FILE_TEST_EXISTS))
            g_setenv("FONTCONFIG_FILE", conf, TRUE);
        g_free(conf);
    }
    if (dir && !g_getenv("FONTCONFIG_PATH")) {
        char *fonts = g_build_filename(dir, "etc", "fonts", NULL);
        if (g_file_test(fonts, G_FILE_TEST_IS_DIR))
            g_setenv("FONTCONFIG_PATH", fonts, TRUE);
        g_free(fonts);
    }
    if (!g_getenv("PANGOCAIRO_BACKEND"))
        g_setenv("PANGOCAIRO_BACKEND", "fc", TRUE);
#ifdef NS_USE_WEB_TEXT_LIB
    if (!g_getenv("NS_PANGOCAIRO_BACKEND"))
        g_setenv("NS_PANGOCAIRO_BACKEND", "fc", TRUE);
#endif
    FcInit();
#endif
}

static void
ns_win32_anchor_gtk_data(void)
{
    if (!g_self_exe) {
        ns_win32_use_fontconfig_backend(NULL);
        return;
    }
    char *dir = g_path_get_dirname(g_self_exe);
    if (!dir) {
        ns_win32_use_fontconfig_backend(NULL);
        return;
    }
    ns_win32_use_fontconfig_backend(dir);
    char *share_dir = g_build_filename(dir, "share", NULL);
    if (g_file_test(share_dir, G_FILE_TEST_IS_DIR)) {
        if (!g_getenv("GTK_DATA_PREFIX")) g_setenv("GTK_DATA_PREFIX", dir, TRUE);
        if (!g_getenv("GTK_EXE_PREFIX"))  g_setenv("GTK_EXE_PREFIX",  dir, TRUE);
        if (!g_getenv("XDG_DATA_DIRS"))   g_setenv("XDG_DATA_DIRS", share_dir, TRUE);
    }
    g_free(share_dir);
    if (!g_getenv("GDK_PIXBUF_MODULE_FILE")) {
        char *loaders = g_build_filename(dir,
            "lib", "gdk-pixbuf-2.0", "2.10.0", "loaders.cache", NULL);
        if (g_file_test(loaders, G_FILE_TEST_EXISTS))
            g_setenv("GDK_PIXBUF_MODULE_FILE", loaders, TRUE);
        g_free(loaders);
    }
    {
        char *ca = g_build_filename(dir,
            "etc", "ssl", "certs", "ca-bundle.crt", NULL);
        if (g_file_test(ca, G_FILE_TEST_EXISTS)) {
            if (!g_getenv("CURL_CA_BUNDLE")) g_setenv("CURL_CA_BUNDLE", ca, TRUE);
            if (!g_getenv("SSL_CERT_FILE"))  g_setenv("SSL_CERT_FILE",  ca, TRUE);
        }
        g_free(ca);
    }
    g_free(dir);
}

static gboolean
ns_win32_args_need_console(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (g_strcmp0(argv[i], "--headless")     == 0 ||
            g_strcmp0(argv[i], "--print-config") == 0 ||
            g_strcmp0(argv[i], "--version")      == 0 ||
            g_strcmp0(argv[i], "--help")         == 0 ||
            g_strcmp0(argv[i], "--wpt")          == 0 ||
            g_str_has_prefix(argv[i], "--dump=") ||
            g_str_has_prefix(argv[i], "--url=")  ||
            g_str_has_prefix(argv[i], "--viewport=") ||
            g_str_has_prefix(argv[i], "--eval=") ||
            g_str_has_prefix(argv[i], "--inspect=") ||
            g_str_has_prefix(argv[i], "--inspect-at=") ||
            g_str_has_prefix(argv[i], "--wpt-timeout-ms=") ||
            g_str_has_prefix(argv[i], "--settle-ms="))
            return TRUE;
    }
    return FALSE;
}

static gboolean
ns_win32_fd_is_bound(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static void
ns_win32_attach_parent_console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *fp;
    if (!ns_win32_fd_is_bound(_fileno(stdout)))
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
    if (!ns_win32_fd_is_bound(_fileno(stderr)))
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
    if (!ns_win32_fd_is_bound(_fileno(stdin)))
        (void)freopen_s(&fp, "CONIN$",  "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}
#endif

#ifdef __APPLE__
static void
ns_macos_anchor_gtk_data(void)
{
    if (!g_self_exe) return;
    char *macos_dir = g_path_get_dirname(g_self_exe);
    if (!macos_dir) return;
    char *contents = g_path_get_dirname(macos_dir);
    g_free(macos_dir);
    if (!contents) return;
    char *res = g_build_filename(contents, "Resources", NULL);
    g_free(contents);

    char *schemas = g_build_filename(res, "share", "glib-2.0", "schemas", NULL);
    if (g_file_test(schemas, G_FILE_TEST_IS_DIR) &&
        !g_getenv("GSETTINGS_SCHEMA_DIR"))
        g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    g_free(schemas);

    char *loaders = g_build_filename(res, "lib", "gdk-pixbuf-2.0",
                                     "2.10.0", "loaders", NULL);
    char *cache = g_build_filename(res, "lib", "gdk-pixbuf-2.0",
                                   "2.10.0", "loaders.cache", NULL);
    if (g_file_test(cache, G_FILE_TEST_EXISTS)) {
        if (!g_getenv("GDK_PIXBUF_MODULE_FILE"))
            g_setenv("GDK_PIXBUF_MODULE_FILE", cache, TRUE);
        if (!g_getenv("GDK_PIXBUF_MODULEDIR"))
            g_setenv("GDK_PIXBUF_MODULEDIR", loaders, TRUE);
    }
    g_free(cache);
    g_free(loaders);

    char *ca = g_build_filename(res, "etc", "ssl", "certs",
                                "ca-bundle.crt", NULL);
    if (g_file_test(ca, G_FILE_TEST_EXISTS)) {
        if (!g_getenv("CURL_CA_BUNDLE")) g_setenv("CURL_CA_BUNDLE", ca, TRUE);
        if (!g_getenv("SSL_CERT_FILE"))  g_setenv("SSL_CERT_FILE",  ca, TRUE);
    }
    g_free(ca);

    char *fonts = g_build_filename(res, "etc", "fonts", NULL);
    char *fonts_conf = g_build_filename(fonts, "fonts.conf", NULL);
    if (g_file_test(fonts_conf, G_FILE_TEST_EXISTS)) {
        if (!g_getenv("FONTCONFIG_FILE"))
            g_setenv("FONTCONFIG_FILE", fonts_conf, TRUE);
        if (!g_getenv("FONTCONFIG_PATH"))
            g_setenv("FONTCONFIG_PATH", fonts, TRUE);
    }
    g_free(fonts_conf);
    g_free(fonts);

    g_free(res);
}
#endif

#ifdef __linux__
static gboolean
ns_linux_has_gpu_render_node(void)
{
    GDir *dir = g_dir_open("/dev/dri", 0, NULL);
    if (!dir) return FALSE;
    const char *name;
    gboolean found = FALSE;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_prefix(name, "renderD") || g_str_has_prefix(name, "card")) {
            found = TRUE;
            break;
        }
    }
    g_dir_close(dir);
    return found;
}
#endif

static void
ns_apply_gsk_renderer(const char *pref)
{
    if (g_getenv("GSK_RENDERER")) return;
    if (!pref || !*pref ||
        g_ascii_strcasecmp(pref, "auto")    == 0 ||
        g_ascii_strcasecmp(pref, "default") == 0 ||
        g_ascii_strcasecmp(pref, "system")  == 0) {
#ifdef __linux__
        if (!ns_linux_has_gpu_render_node()) {
            g_setenv("GSK_RENDERER", "cairo", TRUE);
            g_message("no GPU detected; using the software (cairo) renderer");
        }
#endif
        return;
    }
    static const char *const known[] = {
        "gl", "ngl", "opengl", "vulkan", "cairo", "help",
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known); i++) {
        if (g_ascii_strcasecmp(pref, known[i]) == 0) {
            g_setenv("GSK_RENDERER", known[i], TRUE);
            return;
        }
    }
    g_warning("ignoring unknown gsk_renderer '%s' "
              "(expected one of: auto, gl, ngl, vulkan, cairo)", pref);
}

static gboolean
ns_proc_mode_wanted(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (g_strcmp0(a, "--headless") == 0 ||
            g_strcmp0(a, "--print-config") == 0 ||
            g_strcmp0(a, "--wpt") == 0 ||
            g_str_has_prefix(a, "--dump=") ||
            g_str_has_prefix(a, "--eval=") ||
            g_str_has_prefix(a, "--inspect=") ||
            g_str_has_prefix(a, "--inspect-at=") ||
            g_str_has_prefix(a, "--act="))
            return FALSE;
    }
    return TRUE;
}

static gboolean
ns_print_config_wanted(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (g_strcmp0(argv[i], "--print-config") == 0)
            return TRUE;
    return FALSE;
}

static int
ns_run_headless_host(int argc, char **argv)
{
    char *renderer = ns_proc_renderer_path();
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, renderer);
    g_ptr_array_add(args, (gpointer)NS_PROC_HEADLESS_FLAG);
    for (int i = 1; i < argc; i++)
        g_ptr_array_add(args, argv[i]);
    g_ptr_array_add(args, NULL);
    fflush(NULL);
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv(
        (const char *const *)args->pdata, G_SUBPROCESS_FLAGS_STDIN_INHERIT,
        &err);
    int rc = 1;
    if (proc && g_subprocess_wait(proc, NULL, &err))
        rc = g_subprocess_get_if_exited(proc)
             ? g_subprocess_get_exit_status(proc) : 1;
    if (err) {
        g_printerr("nordstjernen: could not run %s: %s\n", renderer,
                   err->message);
        g_error_free(err);
    }
    g_clear_object(&proc);
    g_ptr_array_free(args, TRUE);
    g_free(renderer);
    return rc;
}

static gboolean
ns_single_process_wanted(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (g_strcmp0(argv[i], "--single-process") == 0)
            return TRUE;
    const char *env = g_getenv(NS_PROC_SINGLE_PROCESS_ENV);
    return env && *env && g_strcmp0(env, "0") != 0;
}

static int
ns_print_version_or_help(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--version") == 0) {
            printf("Nordstjernen Web Browser %s\n", NS_VERSION);
            return 0;
        }
        if (g_strcmp0(argv[i], "--help") == 0 || g_strcmp0(argv[i], "-h") == 0) {
            fputs("Usage: nordstjernen [OPTION...] [URL]\n"
                  "\n"
                  "  --private              open a private window\n"
                  "  --window-size=WxH      initial window size\n"
                  "  --proxy=URL            send traffic through this proxy\n"
                  "  --enable-webgpu        allow WebGPU even when Settings turns it off\n"
                  "  --remote-control       let the MCP server drive this window\n"
                  "  --mcp                  run the MCP server over stdio\n"
                  "  --print-config         print the effective configuration and exit\n"
                  "  --headless             load URL without a window\n"
                  "  --dump=text|dom|layout|png:FILE|pdf:FILE|print:FILE\n"
                  "                         what a headless run writes (default text)\n"
                  "                         pdf: one page as tall as the document\n"
                  "                         print: paged PDF, A4 unless @page says otherwise\n"
                  "  --viewport=W[xH]       headless viewport size (default 1000 wide;\n"
                  "                         print: the sheet's content width)\n"
                  "  --version              print the version and exit\n"
                  "  --help                 print this help and exit\n",
                  stdout);
            return 0;
        }
    }
    return -1;
}

static void
ns_add_gtk_cache_writable_dir(void)
{
    g_autofree char *dir =
        g_build_filename(g_get_user_cache_dir(), "gtk-4.0", NULL);
    if (g_mkdir_with_parents(dir, 0700) == 0)
        ns_security_add_writable_dir(dir);
}

static gboolean
ns_private_mode_wanted(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (g_strcmp0(argv[i], "--private") == 0)
            return TRUE;
    return FALSE;
}

static int
ns_run_proc_gui(int argc, char **argv, const char *url,
                const char *gsk_renderer_override, const char *instance_id,
                gboolean recover, gboolean private_mode)
{
    (void)argc; (void)argv;
    const ns_config *cfg = ns_config_get();
    ns_apply_gsk_renderer(gsk_renderer_override ? gsk_renderer_override
                          : (cfg ? cfg->gsk_renderer : NULL));
    const char *start = url;
    if (!start || !*start)
        start = (cfg && cfg->home_url && *cfg->home_url) ? cfg->home_url
                                                         : "about:start";
    char *file_url = NULL;
    if (!strstr(start, "://") && !g_str_has_prefix(start, "about:") &&
        !g_str_has_prefix(start, "data:") &&
        g_file_test(start, G_FILE_TEST_EXISTS)) {
        char *abs_path = g_canonicalize_filename(start, NULL);
        file_url = g_filename_to_uri(abs_path, NULL, NULL);
        g_free(abs_path);
        if (file_url) start = file_url;
    }
    int status = ns_procapp_run(start, url && *url, instance_id, recover,
                                private_mode);
    g_free(file_url);
    g_free(g_self_exe);
    g_self_exe = NULL;
    ns_config_shutdown();
    return status;
}

int
main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    if (ns_win32_args_need_console(argc, argv))
        ns_win32_attach_parent_console();
#endif
    int info_rc = ns_print_version_or_help(argc, argv);
    if (info_rc >= 0) return info_rc;
    if (!ns_security_refuse_root()) return 77;
    if (!ns_security_require_landlock()) return 78;
    init_self_exe(argc > 0 ? argv[0] : NULL);
    ns_i18n_init(g_self_exe);
    if (ns_private_mode_wanted(argc, argv))
        g_setenv("NS_PRIVATE", "1", TRUE);
    ns_config_init();
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--mcp") == 0) {
            int rc = ns_mcp_run(g_self_exe);
            ns_config_shutdown();
            return rc;
        }
    }
#ifdef G_OS_WIN32
    ns_win32_anchor_gtk_data();
#endif
#ifdef __APPLE__
    ns_macos_anchor_gtk_data();
#endif
    gboolean proc_mode = ns_proc_mode_wanted(argc, argv);
    if (!proc_mode && !ns_print_config_wanted(argc, argv)) {
        int rc = ns_run_headless_host(argc, argv);
        g_free(g_self_exe);
        g_self_exe = NULL;
        ns_config_shutdown();
        return rc;
    }

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--enable-webgpu") == 0) {
#ifdef ND_HAVE_WEBGPU
            g_setenv("NS_WEBGPU_ALLOW", "1", TRUE);
#else
            g_printerr("nordstjernen: --enable-webgpu ignored "
                       "(built without WebGPU support)\n");
#endif
        }
    }

    if (proc_mode) {
        const ns_config *cfg = ns_config_get();
        if (ns_watchdog_should_supervise(argc, argv,
                                         cfg && cfg->watchdog_enabled)) {
            int rc = ns_watchdog_run_supervisor(g_self_exe, argc, argv);
            g_free(g_self_exe);
            g_self_exe = NULL;
            ns_config_shutdown();
            return rc;
        }
    }

    if (proc_mode)
        g_setenv("NS_CONFIG_VIA_PARENT", "1", TRUE);
    ns_security_win32_mitigations_init(proc_mode);
    ns_add_gtk_cache_writable_dir();

    gboolean single_process = proc_mode &&
                              ns_single_process_wanted(argc, argv);
    if (single_process)
        ns_proc_single_tab_enable();

    if (proc_mode) {
        ns_security_add_writable_dir("/dev/shm");
        char *rpath = ns_proc_renderer_path();
        if (rpath) {
            char *rdir = g_path_get_dirname(rpath);
            if (rdir) {
                ns_security_add_exec_dir(rdir);
                g_free(rdir);
            }
            g_free(rpath);
        }
        ns_security_allow_gpu_devices();
        ns_security_sandbox_init(g_self_exe);
    } else {
        ns_security_sandbox_init(g_self_exe);
    }
    ns_thread_dump_install_signal("nordstjernen");
    ns_debug_log_init();
    g_log_set_writer_func(ns_log_writer, NULL, NULL);
#ifdef G_OS_WIN32
    ns_win32_set_app_id();
#endif

    const char *gsk_renderer_override = NULL;
    gboolean private_window = FALSE;
    const char *startup_url = NULL;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--proxy=")) {
            const char *pxy = argv[i] + 8;
            if (*pxy) {
                g_setenv("NS_HTTP_PROXY", pxy, TRUE);
                g_setenv("NS_HTTPS_PROXY", pxy, TRUE);
            }
        } else if (g_str_has_prefix(argv[i], "--gsk-renderer=")) {
            gsk_renderer_override = argv[i] + 15;
        } else if (g_strcmp0(argv[i], "--private") == 0) {
            private_window = TRUE;
        } else if (g_strcmp0(argv[i], "--remote-control") == 0) {
            ns_remote_control_request();
        } else if (g_str_has_prefix(argv[i], "--window-size=")) {
            char *end = NULL;
            gint64 w = g_ascii_strtoll(argv[i] + 14, &end, 10);
            if (end != argv[i] + 14 && (*end == 'x' || *end == 'X') &&
                w > 0 && w < 100000) {
                const char *hs = end + 1;
                gint64 h = g_ascii_strtoll(hs, &end, 10);
                if (end != hs && *end == '\0' && h > 0 && h < 100000)
                    ns_procapp_set_window_size((int)w, (int)h);
            }
        } else if (g_strcmp0(argv[i], "--print-config") == 0) {
            char *dump = ns_config_dump();
            fputs(dump, stdout);
            g_free(dump);
            ns_config_shutdown();
            return 0;
        } else if (g_str_has_prefix(argv[i], "--url=")) {
            startup_url = argv[i] + 6;
        } else if (argv[i][0] != '-' && !startup_url) {
            startup_url = argv[i];
        }
    }

    if (proc_mode) {
        if (ns_watchdog_is_child(argc, argv)) {
            ns_watchdog_child_guard_parent_death();
            ns_watchdog_child_arm_hang_monitor(0);
        }
        const char *session = ns_watchdog_child_session_arg(argc, argv);
        gboolean recover = ns_watchdog_child_is_recovery();
        return ns_run_proc_gui(argc, argv, startup_url, gsk_renderer_override,
                               session, recover, private_window);
    }
    ns_config_shutdown();
    return 0;
}
