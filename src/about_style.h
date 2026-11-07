/* Nordstjernen: shared stylesheet of the built-in about: pages.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */
#ifndef NS_ABOUT_STYLE_H
#define NS_ABOUT_STYLE_H

#define NS_ABOUT_BASE_CSS \
    ":root{color-scheme:light dark;--bg:#f5f7fb;--card:#ffffff;" \
    "--text:#14171f;--muted:#596173;--faint:#6f7787;--switch-off:#878fa0;" \
    "--line:rgba(17,24,39,.08);--field:rgba(17,24,39,.05);" \
    "--accent:#2f6fed;--accent-ink:#ffffff;--accent-text:#1d4ed8;" \
    "--accent-soft:rgba(47,111,237,.11);--ok:#0f6b3e;--danger:#c62f3a;" \
    "--shadow:0 1px 2px rgba(16,24,40,.04),0 8px 28px rgba(16,24,40,.07);" \
    "--radius:18px;" \
    "--font:system-ui,-apple-system,\"Segoe UI\",\"Noto Sans\",Cantarell," \
    "Ubuntu,Helvetica,Arial,sans-serif;" \
    "--mono:ui-monospace,\"SF Mono\",\"Cascadia Mono\",\"DejaVu Sans Mono\"," \
    "Menlo,Consolas,monospace}\n" \
    "@media (prefers-color-scheme:dark){:root{--bg:#0f1218;--card:#181c24;" \
    "--text:#e8eaf0;--muted:#a4abba;--faint:#808898;--switch-off:#687185;" \
    "--line:rgba(255,255,255,.08);--field:rgba(255,255,255,.06);" \
    "--accent:#6c9cff;--accent-ink:#0b1020;--accent-text:#8fb3ff;" \
    "--accent-soft:rgba(108,156,255,.16);--ok:#4cc48a;--danger:#ff6b74;" \
    "--shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px rgba(0,0,0,.35)}}\n" \
    "*{box-sizing:border-box}\n" \
    "html,body{margin:0;min-height:100%;background:var(--bg);" \
    "color:var(--text);font-family:var(--font);line-height:1.5}\n" \
    "a{color:var(--accent);text-decoration:none}\n" \
    "input::placeholder{color:var(--muted);opacity:1}\n" \
    "a:hover{text-decoration:underline}\n" \
    ".card{background:var(--card);border:1px solid var(--line);" \
    "border-radius:var(--radius);box-shadow:var(--shadow)}\n" \
    ".btn{display:inline-flex;align-items:center;gap:8px;height:40px;" \
    "padding:0 18px;border-radius:999px;border:1px solid transparent;" \
    "font:inherit;font-size:14px;font-weight:600;cursor:pointer;" \
    "text-decoration:none;background:var(--field);color:var(--text)}\n" \
    ".btn:hover{text-decoration:none;filter:brightness(.96)}\n" \
    ".btn.primary{background:var(--accent);color:var(--accent-ink)}\n" \
    ".btn.danger{background:transparent;border-color:var(--line);" \
    "color:var(--danger)}\n" \
    ".btn:disabled{opacity:.55;cursor:default}\n" \
    ".btn[hidden]{display:none}\n" \
    ".group-label{margin:0 0 10px 20px;font-size:15px;font-weight:600;" \
    "color:var(--text)}\n" \
    ".kicker{margin:0 0 10px;font-size:12px;font-weight:700;" \
    "letter-spacing:.08em;text-transform:uppercase;color:var(--faint)}\n" \
    ".crumb{display:inline-flex;align-items:center;gap:6px;height:32px;" \
    "padding:0 14px;border-radius:999px;background:var(--field);" \
    "color:var(--muted);font-size:13px;font-weight:600}\n" \
    ".crumb:hover{color:var(--text);text-decoration:none}\n" \
    "details > summary{display:inline-flex;align-items:center;gap:8px;" \
    "min-height:32px;padding:0 10px 0 0;" \
    "border-radius:999px;cursor:pointer;color:var(--text);" \
    "font-size:13.5px;font-weight:600}\n" \
    "details > summary:hover{background:var(--field)}\n" \
    "details > summary::before{content:\"\";width:7px;height:7px;" \
    "margin:0 2px 0 1px;border-right:1.6px solid currentColor;" \
    "border-bottom:1.6px solid currentColor;transform:rotate(-45deg)}\n" \
    "details[open] > summary::before{transform:rotate(45deg)}\n"

