/* Nordstjernen: shared builders of the built-in about: pages.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "about_page.h"
#include "about_logo_gif.h"
#include "about_style.h"
#include "config.h"
#include "history.h"
#include "netutil.h"

#include <string.h>

const char *ns_app_self_exe(void);

char *
ns_about_read_first(const char *const *rel_paths, gsize *out_len)
{
    const char *exe = ns_app_self_exe();
    char *exe_dir = exe ? g_path_get_dirname(exe) : g_strdup(".");
    const char *user_data_dir = g_get_user_data_dir();
    const char *const *system_data_dirs = g_get_system_data_dirs();
    char *contents = NULL;
    gsize len = 0;
    for (int i = 0; rel_paths[i]; i++) {
        char *path = g_build_filename(exe_dir, rel_paths[i], NULL);
        gboolean ok = g_file_get_contents(path, &contents, &len, NULL);
        g_free(path);
        if (ok) break;
        if (user_data_dir && *user_data_dir) {
            path = g_build_filename(user_data_dir, rel_paths[i], NULL);
            ok = g_file_get_contents(path, &contents, &len, NULL);
            g_free(path);
            if (ok) break;
        }
        for (int j = 0; system_data_dirs && system_data_dirs[j]; j++) {
            path = g_build_filename(system_data_dirs[j], rel_paths[i], NULL);
            ok = g_file_get_contents(path, &contents, &len, NULL);
            g_free(path);
            if (ok) break;
        }
        if (ok) break;
    }
    g_free(exe_dir);
    if (out_len) *out_len = contents ? len : 0;
    return contents;
}

static const char *
about_logo_data_uri(void)
{
    static char *cached = NULL;
    if (cached) return cached;

    static const char *const gif_paths[] = {
        "share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../share/icons/hicolor/scalable/apps/nordstjernen.gif",
        "../../data/icons/hicolor/scalable/apps/nordstjernen.gif",
        "data/icons/hicolor/scalable/apps/nordstjernen.gif",
        NULL,
    };
    gsize gif_len = 0;
    char *gif = ns_about_read_first(gif_paths, &gif_len);
    if (gif) {
        gchar *b64 = g_base64_encode((const guchar *)gif, gif_len);
        g_free(gif);
        cached = g_strconcat("data:image/gif;base64,", b64, NULL);
        g_free(b64);
        return cached;
    }

    cached = g_strconcat("data:image/gif;base64,", about_logo_gif_b64, NULL);
    return cached;
}

char *
ns_about_logo_markup(void)
{
    return g_strdup_printf("<img class=\"mark-img\" src=\"%s\" alt=\"\">",
                           about_logo_data_uri());
}

char *
ns_about_substitute(const char *template_text,
                 const char *placeholder, const char *value)
{
    char **parts = g_strsplit(template_text, placeholder, -1);
    char *joined = g_strjoinv(value, parts);
    g_strfreev(parts);
    return joined;
}

static const char k_about_start_template[] =
    "<!doctype html><html lang=\"en\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<meta name=\"color-scheme\" content=\"light dark\">"
    "<title>New Tab</title>" NS_ABOUT_ICON_START
    "<style>\n" NS_ABOUT_BASE_CSS
    ".ntp{max-width:640px;margin:0 auto;padding:12vh 24px 48px;"
    "display:flex;flex-direction:column;align-items:center}\n"
    ".mark-img{display:block;width:48px;height:48px;border-radius:13px;"
    "box-shadow:0 8px 22px rgba(20,40,110,.24)}\n"
    "@media (prefers-color-scheme:dark){.mark-img{box-shadow:"
    "0 0 0 1px rgba(255,255,255,.1),0 8px 24px rgba(0,0,0,.5)}}\n"
    ".brand{display:flex;flex-direction:column;align-items:center;gap:10px}\n"
    ".brand h1{margin:0;font-size:20px;font-weight:600;color:var(--muted);"
    "letter-spacing:-.01em}\n"
    ".note{margin:22px 0 0;padding:6px 16px;border-radius:999px;"
    "background:var(--field);color:var(--muted);font-size:13.5px;"
    "text-align:center}\n"
    ".search{display:flex;align-items:center;gap:12px;width:100%;"
    "height:56px;margin-top:28px;padding:0 7px 0 22px;border-radius:999px;"
    "background:var(--card);border:1px solid var(--line);"
    "box-shadow:var(--shadow)}\n"
    ".search:focus-within{border-color:var(--accent);"
    "box-shadow:0 0 0 4px var(--accent-soft),var(--shadow)}\n"
    ".search svg{flex:0 0 auto;color:var(--faint)}\n"
    ".search input{flex:1 1 auto;min-width:0;height:100%;border:0;"
    "outline:0;background:transparent;color:var(--text);font:inherit;"
    "font-size:17px}\n"
    ".search button{flex:0 0 auto;display:flex;align-items:center;"
    "justify-content:center;width:42px;height:42px;border:0;"
    "border-radius:50%;background:var(--field);color:var(--muted);"
    "cursor:pointer}\n"
    ".search button svg{color:currentColor}\n"
    ".search.typed button{background:var(--accent);"
    "color:var(--accent-ink)}\n"
    ".search button:hover{filter:brightness(1.08)}\n"
    ".hint{min-height:18px;margin:10px 0 0;color:var(--muted);"
    "font-size:13px;text-align:center}\n"
    ".hint kbd{padding:1px 6px;border:1px solid var(--line);"
    "border-radius:5px;font:inherit;font-size:12px}\n"
    ".row{width:100%;margin-top:30px}\n"
    ".row h2{margin:0 0 6px 8px;font-size:13px;font-weight:600;"
    "color:var(--muted)}\n"
    ".sites{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));"
    "gap:6px 10px;margin:0;padding:0;list-style:none}\n"
    ".site{position:relative;min-width:0}\n"
    ".tile{display:flex;flex-direction:column;align-items:center;gap:9px;"
    "padding:14px 6px 10px;border-radius:16px;color:var(--text)}\n"
    ".tile:hover,.tile:focus-visible{background:var(--field);"
    "text-decoration:none}\n"
    ".av{display:flex;align-items:center;justify-content:center;"
    "width:48px;height:48px;border-radius:14px;background:var(--accent-soft);"
    "color:var(--accent);font-size:20px;font-weight:700;"
    "text-transform:uppercase}\n"
    NS_ABOUT_TINT_CSS
    ".nm{max-width:100%;font-size:12.5px;color:var(--muted);"
    "white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
    ".forget{position:absolute;top:4px;right:4px;display:flex;"
    "align-items:center;justify-content:center;width:24px;height:24px;"
    "border-radius:50%;color:var(--muted);font-size:16px;line-height:1;"
    "opacity:0}\n"
    ".site:hover .forget,.forget:focus-visible{opacity:1}\n"
    ".forget:hover{background:var(--card);color:var(--text);"
    "text-decoration:none}\n"
    ".links{display:flex;flex-wrap:wrap;justify-content:center;gap:6px;"
    "margin-top:34px}\n"
    ".links a{display:flex;align-items:center;gap:7px;height:32px;"
    "padding:0 12px;border-radius:999px;color:var(--muted);font-size:13px;"
    "font-weight:600}\n"
    ".links a:hover,.links a:focus-visible{background:var(--field);"
    "color:var(--text);text-decoration:none}\n"
    ".links svg{width:16px;height:16px;color:var(--accent)}\n"
    "@media (max-width:480px){.ntp{padding-top:7vh}"
    ".sites{grid-template-columns:repeat(3,minmax(0,1fr))}}\n"
    "</style></head>"
    "<body><main class=\"ntp\">"
    "<div class=\"brand\">__ND_LOGO_MARK__<h1>Nordstjernen</h1></div>"
    "<form class=\"search\" id=\"dsearch\" role=\"search\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.6\" stroke-linecap=\"round\""
    " aria-hidden=\"true\">"
    "<circle cx=\"7\" cy=\"7\" r=\"4.6\"/><path d=\"M10.4 10.4 14 14\"/></svg>"
    "<input id=\"sq\" name=\"q\" autocomplete=\"off\" dir=\"auto\""
    " aria-label=\"Search with __ND_SEARCH_NAME__ or enter an address\""
    " placeholder=\"Search with __ND_SEARCH_NAME__ or enter an address\">"
    "<button type=\"submit\" aria-label=\"Go\">"
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\""
    " stroke-linejoin=\"round\" aria-hidden=\"true\">"
    "<path d=\"M3 8h9.5M8.5 4l4 4-4 4\"/></svg>"
    "</button></form>"
    "<p class=\"hint\" id=\"hint\" aria-live=\"polite\"></p>"
    "__ND_NOTE____ND_TOP_SITES____ND_BOOKMARKS__"
    "<nav class=\"links\" aria-label=\"Browser pages\">"
    "<a href=\"about:history\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\""
    " stroke-linejoin=\"round\" aria-hidden=\"true\">"
    "<circle cx=\"8\" cy=\"8\" r=\"5.75\"/>"
    "<path d=\"M8 4.75V8l2.25 1.5\"/></svg>History</a>"
    "<a href=\"about:settings\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\""
    " aria-hidden=\"true\">"
    "<path d=\"M2.75 4.5h6.5M12.25 4.5h1M2.75 11.5h1M6.75 11.5h6.5\"/>"
    "<circle cx=\"10.75\" cy=\"4.5\" r=\"1.5\"/>"
    "<circle cx=\"5.25\" cy=\"11.5\" r=\"1.5\"/></svg>Settings</a>"
    "<a href=\"about:nordstjernen\">"
    "<svg width=\"20\" height=\"20\" viewBox=\"0 0 16 16\" fill=\"none\""
    " stroke=\"currentColor\" stroke-width=\"1.5\" stroke-linecap=\"round\""
    " aria-hidden=\"true\">"
    "<circle cx=\"8\" cy=\"8\" r=\"5.75\"/><path d=\"M8 7.25v3.5\"/>"
    "<circle cx=\"8\" cy=\"5.1\" r=\".4\" fill=\"currentColor\"/>"
    "</svg>About</a>"
    "</nav></main>"
    "<script>\n"
    "var searchUrl='__ND_SEARCH_URL__';\n"
    "var form=document.getElementById('dsearch');\n"
    "var sq=document.getElementById('sq');\n"
    "var hint=document.getElementById('hint');\n"
    "function addressFor(s){\n"
    " if(/^(https?|file|about|view-source):/i.test(s)) return s;\n"
    " if(!/\\s/.test(s)&&/^(localhost|[^\\s.\\/:]+(\\.[^\\s.\\/:]+)+)"
    "(:\\d+)?(\\/.*)?$/i.test(s)) return 'https://'+s;\n"
    " return null;}\n"
    "sq.addEventListener('input',function(){\n"
    " var s=sq.value.trim(); hint.textContent='';\n"
    " form.classList.toggle('typed',s!=='');\n"
    " if(!s) return;\n"
    " var a=addressFor(s);\n"
    " hint.appendChild(document.createTextNode('Press '));\n"
    " var k=document.createElement('kbd'); k.textContent='Enter';\n"
    " hint.appendChild(k);\n"
    " hint.appendChild(document.createTextNode(a\n"
    "  ? ' to open '+a.replace(/^https:\\/\\//,'')\n"
    "  : ' to search with __ND_SEARCH_NAME__'));});\n"
    "form.addEventListener('submit',function(e){e.preventDefault();\n"
    " var s=sq.value.trim(); if(!s) return;\n"
    " var a=addressFor(s); if(a){window.location.href=a;return;}\n"
    " window.location.href=searchUrl.indexOf('%s')>=0\n"
    "  ? searchUrl.replace('%s',encodeURIComponent(s))\n"
    "  : searchUrl+encodeURIComponent(s);});\n"
    "</script></body></html>";

static char *
about_search_url_js(void)
{
    ns_config_lock();
    const ns_config *cfg = ns_config_get();
    char *engine = (cfg && cfg->search_engine && *cfg->search_engine)
        ? g_strdup(cfg->search_engine) : g_strdup(NS_DEFAULT_SEARCH_ENGINE);
    ns_config_unlock();
    GString *out = g_string_new(NULL);
    for (const char *p = engine; *p; p++) {
        if (*p == '\'' || *p == '\\' || *p == '<' || *p == '"' ||
            (guchar)*p < 0x20)
            g_string_append_printf(out, "\\u%04x", (guchar)*p);
        else
            g_string_append_c(out, *p);
    }
    g_free(engine);
    return g_string_free(out, FALSE);
}

char *
ns_about_start_html(gboolean private_window)
{
    char *logo = ns_about_logo_markup();
    char *sites = private_window ? g_strdup("")
                                 : ns_history_top_sites_html(8);
    char *bookmarks = ns_history_bookmark_tiles_html(8);
    const char *note = "";
    if (private_window)
        note = "<p class=\"note\">Private window: pages you visit here are "
               "not added to your history</p>";
    char *page = ns_about_substitute(k_about_start_template,
                                     "__ND_LOGO_MARK__", logo);
    char *next = ns_about_substitute(page, "__ND_NOTE__", note);
    g_free(page);
    char *engine_name = ns_search_engine_name();
    char *search_name = g_markup_escape_text(engine_name ? engine_name
                                                         : "the web", -1);
    page = ns_about_substitute(next, "__ND_SEARCH_NAME__", search_name);
    g_free(next);
    char *search_url = about_search_url_js();
    next = ns_about_substitute(page, "__ND_SEARCH_URL__", search_url);
    g_free(page);
    g_free(search_url);
    g_free(search_name);
    g_free(engine_name);
    page = ns_about_substitute(next, "__ND_TOP_SITES__", sites);
    g_free(next);
    next = ns_about_substitute(page, "__ND_BOOKMARKS__", bookmarks);
    g_free(page);
    g_free(bookmarks);
    g_free(sites);
    g_free(logo);
    return next;
}
