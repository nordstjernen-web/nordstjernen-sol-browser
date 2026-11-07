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
    "--accent:#2f6fed;--accent-ink:#ffffff;" \
    "--accent-soft:rgba(47,111,237,.11);--ok:#13804a;--danger:#c62f3a;" \
    "--shadow:0 1px 2px rgba(16,24,40,.04),0 8px 28px rgba(16,24,40,.07);" \
    "--radius:18px;" \
    "--font:system-ui,-apple-system,\"Segoe UI\",\"Noto Sans\",Cantarell," \
    "Ubuntu,Helvetica,Arial,sans-serif;" \
    "--mono:ui-monospace,\"SF Mono\",\"Cascadia Mono\",\"DejaVu Sans Mono\"," \
    "Menlo,Consolas,monospace}\n" \
    "@media (prefers-color-scheme:dark){:root{--bg:#0f1218;--card:#181c24;" \
    "--text:#e8eaf0;--muted:#a4abba;--faint:#808898;--switch-off:#687185;" \
    "--line:rgba(255,255,255,.08);--field:rgba(255,255,255,.06);" \
    "--accent:#6c9cff;--accent-ink:#0b1020;" \
    "--accent-soft:rgba(108,156,255,.16);--ok:#4cc48a;--danger:#ff6b74;" \
    "--shadow:0 1px 2px rgba(0,0,0,.4),0 10px 30px rgba(0,0,0,.35)}}\n" \
    "*{box-sizing:border-box}\n" \
    "html,body{margin:0;min-height:100%;background:var(--bg);" \
    "color:var(--text);font-family:var(--font);line-height:1.5}\n" \
    "a{color:var(--accent);text-decoration:none}\n" \
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
    ".kicker{margin:0 0 10px;font-size:12px;font-weight:700;" \
    "letter-spacing:.08em;text-transform:uppercase;color:var(--faint)}\n" \
    ".crumb{display:inline-flex;align-items:center;gap:6px;height:32px;" \
    "padding:0 14px;border-radius:999px;background:var(--field);" \
    "color:var(--muted);font-size:13px;font-weight:600}\n" \
    ".crumb:hover{color:var(--text);text-decoration:none}\n"

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

#endif