#define NS_ABOUT_TINT_CSS \
    ".av.h1{background:rgba(13,148,136,.13);color:#0b7a70}\n" \
    ".av.h2{background:rgba(22,163,74,.13);color:#15803d}\n" \
    ".av.h3{background:rgba(217,119,6,.14);color:#b45309}\n" \
    ".av.h4{background:rgba(220,38,38,.12);color:#c02626}\n" \
    ".av.h5{background:rgba(219,39,119,.12);color:#be185d}\n" \
    ".av.h6{background:rgba(124,58,237,.12);color:#6d28d9}\n" \
    ".av.h7{background:rgba(71,85,105,.13);color:#475569}\n" \
    "@media (prefers-color-scheme:dark){" \
    ".av.h1{background:rgba(45,212,191,.16);color:#5eead4}" \
    ".av.h2{background:rgba(74,222,128,.15);color:#86efac}" \
    ".av.h3{background:rgba(251,191,36,.15);color:#fcd34d}" \
    ".av.h4{background:rgba(248,113,113,.16);color:#fca5a5}" \
    ".av.h5{background:rgba(244,114,182,.16);color:#f9a8d4}" \
    ".av.h6{background:rgba(167,139,250,.17);color:#c4b5fd}" \
    ".av.h7{background:rgba(148,163,184,.16);color:#cbd5e1}}\n"

#define NS_ABOUT_ICON_LINK(stroke, paths) \
    "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg " \
    "xmlns='http://www.w3.org/2000/svg' width='32' height='32' " \
    "viewBox='0 0 16 16' fill='none' stroke='" stroke "' " \
    "stroke-width='1.5' stroke-linecap='round' stroke-linejoin='round'%3E" \
    paths "%3C/svg%3E\">"
#define NS_ABOUT_ICON_BLUE "%234a86f0"
#define NS_ABOUT_ICON_HISTORY NS_ABOUT_ICON_LINK(NS_ABOUT_ICON_BLUE, \
    "%3Ccircle cx='8' cy='8' r='5.75'/%3E%3Cpath d='M8 4.75V8l2.25 1.5'/%3E")
#define NS_ABOUT_ICON_START \
    "<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg " \
    "xmlns='http://www.w3.org/2000/svg' width='32' height='32' " \
    "viewBox='0 0 16 16'%3E%3Crect x='.5' y='.5' width='15' height='15' " \
    "rx='4' fill='%232f6fed'/%3E%3Cpath fill='%23fff' d='M4.25 12V4h1.5l4.65 " \
    "5.6V4h1.35v8h-1.5L5.6 6.4V12z'/%3E%3C/svg%3E\">"
#define NS_ABOUT_ICON_SETTINGS NS_ABOUT_ICON_LINK(NS_ABOUT_ICON_BLUE, \
    "%3Cpath d='M2.75 4.5h6.5M12.25 4.5h1M2.75 11.5h1M6.75 11.5h6.5'/%3E" \
    "%3Ccircle cx='10.75' cy='4.5' r='1.5'/%3E" \
    "%3Ccircle cx='5.25' cy='11.5' r='1.5'/%3E")
#define NS_ABOUT_ICON_INFO NS_ABOUT_ICON_LINK(NS_ABOUT_ICON_BLUE, \
    "%3Ccircle cx='8' cy='8' r='5.75'/%3E%3Cpath d='M8 7.25v3.5M8 5v.1'/%3E")
#define NS_ABOUT_ICON_FOLDER NS_ABOUT_ICON_LINK(NS_ABOUT_ICON_BLUE, \
    "%3Cpath d='M1.75 4.25c0-.83.67-1.5 1.5-1.5h2.9l1.5 1.75h5.1c.83 0 " \
    "1.5.67 1.5 1.5v5.75c0 .83-.67 1.5-1.5 1.5H3.25c-.83 0-1.5-.67-1.5" \
    "-1.5z'/%3E")
#define NS_ABOUT_ICON_ERROR NS_ABOUT_ICON_LINK("%238a93a3", \
    "%3Cpath d='M4 1.75h4.75L12 5v9.25H4zM8.5 1.75V5.25H12M6.25 " \
    "8.25l3.5 3.5M9.75 8.25l-3.5 3.5'/%3E")
#define NS_ABOUT_ICON_DANGER NS_ABOUT_ICON_LINK("%23e5484d", \
    "%3Cpath d='M8 1.75 13.25 3.75v4c0 3.25-2.25 5.5-5.25 6.5-3-1-5.25" \
    "-3.25-5.25-6.5v-4z'/%3E%3Cpath d='M8 5.25v3.25M8 10.6v.1'/%3E")

#endif
