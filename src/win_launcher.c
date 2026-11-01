/* win_launcher.c - Windows bundle launcher and extraction guard. */

#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <io.h>

#define NS_APP_DIR L"app"
#define NS_BROWSER_EXE L"nordstjernen-ui.exe"
#define NS_EXIT_MISSING_RUNTIME 127
#define NS_STATUS_DLL_NOT_FOUND ((DWORD)0xC0000135u)
#define NS_STATUS_ENTRYPOINT_NOT_FOUND ((DWORD)0xC0000139u)

typedef struct ns_wbuf {
    wchar_t *data;
    size_t len;
    size_t cap;
} ns_wbuf;

static void *
ns_alloc(size_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static void *
ns_realloc(void *ptr, size_t size)
{
    if (!ptr) return ns_alloc(size);
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ptr, size);
}

static void
ns_free(void *ptr)
{
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);
}

static bool
ns_has_prefix(const wchar_t *s, const wchar_t *prefix)
{
    size_t n = wcslen(prefix);
    return wcsncmp(s, prefix, n) == 0;
}

static bool
ns_args_need_console(int argc, wchar_t **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (wcscmp(argv[i], L"--headless") == 0 ||
            wcscmp(argv[i], L"--print-config") == 0 ||
            ns_has_prefix(argv[i], L"--dump=") ||
            ns_has_prefix(argv[i], L"--url=") ||
            ns_has_prefix(argv[i], L"--viewport=") ||
            ns_has_prefix(argv[i], L"--eval=") ||
            ns_has_prefix(argv[i], L"--inspect=") ||
            ns_has_prefix(argv[i], L"--inspect-at=") ||
            ns_has_prefix(argv[i], L"--settle-ms="))
            return true;
    }
    return false;
}

