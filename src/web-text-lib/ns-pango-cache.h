/* ns-pango
 * ns-pango-cache.h: Control over the caches this fork adds.
 *
 * Copyright (C) 2026 Northstar contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __NS_PANGO_CACHE_H__
#define __NS_PANGO_CACHE_H__

#include <glib.h>
#include <web-text-lib/pango-version-macros.h>

/* What nothing has read since the last time, keeping the working set: what to
 * call when the browser is asked to give memory back but is still displaying
 * the page it cached for.
 */

NS_PANGO_AVAILABLE_IN_ALL
void ns_pango_cache_get_stats (guint64 *hits,
                               guint64 *misses,
                               guint64 *skipped,
                               guint64 *entries);

/* The reasons behind the `skipped' count above, for a diagnostics readout. */

/* The shape cache holds glyphs, keyed on the font; this one holds the unicode
 * break attributes of a paragraph, which depend on nothing but its text. They
 * fill and evict independently, so they are counted apart.
 */
NS_PANGO_AVAILABLE_IN_ALL
void ns_pango_break_cache_stats (guint64 *hits,
                                 guint64 *misses,
                                 guint64 *skipped,
                                 guint64 *entries);

/* And this one holds the items a paragraph was cut into before any of it was
 * shaped, which depend on the text, the attributes and the context but on no
 * font the browser can name.
 */
NS_PANGO_AVAILABLE_IN_ALL
void ns_pango_item_cache_stats  (guint64 *hits,
                                 guint64 *misses,
                                 guint64 *skipped,
                                 guint64 *entries);

/* Emoji runs are shaped with the family "emoji". A family this returns TRUE
 * for, from the run's own family list, is tried first, so a web font that
 * maps an emoji code point to its own glyph keeps it.
 */
typedef gboolean (*NsPangoEmojiFamilyFilter) (const char *family);

NS_PANGO_AVAILABLE_IN_ALL
void ns_pango_set_emoji_family_filter (NsPangoEmojiFamilyFilter filter);

G_END_DECLS

#endif /* __NS_PANGO_CACHE_H__ */
