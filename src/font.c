/* Nordstjernen — @font-face web font loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "font.h"

#include <gio/gio.h>
#include "ns_pango.h"
#ifdef NS_USE_WEB_TEXT_LIB
#include <web-text-lib/ns-pango-cache.h>
#endif
#include <stdio.h>
#include <string.h>

#include "net.h"
#include "paint.h"
#ifdef NS_HAVE_WOFF2
#include "woff2.h"
#endif

#ifdef NS_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#define NS_FONT_FACE_VERSION_BASE 0x40000000
#endif
#ifdef NS_HAVE_PANGOFT2
#define NS_HAVE_PANGOFC 1
#endif
#ifdef NS_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#endif

typedef struct ns_font_entry {
    char *family;
    char *url;
    ns_font_descriptors descriptors;
    int declared;
    gboolean loaded;
    gboolean inflight;
    GCancellable *cancel;
    char *deferred_path;
} ns_font_entry;

typedef struct ns_font_pending {
    GPtrArray *keys;
    GCancellable *cancel;
} ns_font_pending;

static GHashTable        *g_entries;
static GHashTable        *g_pending_by_url;
static char              *g_cache_dir;
static ns_font_loaded_cb  g_loaded_cb;
static guint              g_font_generation;
static int                g_faces_declared;
static gpointer           g_loaded_ud;

typedef struct ns_font_idle_waiter {
    ns_font_idle_cb cb;
    gpointer        ud;
} ns_font_idle_waiter;

static GArray *g_idle_waiters;

#ifdef NS_HAVE_FONTCONFIG
typedef struct ns_font_family_index {
    GHashTable *spelling_by_exact;
    GHashTable *exact_by_loose;
    FcConfig   *config;
    int         system_fonts;
    int         app_fonts;
    guint       generation;
} ns_font_family_index;

static ns_font_family_index g_family_index;
static GMutex               g_family_index_lock;
static const char           g_family_ambiguous[] = "";
#endif

guint
ns_font_pending_count(void)
{
    return g_pending_by_url ? g_hash_table_size(g_pending_by_url) : 0;
}

void
ns_font_add_idle_cb(ns_font_idle_cb cb, gpointer user_data)
{
    if (!cb) return;
    if (!g_idle_waiters)
        g_idle_waiters = g_array_new(FALSE, FALSE, sizeof(ns_font_idle_waiter));
    ns_font_idle_waiter w = { cb, user_data };
    g_array_append_val(g_idle_waiters, w);
}

void
ns_font_remove_idle_cb(ns_font_idle_cb cb, gpointer user_data)
{
    if (!g_idle_waiters) return;
    for (guint i = 0; i < g_idle_waiters->len; ) {
        ns_font_idle_waiter *w = &g_array_index(g_idle_waiters, ns_font_idle_waiter, i);
        if (w->cb == cb && w->ud == user_data)
            g_array_remove_index(g_idle_waiters, i);
        else
            i++;
    }
}

static void
ns_font_notify_idle(void)
{
    if (!g_idle_waiters || g_idle_waiters->len == 0) return;
    if (ns_font_pending_count() > 0) return;
    GArray *waiters = g_idle_waiters;
    g_idle_waiters = NULL;
    for (guint i = 0; i < waiters->len; i++) {
        ns_font_idle_waiter *w = &g_array_index(waiters, ns_font_idle_waiter, i);
        w->cb(w->ud);
    }
    g_array_free(waiters, TRUE);
}

static void
ns_font_pending_free(gpointer data)
{
    ns_font_pending *p = data;
    if (!p) return;
    if (p->cancel) g_object_unref(p->cancel);
    if (p->keys) g_ptr_array_free(p->keys, TRUE);
    g_free(p);
}

static char *
ns_font_entry_key(const char *family, const char *url,
                  ns_font_descriptors descriptors)
{
    return g_strdup_printf("%s\x1f%s\x1f%d\x1f%d", family, url ? url : "",
                           descriptors.weight, (int)descriptors.slant);
}

void
ns_font_init(void)
{
    if (g_entries) return;
#ifdef NS_USE_WEB_TEXT_LIB
    ns_pango_set_emoji_family_filter(ns_font_family_loaded);
#endif
    g_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_pending_by_url = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, ns_font_pending_free);
    const char *xdg = g_getenv("XDG_CACHE_HOME");
    char *base = xdg && *xdg
        ? g_strdup(xdg)
        : g_build_filename(g_get_home_dir(), ".cache", NULL);
    g_cache_dir = g_build_filename(base, "nordstjernen", "webfonts", NULL);
    g_free(base);
    g_mkdir_with_parents(g_cache_dir, 0700);
    ns_paint_register_font_oracle();
}

void
ns_font_shutdown(void)
{
#ifdef NS_HAVE_FONTCONFIG
    g_mutex_lock(&g_family_index_lock);
    g_clear_pointer(&g_family_index.spelling_by_exact, g_hash_table_destroy);
    g_clear_pointer(&g_family_index.exact_by_loose, g_hash_table_destroy);
    g_mutex_unlock(&g_family_index_lock);
#endif
    if (!g_entries) return;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ns_font_entry *e = v;
        if (e->cancel) {
            g_cancellable_cancel(e->cancel);
            g_object_unref(e->cancel);
        }
        g_free(e->family);
        g_free(e->url);
        g_free(e->deferred_path);
        g_free(e);
    }
    g_hash_table_destroy(g_entries);
    g_entries = NULL;
    if (g_pending_by_url) {
        g_hash_table_destroy(g_pending_by_url);
        g_pending_by_url = NULL;
    }
    g_free(g_cache_dir);
    g_cache_dir = NULL;
}

gboolean
ns_font_available(void)
{
#ifdef NS_HAVE_FONTCONFIG
    return TRUE;
#else
    return FALSE;
#endif
}

#ifdef NS_HAVE_FONTCONFIG
static char *
ns_font_family_key(const char *family, gboolean drop_blanks)
{
    char *folded = g_utf8_validate(family, -1, NULL)
        ? g_utf8_casefold(family, -1) : g_ascii_strdown(family, -1);
    if (!drop_blanks) return folded;
    char *w = folded;
    for (const char *r = folded; *r; r++)
        if (*r != ' ') *w++ = *r;
    *w = '\0';
    return folded;
}

static void
ns_font_family_index_add(ns_font_family_index *idx, const FcFontSet *set)
{
    for (int i = 0; set && i < set->nfont; i++) {
        FcBool variable = FcFalse;
        if (FcPatternGetBool(set->fonts[i], FC_VARIABLE, 0, &variable) ==
                FcResultMatch && variable)
            continue;
        FcChar8 *name = NULL;
        for (int j = 0; FcPatternGetString(set->fonts[i], FC_FAMILY, j,
                                           &name) == FcResultMatch; j++) {
            char *exact = ns_font_family_key((const char *)name, FALSE);
            char *loose = ns_font_family_key((const char *)name, TRUE);
            const char *seen = g_hash_table_lookup(idx->exact_by_loose, loose);
            if (!seen) {
                g_hash_table_insert(idx->exact_by_loose, loose, g_strdup(exact));
            } else {
                if (seen != g_family_ambiguous && strcmp(seen, exact) != 0)
                    g_hash_table_insert(idx->exact_by_loose, loose,
                                        (gpointer)g_family_ambiguous);
                else
                    g_free(loose);
            }
            if (!g_hash_table_contains(idx->spelling_by_exact, exact))
                g_hash_table_insert(idx->spelling_by_exact, exact,
                                    g_strdup((const char *)name));
            else
                g_free(exact);
        }
    }
}

static void
ns_font_family_value_free(gpointer value)
{
    if (value != g_family_ambiguous) g_free(value);
}

static ns_font_family_index *
ns_font_family_index_current(void)
{
    ns_font_family_index *idx = &g_family_index;
    FcConfig *config = FcConfigGetCurrent();
    const FcFontSet *system_set = FcConfigGetFonts(config, FcSetSystem);
    const FcFontSet *app_set = FcConfigGetFonts(config, FcSetApplication);
    int system_fonts = system_set ? system_set->nfont : 0;
    int app_fonts = app_set ? app_set->nfont : 0;
    if (idx->spelling_by_exact && idx->config == config &&
        idx->system_fonts == system_fonts && idx->app_fonts == app_fonts &&
        idx->generation == g_font_generation)
        return idx;
    if (!idx->spelling_by_exact) {
        idx->spelling_by_exact = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, g_free);
        idx->exact_by_loose = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, ns_font_family_value_free);
    } else {
        g_hash_table_remove_all(idx->spelling_by_exact);
        g_hash_table_remove_all(idx->exact_by_loose);
    }
    ns_font_family_index_add(idx, system_set);
    ns_font_family_index_add(idx, app_set);
    idx->config = config;
    idx->system_fonts = system_fonts;
    idx->app_fonts = app_fonts;
    idx->generation = g_font_generation;
    return idx;
}

static gboolean
ns_font_pattern_object_below_family(FcPattern *pat, FcPatternIter *iter)
{
    static const char *const below_family[] = {
        "family", "familylang", "stylelang", "fullnamelang", "namelang",
        "lang", "genericfamily", "slant", "weight", "width", "size",
        "pixelsize", "spacing", "symbol", "decorative", "antialias", "order",
        "fontversion", "hinting", "hintstyle", "verticallayout", "autohint",
        "globaladvance", "embeddedbitmap", "dpi", "rgba", "scale",
        "lcdfilter", "prgname",
    };
    const char *object = FcPatternIterGetObject(pat, iter);
    if (!object) return FALSE;
    if (strcmp(object, FC_VARIABLE) == 0) {
        FcValue value;
        for (int i = 0; i < FcPatternIterValueCount(pat, iter); i++)
            if (FcPatternIterGetValue(pat, iter, i, &value, NULL) !=
                    FcResultMatch ||
                value.type != FcTypeBool || value.u.b != FcFalse)
                return FALSE;
        return TRUE;
    }
    for (gsize i = 0; i < G_N_ELEMENTS(below_family); i++)
        if (strcmp(object, below_family[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean
ns_font_pattern_ranks_family_first(FcPattern *pat)
{
    FcPatternIter iter;
    FcPatternIterStart(pat, &iter);
    if (!FcPatternIterIsValid(pat, &iter)) return TRUE;
    do {
        if (!ns_font_pattern_object_below_family(pat, &iter)) return FALSE;
    } while (FcPatternIterNext(pat, &iter));
    return TRUE;
}

static gboolean
ns_font_alias_from_index(FcPattern *pat, int last_bound, char **alias)
{
    if (!ns_font_pattern_ranks_family_first(pat)) return FALSE;
    g_mutex_lock(&g_family_index_lock);
    ns_font_family_index *idx = ns_font_family_index_current();
    gboolean decided = TRUE;
    gboolean weak_present = FALSE;
    FcValue value;
    FcValueBinding binding;
    for (int i = 0; i <= last_bound &&
         FcPatternGetWithBinding(pat, FC_FAMILY, i, &value, &binding) ==
             FcResultMatch; i++) {
        if (value.type != FcTypeString) continue;
        char *loose = ns_font_family_key((const char *)value.u.s, TRUE);
        const char *exact_seen = g_hash_table_lookup(idx->exact_by_loose, loose);
        g_free(loose);
        if (!exact_seen) continue;
        if (binding == FcValueBindingWeak) {
            weak_present = TRUE;
            continue;
        }
        char *exact = ns_font_family_key((const char *)value.u.s, FALSE);
        if (exact_seen != g_family_ambiguous && strcmp(exact_seen, exact) == 0)
            *alias = g_strdup(g_hash_table_lookup(idx->spelling_by_exact, exact));
        else
            decided = FALSE;
        g_free(exact);
        g_mutex_unlock(&g_family_index_lock);
        return decided;
    }
    g_mutex_unlock(&g_family_index_lock);
    return !weak_present;
}
#endif

char *
ns_font_metric_alias(const char *family)
{
#ifdef NS_HAVE_FONTCONFIG
    if (!family || !*family) return NULL;
    FcPattern *pat = FcPatternCreate();
    if (!pat) return NULL;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    int last_bound = -1;
    FcValue value;
    FcValueBinding binding;
    for (int i = 0; FcPatternGetWithBinding(pat, FC_FAMILY, i, &value,
                                            &binding) == FcResultMatch; i++)
        if (binding != FcValueBindingWeak) last_bound = i;
    char *alias = NULL;
    if (ns_font_alias_from_index(pat, last_bound, &alias)) {
        FcPatternDestroy(pat);
        return alias;
    }
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcChar8 *got = NULL;
    for (int j = 0; match && !alias &&
         FcPatternGetString(match, FC_FAMILY, j, &got) == FcResultMatch; j++) {
        for (int i = 0; i <= last_bound; i++) {
            FcChar8 *want = NULL;
            if (FcPatternGetString(pat, FC_FAMILY, i, &want) == FcResultMatch &&
                FcStrCmpIgnoreCase(want, got) == 0) {
                alias = g_strdup((const char *)got);
                break;
            }
        }
    }
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return alias;
#else
    (void)family;
    return NULL;
#endif
}

guint
ns_font_generation(void)
{
    return g_font_generation;
}

gboolean
ns_font_family_loaded(const char *family)
{
    if (!family || !g_entries) return FALSE;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ns_font_entry *e = v;
        if (e->loaded && e->family && strcmp(e->family, family) == 0)
            return TRUE;
    }
    return FALSE;
}

#ifdef NS_HAVE_FONTCONFIG
static gboolean
ns_font_request_prefers_color(const char *family)
{
    FcPattern *pattern = FcPatternCreate();
    FcBool color = FcFalse;
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    if (FcPatternGetBool(pattern, FC_COLOR, 0, &color) != FcResultMatch)
        color = FcFalse;
    FcPatternDestroy(pattern);
    return color == FcTrue;
}
#endif

static char *
ns_font_private_family(const char *family)
{
    return g_strconcat("NS face ", family, NULL);
}

char *
ns_font_family_for_text(const char *family)
{
    if (ns_font_family_is_emoji(family) && ns_font_family_loaded(family))
        return ns_font_private_family(family);
    return g_strdup(family);
}

gboolean
ns_font_family_is_emoji(const char *family)
{
#ifdef NS_HAVE_FONTCONFIG
    static __thread GHashTable *known;
    if (!family || !*family) return FALSE;
    if (!known)
        known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpointer cached = NULL;
    if (g_hash_table_lookup_extended(known, family, NULL, &cached))
        return GPOINTER_TO_INT(cached);
    gboolean emoji = ns_font_request_prefers_color(family);
    if (g_hash_table_size(known) >= 1024) g_hash_table_remove_all(known);
    g_hash_table_insert(known, g_strdup(family), GINT_TO_POINTER(emoji));
    return emoji;
#else
    (void)family;
    return FALSE;
#endif
}

static const char *
ns_font_extension_for(const char *url, char *buf, gsize buflen)
{
    if (!url) return ".bin";
    const char *q = strchr(url, '?');
    gsize end = q ? (gsize)(q - url) : strlen(url);
    const char *frag = memchr(url, '#', end);
    if (frag) end = (gsize)(frag - url);
    gsize i = end;
    while (i > 0 && url[i - 1] != '.' && url[i - 1] != '/') i--;
    if (i == 0 || url[i - 1] != '.') return ".bin";
    gsize len = end - i;
    if (len + 2 > buflen) return ".bin";
    buf[0] = '.';
    for (gsize j = 0; j < len; j++) buf[j + 1] = g_ascii_tolower(url[i + j]);
    buf[len + 1] = '\0';
    return buf;
}

static gboolean
ns_font_bytes_look_like_font(const guint8 *d, gsize len)
{
    if (!d || len < 4) return FALSE;
    static const guint8 sigs[][4] = {
        { 0x00, 0x01, 0x00, 0x00 },
        { 'O', 'T', 'T', 'O' },
        { 't', 'r', 'u', 'e' },
        { 't', 'y', 'p', '1' },
        { 't', 't', 'c', 'f' },
        { 'w', 'O', 'F', 'F' },
        { 'w', 'O', 'F', '2' },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(sigs); i++)
        if (memcmp(d, sigs[i], 4) == 0) return TRUE;
    return FALSE;
}

#ifdef NS_HAVE_FREETYPE
static gboolean
ns_font_bytes_are_woff(const guint8 *d, gsize len)
{
    if (!d || len < 4) return FALSE;
    return d[0] == 'w' && d[1] == 'O' && d[2] == 'F' &&
           (d[3] == 'F' || d[3] == '2');
}

static void
ns_put_be16(guint8 *p, guint16 v) { p[0] = v >> 8; p[1] = (guint8)v; }

static void
ns_put_be32(guint8 *p, guint32 v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (guint8)v;
}

static guint8 *
ns_font_woff_to_sfnt(const guint8 *data, gsize len, gsize *out_len,
                     gboolean *out_cff)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib)) return NULL;
    FT_Face face;
    if (FT_New_Memory_Face(lib, data, (FT_Long)len, 0, &face)) {
        FT_Done_FreeType(lib);
        return NULL;
    }

    typedef struct { FT_ULong tag, len, off; } ns_sfnt_tab;
    guint8 *buf = NULL;
    FT_ULong num = 0;
    if (FT_Sfnt_Table_Info(face, 0, NULL, &num) || num == 0 || num > 4096)
        goto out;

    ns_sfnt_tab *tabs = g_new0(ns_sfnt_tab, num);
    gboolean cff = FALSE, bad = FALSE;
    for (FT_UInt i = 0; i < num; i++) {
        FT_ULong tag = 0, tlen = 0;
        if (FT_Sfnt_Table_Info(face, i, &tag, &tlen)) { bad = TRUE; break; }
        for (FT_UInt j = 0; j < i; j++)
            if (tabs[j].tag == tag) bad = TRUE;
        if (bad) break;
        tabs[i].tag = tag;
        tabs[i].len = tlen;
        if (tag == FT_MAKE_TAG('C', 'F', 'F', ' ')) cff = TRUE;
    }

    if (!bad) {
        guint16 es = 0;
        while ((1u << (es + 1)) <= num) es++;
        guint16 sr = (guint16)((1u << es) * 16);
        guint16 rs = (guint16)(num * 16 - sr);
        gsize off = 12 + (gsize)num * 16;
        const gsize max_sfnt = 64u * 1024u * 1024u;
        gboolean too_big = FALSE;
        for (FT_UInt i = 0; i < num; i++) {
            tabs[i].off = off;
            if (tabs[i].len > max_sfnt) {
                too_big = TRUE;
                break;
            }
            gsize padded = ((gsize)tabs[i].len + 3) & ~(gsize)3;
            if (padded > max_sfnt || off > max_sfnt - padded) {
                too_big = TRUE;
                break;
            }
            off += padded;
        }
        buf = too_big ? NULL : g_try_malloc0(off);
        if (buf) {
            ns_put_be32(buf, cff ? FT_MAKE_TAG('O', 'T', 'T', 'O') : 0x00010000u);
            ns_put_be16(buf + 4, (guint16)num);
            ns_put_be16(buf + 6, sr);
            ns_put_be16(buf + 8, es);
            ns_put_be16(buf + 10, rs);
            for (FT_UInt i = 0; i < num; i++) {
                FT_ULong tlen = tabs[i].len;
                if (FT_Load_Sfnt_Table(face, tabs[i].tag, 0,
                                       buf + tabs[i].off, &tlen)) {
                    g_clear_pointer(&buf, g_free);
                    break;
                }
                guint32 sum = 0;
                gsize padded = (tabs[i].len + 3) & ~(gsize)3;
                for (gsize k = 0; k < padded; k += 4) {
                    const guint8 *q = buf + tabs[i].off + k;
                    sum += ((guint32)q[0] << 24) | ((guint32)q[1] << 16) |
                           ((guint32)q[2] << 8) | q[3];
                }
                guint8 *dir = buf + 12 + (gsize)i * 16;
                ns_put_be32(dir, tabs[i].tag);
                ns_put_be32(dir + 4, sum);
                ns_put_be32(dir + 8, (guint32)tabs[i].off);
                ns_put_be32(dir + 12, (guint32)tabs[i].len);
            }
        }
        if (buf) {
            if (out_len) *out_len = off;
            if (out_cff) *out_cff = cff;
        }
    }
    g_free(tabs);

out:
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return buf;
}
#endif

static char *
ns_font_cache_path_for(const char *family, const char *url,
                       const char *forced_ext)
{
    if (!g_cache_dir || !family) return NULL;
    char *sanitized = g_strdup(family);
    for (char *p = sanitized; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') *p = '_';
    char extbuf[12];
    const char *ext = forced_ext ? forced_ext
                                 : ns_font_extension_for(url, extbuf, sizeof extbuf);
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                 url ? url : "", -1);
    char tag[65];
    g_strlcpy(tag, digest ? digest : "0", sizeof tag);
    g_free(digest);
    char *name = g_strdup_printf("%s-%s%s", sanitized, tag, ext);
    g_free(sanitized);
    char *full = g_build_filename(g_cache_dir, name, NULL);
    g_free(name);
    return full;
}

typedef struct ns_font_fetch_ctx {
    char *url;
} ns_font_fetch_ctx;

#ifdef NS_HAVE_FONTCONFIG
static int
ns_font_fc_slant(ns_font_slant slant)
{
    if (slant == NS_FONT_SLANT_ITALIC) return FC_SLANT_ITALIC;
    if (slant == NS_FONT_SLANT_OBLIQUE) return FC_SLANT_OBLIQUE;
    return FC_SLANT_ROMAN;
}

static gboolean
ns_font_pattern_is_variable(const FcPattern *pat)
{
    FcBool variable = FcFalse;
    return FcPatternGetBool(pat, FC_VARIABLE, 0, &variable) == FcResultMatch &&
           variable;
}

static gboolean
ns_font_pattern_has_weight_axis(const FcPattern *pat)
{
    FcValue weight;
    return FcPatternGet(pat, FC_WEIGHT, 0, &weight) == FcResultMatch &&
           weight.type == FcTypeRange;
}

static int
ns_font_pattern_face_index(const FcPattern *pat)
{
    int index = 0;
    FcPatternGetInteger(pat, FC_INDEX, 0, &index);
    return index & 0xFFFF;
}

static gboolean
ns_font_is_instance_of_variable_face(const FcFontSet *fonts, int first,
                                     const FcPattern *pat)
{
    if (ns_font_pattern_is_variable(pat)) return FALSE;
    int face_index = ns_font_pattern_face_index(pat);
    for (int i = first; i < fonts->nfont; i++) {
        const FcPattern *other = fonts->fonts[i];
        if (other && ns_font_pattern_is_variable(other) &&
            ns_font_pattern_face_index(other) == face_index)
            return TRUE;
    }
    return FALSE;
}

static void
ns_font_set_order(FcPattern *pat, int order)
{
    FcPatternDel(pat, FC_FONTVERSION);
    FcPatternAddInteger(pat, FC_FONTVERSION, order);
}

static void
ns_font_describe_face(FcPattern *pat, const ns_font_entry *face)
{
    if (face->descriptors.weight > 0 && !ns_font_pattern_has_weight_axis(pat)) {
        FcPatternDel(pat, FC_WEIGHT);
        FcPatternAddDouble(pat, FC_WEIGHT,
                           FcWeightFromOpenTypeDouble(face->descriptors.weight));
    }
    if (face->descriptors.slant != NS_FONT_SLANT_AUTO) {
        FcPatternDel(pat, FC_SLANT);
        FcPatternAddInteger(pat, FC_SLANT,
                            ns_font_fc_slant(face->descriptors.slant));
    }
    FcPatternDel(pat, FC_SIZE);
    ns_font_set_order(pat, NS_FONT_FACE_VERSION_BASE + face->declared);
}

static gboolean
ns_font_pattern_named(const FcPattern *pat, const char *family)
{
    FcChar8 *name = NULL;
    return FcPatternGetString(pat, FC_FAMILY, 0, &name) == FcResultMatch &&
           name && g_ascii_strcasecmp((const char *)name, family) == 0;
}

static void
ns_font_add_under_family(FcFontSet *app_fonts, const FcPattern *scanned,
                         const ns_font_entry *face, const char *family)
{
    FcPattern *pat = FcPatternDuplicate(scanned);
    if (!pat) return;
    FcPatternDel(pat, FC_FAMILY);
    FcPatternDel(pat, FC_FAMILYLANG);
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    ns_font_describe_face(pat, face);
    if (!FcFontSetAdd(app_fonts, pat))
        FcPatternDestroy(pat);
}

static void
ns_font_drop_variable_instances(FcFontSet *app_fonts, int first_added)
{
    for (int i = first_added; i < app_fonts->nfont; i++) {
        FcPattern *pat = app_fonts->fonts[i];
        if (ns_font_is_instance_of_variable_face(app_fonts, first_added, pat)) {
            FcPatternDestroy(pat);
            app_fonts->fonts[i] = NULL;
        }
    }
    int kept = first_added;
    for (int i = first_added; i < app_fonts->nfont; i++)
        if (app_fonts->fonts[i]) app_fonts->fonts[kept++] = app_fonts->fonts[i];
    app_fonts->nfont = kept;
}

static void
ns_font_add_private_copies(FcFontSet *app_fonts, int first_added,
                           const ns_font_entry *face)
{
    char *private_family = ns_font_private_family(face->family);
    int added = app_fonts->nfont;
    for (int i = first_added; i < added; i++)
        if (ns_font_pattern_named(app_fonts->fonts[i], face->family))
            ns_font_add_under_family(app_fonts, app_fonts->fonts[i], face,
                                     private_family);
    g_free(private_family);
}

static void
ns_font_describe_added(FcFontSet *app_fonts, int first_added,
                       const ns_font_entry *face)
{
    ns_font_drop_variable_instances(app_fonts, first_added);
    int scanned = app_fonts->nfont;
    for (int i = first_added; i < scanned; i++) {
        FcPattern *pat = app_fonts->fonts[i];
        if (ns_font_pattern_named(pat, face->family)) {
            ns_font_describe_face(pat, face);
        } else {
            ns_font_set_order(pat, face->declared);
            ns_font_add_under_family(app_fonts, pat, face, face->family);
        }
    }
    if (ns_font_family_is_emoji(face->family))
        ns_font_add_private_copies(app_fonts, first_added, face);
}

static void
ns_font_map_add(FcFontSet *app_fonts, int first_added)
{
    (void)app_fonts;
    (void)first_added;
#ifdef NS_HAVE_PANGOFC
    NsPangoFontMap *fm = ns_pango_cairo_font_map_get_default();
    if (!fm || !NS_PANGO_IS_FC_FONT_MAP(fm)) return;
#ifdef NS_USE_WEB_TEXT_LIB
    ns_pango_fc_font_map_add_fonts(NS_PANGO_FC_FONT_MAP(fm), app_fonts,
                                   first_added);
#else
    ns_pango_fc_font_map_config_changed(NS_PANGO_FC_FONT_MAP(fm));
#endif
#endif
}

static void
ns_font_install_file(const char *path, const ns_font_entry *face)
{
    if (!path) return;
    const FcFontSet *before = FcConfigGetFonts(NULL, FcSetApplication);
    int first_added = before ? before->nfont : 0;
    FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
    FcFontSet *app_fonts = FcConfigGetFonts(NULL, FcSetApplication);
    if (app_fonts) ns_font_describe_added(app_fonts, first_added, face);
    ns_font_map_add(app_fonts, first_added);
}
#endif

static guint8 *
ns_font_to_sfnt(const guint8 *data, gsize len, gsize *out_len,
                const char **out_ext)
{
    guint8 *converted = NULL;
    gboolean cff = FALSE;
#ifdef NS_HAVE_FREETYPE
    if (ns_font_bytes_are_woff(data, len))
        converted = ns_font_woff_to_sfnt(data, len, out_len, &cff);
#endif
#ifdef NS_HAVE_WOFF2
    if (!converted && ns_woff2_is_woff2(data, len))
        converted = ns_woff2_to_sfnt(data, len, out_len, &cff);
#endif
    if (converted) *out_ext = cff ? ".otf" : ".ttf";
    return converted;
}

static gboolean
ns_font_slant_is_upright(ns_font_slant slant)
{
    return slant == NS_FONT_SLANT_AUTO || slant == NS_FONT_SLANT_ROMAN;
}

static gboolean
ns_font_upright_face_loading(const ns_font_entry *face)
{
    if (ns_font_slant_is_upright(face->descriptors.slant) || !g_entries)
        return FALSE;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const ns_font_entry *other = v;
        if (other->inflight && !other->loaded &&
            ns_font_slant_is_upright(other->descriptors.slant) &&
            g_strcmp0(other->family, face->family) == 0)
            return TRUE;
    }
    return FALSE;
}

static void
ns_font_activate(ns_font_entry *e, const char *path)
{
#ifdef NS_HAVE_FONTCONFIG
    ns_font_install_file(path, e);
#else
    (void)path;
#endif
    e->loaded = TRUE;
    g_font_generation++;
    if (g_loaded_cb) g_loaded_cb(e->family, g_loaded_ud);
}

static void
ns_font_activate_deferred(void)
{
    if (!g_entries) return;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        ns_font_entry *e = v;
        if (!e->deferred_path || ns_font_upright_face_loading(e)) continue;
        char *path = e->deferred_path;
        e->deferred_path = NULL;
        ns_font_activate(e, path);
        g_free(path);
    }
}

static gboolean
ns_font_store_entry(ns_font_entry *e, const guint8 *data, gsize len)
{
    gsize sfnt_len = 0;
    const char *forced_ext = NULL;
    guint8 *converted = ns_font_to_sfnt(data, len, &sfnt_len, &forced_ext);
    if (converted) {
        data = converted;
        len = sfnt_len;
    }
    char *path = ns_font_cache_path_for(e->family, e->url, forced_ext);
    GError *werr = NULL;
    gboolean stored = path &&
        g_file_set_contents_full(path, (const char *)data, (gssize)len,
                                 G_FILE_SET_CONTENTS_CONSISTENT |
                                 G_FILE_SET_CONTENTS_ONLY_EXISTING, 0600, &werr);
    if (stored) {
        g_free(e->deferred_path);
        e->deferred_path = NULL;
        if (ns_font_upright_face_loading(e))
            e->deferred_path = g_strdup(path);
        else
            ns_font_activate(e, path);
    }
    g_clear_error(&werr);
    g_free(path);
    g_free(converted);
    return stored;
}

gboolean
ns_font_install_bytes(const char *family, const guint8 *data, gsize len,
                      ns_font_descriptors descriptors)
{
    if (!ns_font_available() || !family || !*family || !data || len == 0 ||
        !ns_font_bytes_look_like_font(data, len))
        return FALSE;
    if (!g_entries) ns_font_init();
    char *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, len);
    char *url = g_strconcat("buffer:", digest, NULL);
    g_free(digest);
    char *key = ns_font_entry_key(family, url, descriptors);
    ns_font_entry *e = g_hash_table_lookup(g_entries, key);
    if (e && e->loaded) {
        g_free(key);
        g_free(url);
        return TRUE;
    }
    if (!e) {
        e = g_new0(ns_font_entry, 1);
        e->family = g_strdup(family);
        e->url = url;
        e->descriptors = descriptors;
        e->declared = ++g_faces_declared;
        g_hash_table_insert(g_entries, key, e);
    } else {
        g_free(key);
        g_free(url);
    }
    return ns_font_store_entry(e, data, len);
}

static void
ns_font_on_fetched(GObject *src, GAsyncResult *res, gpointer user_data)
{
    (void)src;
    ns_font_fetch_ctx *ctx = user_data;
    GError *err = NULL;
    ns_response *resp = ns_net_fetch_finish(res, &err);
    ns_font_pending *pending = g_pending_by_url
        ? g_hash_table_lookup(g_pending_by_url, ctx->url) : NULL;
    GPtrArray *keys = pending ? pending->keys : NULL;
    if (keys) {
        for (guint i = 0; i < keys->len; i++) {
            ns_font_entry *e = g_entries
                ? g_hash_table_lookup(g_entries, g_ptr_array_index(keys, i))
                : NULL;
            if (!e) continue;
            g_clear_object(&e->cancel);
            e->inflight = FALSE;
        }
    }
    if (resp && !resp->error && resp->status < 400 &&
        resp->body && resp->body->len > 0 &&
        ns_font_bytes_look_like_font(resp->body->data, resp->body->len)) {
        for (guint i = 0; keys && i < keys->len; i++) {
            ns_font_entry *e = g_entries
                ? g_hash_table_lookup(g_entries, g_ptr_array_index(keys, i))
                : NULL;
            if (e) ns_font_store_entry(e, resp->body->data, resp->body->len);
        }
    }
    if (g_pending_by_url) g_hash_table_remove(g_pending_by_url, ctx->url);
    ns_font_activate_deferred();
    ns_response_free(resp);
    g_clear_error(&err);
    g_free(ctx->url);
    g_free(ctx);
    ns_font_notify_idle();
}

void
ns_font_request(const char *family, const char *src_url, const char *base_url,
                const char *document_url, ns_font_descriptors descriptors)
{
    if (!ns_font_available()) return;
    if (!g_entries) ns_font_init();
    if (!family || !*family || !src_url || !*src_url) return;

    char *abs = base_url ? ns_url_resolve(base_url, src_url) : g_strdup(src_url);
    if (!abs) return;

    char *key = ns_font_entry_key(family, abs, descriptors);
    ns_font_entry *existing = g_hash_table_lookup(g_entries, key);
    if (existing) {
        if (existing->loaded || existing->inflight || existing->deferred_path) {
            g_free(key);
            g_free(abs);
            return;
        }
        g_free(abs);
    } else {
        existing = g_new0(ns_font_entry, 1);
        existing->family = g_strdup(family);
        existing->url = abs;
        existing->descriptors = descriptors;
        existing->declared = ++g_faces_declared;
        g_hash_table_insert(g_entries, g_strdup(key), existing);
    }

    ns_font_pending *pending = g_pending_by_url
        ? g_hash_table_lookup(g_pending_by_url, existing->url) : NULL;
    if (pending) {
        if (g_ptr_array_find_with_equal_func(pending->keys, key, g_str_equal,
                                             NULL))
            g_free(key);
        else
            g_ptr_array_add(pending->keys, key);
        existing->inflight = TRUE;
        g_set_object(&existing->cancel, pending->cancel);
        return;
    }

    pending = g_new0(ns_font_pending, 1);
    pending->keys = g_ptr_array_new_with_free_func(g_free);
    pending->cancel = g_cancellable_new();
    g_ptr_array_add(pending->keys, key);
    if (g_pending_by_url)
        g_hash_table_insert(g_pending_by_url, g_strdup(existing->url), pending);

    existing->inflight = TRUE;
    g_set_object(&existing->cancel, pending->cancel);

    ns_font_fetch_ctx *ctx = g_new0(ns_font_fetch_ctx, 1);
    ctx->url = g_strdup(existing->url);
    ns_net_request_async(
        existing->url, document_url ? document_url : base_url, "GET", NULL, 0,
        NULL,
        ns_net_accept_headers_for(NS_FETCH_DEST_FONT), existing->cancel,
        ns_font_on_fetched, ctx);
}