static bool
ns_fd_is_bound(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

static void
ns_attach_parent_console(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE *fp;
    if (!ns_fd_is_bound(_fileno(stdout)))
        (void)freopen_s(&fp, "CONOUT$", "w", stdout);
    if (!ns_fd_is_bound(_fileno(stderr)))
        (void)freopen_s(&fp, "CONOUT$", "w", stderr);
    if (!ns_fd_is_bound(_fileno(stdin)))
        (void)freopen_s(&fp, "CONIN$", "r", stdin);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static wchar_t *
ns_module_path(void)
{
    DWORD cap = 512;
    for (;;) {
        wchar_t *buf = ns_alloc((size_t)cap * sizeof *buf);
        if (!buf) return NULL;
        DWORD n = GetModuleFileNameW(NULL, buf, cap);
        if (n == 0) {
            ns_free(buf);
            return NULL;
        }
        if (n + 1 < cap) {
            buf[n] = L'\0';
            return buf;
        }
        ns_free(buf);
        if (cap > 32768) return NULL;
        cap *= 2;
    }
}

static wchar_t *
ns_dirname(const wchar_t *path)
{
    size_t n = wcslen(path);
    wchar_t *dir = ns_alloc((n + 1) * sizeof *dir);
    if (!dir) return NULL;
    wcscpy(dir, path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    wchar_t *alt = wcsrchr(dir, L'/');
    if (!slash || (alt && alt > slash)) slash = alt;
    if (!slash) {
        wcscpy(dir, L".");
        return dir;
    }
    if (slash == dir) {
        slash[1] = L'\0';
        return dir;
    }
    if (slash == dir + 2 && dir[1] == L':') {
        slash[1] = L'\0';
        return dir;
    }
    *slash = L'\0';
    return dir;
}

static wchar_t *
ns_join_path(const wchar_t *dir, const wchar_t *name)
{
    size_t dl = wcslen(dir);
    size_t nl = wcslen(name);
    bool sep = dl > 0 && dir[dl - 1] != L'\\' && dir[dl - 1] != L'/';
    wchar_t *path = ns_alloc((dl + (sep ? 1 : 0) + nl + 1) * sizeof *path);
    if (!path) return NULL;
    wcscpy(path, dir);
    if (sep) wcscat(path, L"\\");
    wcscat(path, name);
    return path;
}

static bool
ns_file_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool
ns_wbuf_reserve(ns_wbuf *b, size_t extra)
{
    if (extra > ((size_t)-1) - b->len - 1) return false;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) return false;
        cap *= 2;
    }
    wchar_t *data = ns_realloc(b->data, cap * sizeof *data);
    if (!data) return false;
    b->data = data;
    b->cap = cap;
    return true;
}

static bool
ns_wbuf_append_char(ns_wbuf *b, wchar_t ch)
{
    if (!ns_wbuf_reserve(b, 1)) return false;
    b->data[b->len++] = ch;
    b->data[b->len] = L'\0';
    return true;
}

static bool
ns_wbuf_append_many(ns_wbuf *b, wchar_t ch, size_t count)
{
    if (!ns_wbuf_reserve(b, count)) return false;
    for (size_t i = 0; i < count; i++) b->data[b->len++] = ch;
    b->data[b->len] = L'\0';
    return true;
}

static bool
ns_wbuf_append_quoted(ns_wbuf *b, const wchar_t *arg)
{
    if (!ns_wbuf_append_char(b, L'"')) return false;
    size_t slashes = 0;
    for (const wchar_t *p = arg; *p; p++) {
        if (*p == L'\\') {
            slashes++;
            continue;
        }
        if (*p == L'"') {
            if (!ns_wbuf_append_many(b, L'\\', slashes * 2 + 1))
                return false;
            if (!ns_wbuf_append_char(b, *p)) return false;
            slashes = 0;
            continue;
        }
        if (slashes > 0) {
            if (!ns_wbuf_append_many(b, L'\\', slashes)) return false;
            slashes = 0;
        }
        if (!ns_wbuf_append_char(b, *p)) return false;
    }
    if (slashes > 0 && !ns_wbuf_append_many(b, L'\\', slashes * 2))
        return false;
    return ns_wbuf_append_char(b, L'"');
}

static wchar_t *
ns_build_command_line(const wchar_t *browser_path, int argc, wchar_t **argv)
{
    ns_wbuf b = {0};
    if (!ns_wbuf_append_quoted(&b, browser_path)) {
        ns_free(b.data);
        return NULL;
    }
    for (int i = 1; i < argc; i++) {
        if (!ns_wbuf_append_char(&b, L' ') ||
            !ns_wbuf_append_quoted(&b, argv[i])) {
            ns_free(b.data);
            return NULL;
        }
    }
    return b.data;
}

static void
ns_console_line(const wchar_t *text)
{
    fwprintf(stderr, L"%ls\n", text);
    fflush(stderr);
}

static void
ns_report_extract_needed(bool console)
{
    const wchar_t *text =
        L"Nordstjernen must be extracted before it can run.\n\n"
        L"In File Explorer, right-click the ZIP, choose Extract All, then run "
        L"nordstjernen.exe from the extracted nordstjernen-win64 folder.";
    ns_console_line(text);
    if (!console)
        MessageBoxW(NULL, text, L"Nordstjernen Sol Explorer", MB_OK | MB_ICONERROR |
                    MB_SETFOREGROUND);
}

static void
ns_report_start_error(bool console, DWORD error)
{
    wchar_t text[512];
    _snwprintf(text, 512,
               L"Nordstjernen could not start.\n\nWindows error: %lu",
               (unsigned long)error);
    text[511] = L'\0';
    ns_console_line(text);
    if (!console)
        MessageBoxW(NULL, text, L"Nordstjernen Sol Explorer", MB_OK | MB_ICONERROR |
                    MB_SETFOREGROUND);
}

static void
ns_report_runtime_error(bool console)
{
    const wchar_t *text =
        L"Nordstjernen could not find its bundled runtime files.\n\n"
        L"Extract the whole nordstjernen-win64 folder and keep "
        L"nordstjernen.exe and the app folder together.";
    ns_console_line(text);
    if (!console)
        MessageBoxW(NULL, text, L"Nordstjernen Sol Explorer", MB_OK | MB_ICONERROR |
                    MB_SETFOREGROUND);
}

int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
        LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool console = argv ? ns_args_need_console(argc, argv) : false;
    if (console) ns_attach_parent_console();

    UINT old_error_mode =
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    wchar_t *launcher_path = ns_module_path();
    wchar_t *dir = launcher_path ? ns_dirname(launcher_path) : NULL;
    wchar_t *app_dir = dir ? ns_join_path(dir, NS_APP_DIR) : NULL;
    wchar_t *browser_path = app_dir ? ns_join_path(app_dir, NS_BROWSER_EXE) : NULL;
    int rc = 1;

    if (!browser_path || !ns_file_exists(browser_path)) {
        ns_report_extract_needed(console);
        rc = NS_EXIT_MISSING_RUNTIME;
        goto out;
    }

    wchar_t *command_line = ns_build_command_line(browser_path, argc, argv);
    if (!command_line) {
        ns_report_start_error(console, ERROR_OUTOFMEMORY);
        rc = 1;
        goto out;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;

    BOOL ok = CreateProcessW(browser_path, command_line, NULL, NULL, TRUE, 0,
                             NULL, app_dir, &si, &pi);
    ns_free(command_line);
    if (!ok) {
        ns_report_start_error(console, GetLastError());
        rc = 1;
        goto out;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code))
        exit_code = 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code == NS_STATUS_DLL_NOT_FOUND ||
        exit_code == NS_STATUS_ENTRYPOINT_NOT_FOUND)
        ns_report_runtime_error(console);
    rc = (int)exit_code;

out:
    SetErrorMode(old_error_mode);
    if (argv) LocalFree(argv);
    ns_free(browser_path);
    ns_free(app_dir);
    ns_free(dir);
    ns_free(launcher_path);
    return rc;
}
