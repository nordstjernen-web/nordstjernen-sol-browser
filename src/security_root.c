/* Nordstjernen — refuse to start as root or Administrator; drop elevation on Windows.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "security.h"

#include <stdio.h>

#ifndef G_OS_WIN32
#include <unistd.h>
#endif

#ifdef G_OS_WIN32
#include <windows.h>
#endif

#ifdef G_OS_WIN32
static gboolean
ns_win_is_elevated(void)
{
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    PSID admins_sid = NULL;
    BOOL is_member = FALSE;
    if (!AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admins_sid))
        return FALSE;
    if (!CheckTokenMembership(NULL, admins_sid, &is_member))
        is_member = FALSE;
    FreeSid(admins_sid);
    return is_member ? TRUE : FALSE;
}

static gboolean
ns_win_relaunch_deelevated(void)
{
    HWND shell = GetShellWindow();
    if (!shell) return FALSE;
    DWORD shell_pid = 0;
    GetWindowThreadProcessId(shell, &shell_pid);
    if (!shell_pid) return FALSE;

    HANDLE shell_proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, shell_pid);
    if (!shell_proc) return FALSE;

    HANDLE shell_tok = NULL;
    gboolean relaunched = FALSE;
    if (OpenProcessToken(shell_proc, TOKEN_DUPLICATE, &shell_tok)) {
        HANDLE user_tok = NULL;
        DWORD access = TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE |
                       TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
        if (DuplicateTokenEx(shell_tok, access, NULL, SecurityImpersonation,
                             TokenPrimary, &user_tok)) {
            TOKEN_ELEVATION elev;
            DWORD got = 0;
            gboolean shell_elevated =
                GetTokenInformation(user_tok, TokenElevation, &elev,
                                    sizeof elev, &got) && elev.TokenIsElevated;
            if (!shell_elevated) {
                wchar_t exe[4096];
                DWORD n = GetModuleFileNameW(NULL, exe, G_N_ELEMENTS(exe));
                LPWSTR app = (n > 0 && n < G_N_ELEMENTS(exe)) ? exe : NULL;
                LPWSTR src = GetCommandLineW();
                gsize bytes = ((gsize)lstrlenW(src) + 1) * sizeof(wchar_t);
                LPWSTR cmd = g_malloc(bytes);
                memcpy(cmd, src, bytes);
                SetEnvironmentVariableW(L"NS_DEELEVATED", L"1");
                STARTUPINFOW si;
                memset(&si, 0, sizeof si);
                si.cb = sizeof si;
                PROCESS_INFORMATION pi;
                memset(&pi, 0, sizeof pi);
                if (CreateProcessWithTokenW(user_tok, 0, app, cmd, 0,
                                            NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    relaunched = TRUE;
                }
                g_free(cmd);
            }
            CloseHandle(user_tok);
        }
        CloseHandle(shell_tok);
    }
    CloseHandle(shell_proc);
    return relaunched;
}
#endif

gboolean
ns_security_refuse_root(void)
{
#if defined(__linux__) || defined(__APPLE__)
    if (geteuid() != 0 && getuid() != 0) return TRUE;
    if (ns_security_env_flag("NS_ALLOW_ROOT")) {
        g_warning("nordstjernen: running as root because NS_ALLOW_ROOT is set");
        return TRUE;
    }
    fprintf(stderr,
        "nordstjernen: refusing to run as root.\n"
        "  Web browsers process untrusted content; running as root exposes\n"
        "  the whole system if the renderer is compromised.\n"
        "  Re-run as an unprivileged user, or set NS_ALLOW_ROOT=1 to override.\n");
    return FALSE;
#elif defined(G_OS_WIN32)
    if (!ns_win_is_elevated()) return TRUE;
    if (ns_security_env_flag("NS_ALLOW_ROOT")) {
        g_warning("nordstjernen: running as Administrator because "
                  "NS_ALLOW_ROOT is set");
        return TRUE;
    }
    if (!ns_security_env_flag("NS_DEELEVATED") && ns_win_relaunch_deelevated()) {
        g_warning("nordstjernen: dropped Administrator rights by relaunching "
                  "as the current desktop user");
        return FALSE;
    }

    const char *msg =
        "Nordstjernen is running as administrator.\n"
        "\n"
        "Nordstjernen tried to restart itself without administrator rights but "
        "could not. You do not need administrator rights to browse the web, and "
        "it is much safer without them: if a web page exploited a flaw in the "
        "browser while it has administrator rights, it could take over your "
        "whole PC instead of being limited to your own user account.\n"
        "\n"
        "How to start Nordstjernen normally:\n"
        "  -  Close this window, then open Nordstjernen with a normal click on "
        "its Start-menu or desktop icon. Do not choose \"Run as "
        "administrator\".\n"
        "  -  If you started it from a Command Prompt or PowerShell, use an "
        "ordinary window that is not marked \"Administrator\".\n"
        "  -  If a shortcut keeps starting it elevated, right-click the "
        "shortcut, choose Properties, then Advanced, and turn off \"Run as "
        "administrator\".\n"
        "\n"
        "Do you want to run as administrator anyway?\n"
        "  -  No (recommended): quit, then reopen Nordstjernen normally.\n"
        "  -  Yes: run as administrator this once, at your own risk.";
    fprintf(stderr, "nordstjernen: %s\n", msg);

    int choice = MessageBoxA(NULL, msg,
        "Nordstjernen - running as administrator",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
        MB_SETFOREGROUND | MB_TOPMOST);
    if (choice == IDYES) {
        g_warning("nordstjernen: running as Administrator at the user's "
                  "request");
        return TRUE;
    }
    return FALSE;
#else
    return TRUE;
#endif
}
