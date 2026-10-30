/* Pango
 * shape.c: Convert characters into glyphs.
 *
 * Copyright (C) 1999 Red Hat Software
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

#include "config.h"

#include <string.h>
#include <math.h>
#include <glib.h>

#include "pango-impl-utils.h"
#include "pango-glyph.h"

#include "pango-item-private.h"
#include "pango-font-private.h"
#include "ns-shape-cache.h"

#include <hb-ot.h>

/* {{{ Harfbuzz shaping */
/* {{{ Buffer handling */

/* One buffer per thread that shapes, rather than one for the whole process
 * behind a trylock. Upstream's single buffer means every thread but the one
 * holding it creates and destroys a buffer for every run it shapes, and a
 * browser shapes off the main thread -- so on four threads three of them paid an
 * allocation, and the internal arrays HarfBuzz had already grown, per run.
 *
 * The buffer is taken out of the private on acquire and put back on release, so
 * a shape nested inside another on the same thread finds nothing there and makes
 * its own, exactly as it did when it lost the trylock.
 */
static void
destroy_thread_buffer (gpointer data)
{
  hb_buffer_destroy (data);
}

static GPrivate thread_buffer = G_PRIVATE_INIT (destroy_thread_buffer);

static hb_buffer_t *
acquire_buffer (void)
{
  hb_buffer_t *buffer = g_private_get (&thread_buffer);

  if (G_UNLIKELY (buffer == NULL))
    return hb_buffer_create ();

  g_private_set (&thread_buffer, NULL);

  return buffer;
}

static void
release_buffer (hb_buffer_t *buffer)
{
  hb_buffer_reset (buffer);

  if (G_LIKELY (g_private_get (&thread_buffer) == NULL))
    g_private_set (&thread_buffer, buffer);
  else
    hb_buffer_destroy (buffer);
}

/* }}} */
/* {{{ Use NsPangoFont with Harfbuzz */

/* The glyphs we make up -- unknown-glyph boxes and the empty glyph -- lie far
 * outside the glyph space, and HarfBuzz must not see them. Its per-face
 * coverage cache (get_coverage_binary in OT/Layout/Common/Coverage.hh,
 * hb_ot_layout_binary_cache_t) has 14-bit keys and stores every glyph it is
 * asked about with set_unchecked(), which drops the high bits: shaping a space
 * with a font that has none (any icon font) recorded "not covered" for the real
 * glyph whose id matched the low bits -- 0x10000020 lands on glyph 32 -- and
 * every later ligature starting with that glyph stopped forming for as long as
 * the face lived. So HarfBuzz gets a stand-in for each made-up glyph instead: an
 * id just past the font's last glyph, which no lookup covers, and which the
 * advance and extents callbacks below answer for as the made-up glyph would.
 * ns_pango_hb_shape puts the made-up glyphs back. A font too big to leave room
 * for stand-ins under the cache's key range gets the made-up ids as before.
 * The unchecked store arrived in HarfBuzz 11.4.0 and 14.6.0 checks the key
 * again, so only builds against the releases in between carry the stand-ins.
 */
#if HB_VERSION_ATLEAST (11, 4, 0) && !HB_VERSION_ATLEAST (14, 6, 0)
#define NS_PANGO_HB_STAND_INS
#define NS_PANGO_STAND_IN_END (1u << 14)
#endif

typedef struct
{
  NsPangoFont *font;
  hb_font_t *parent;
  NsPangoShowFlags show_flags;
#ifdef NS_PANGO_HB_STAND_INS
  hb_codepoint_t stand_in_base;
  GArray *stand_ins;
  GHashTable *stand_in_index;
#endif
} NsPangoHbShapeContext;

#ifdef NS_PANGO_HB_STAND_INS
static hb_codepoint_t
stand_in_for (NsPangoHbShapeContext *context,
              hb_codepoint_t         made_up)
{
  gpointer index;
  hb_codepoint_t stand_in;

  if (context->stand_ins == NULL)
    {
      context->stand_ins = g_array_new (FALSE, FALSE, sizeof (hb_codepoint_t));
      context->stand_in_index = g_hash_table_new (NULL, NULL);
    }

  if (g_hash_table_lookup_extended (context->stand_in_index,
                                    GUINT_TO_POINTER (made_up), NULL, &index))
    return context->stand_in_base + GPOINTER_TO_UINT (index);

  stand_in = context->stand_in_base + context->stand_ins->len;
  if (stand_in >= NS_PANGO_STAND_IN_END)
    return made_up;

  g_hash_table_insert (context->stand_in_index, GUINT_TO_POINTER (made_up),
                       GUINT_TO_POINTER (context->stand_ins->len));
  g_array_append_val (context->stand_ins, made_up);

  return stand_in;
}

static hb_codepoint_t
made_up_for (const NsPangoHbShapeContext *context,
             hb_codepoint_t               glyph)
{
  if (context->stand_ins != NULL &&
      glyph >= context->stand_in_base &&
      glyph - context->stand_in_base < context->stand_ins->len)
    return g_array_index (context->stand_ins, hb_codepoint_t,
                          glyph - context->stand_in_base);

  return glyph;
}

static void
free_stand_ins (NsPangoHbShapeContext *context)
{
  if (context->stand_ins == NULL)
    return;

  g_array_free (context->stand_ins, TRUE);
  g_hash_table_destroy (context->stand_in_index);
  context->stand_ins = NULL;
  context->stand_in_index = NULL;
}
#else
static inline hb_codepoint_t
made_up_for (const NsPangoHbShapeContext *context G_GNUC_UNUSED,
             hb_codepoint_t               glyph)
{
  return glyph;
}

static inline void
free_stand_ins (NsPangoHbShapeContext *context G_GNUC_UNUSED)
{
}
#endif

static hb_bool_t
ns_pango_nominal_or_unknown_glyph (NsPangoHbShapeContext *context,
                                   hb_codepoint_t         unicode,
                                   hb_codepoint_t        *glyph)
{

  if (context->show_flags != 0)
    {
      if ((context->show_flags & NS_PANGO_SHOW_SPACES) != 0 &&
          g_unichar_type (unicode) == G_UNICODE_SPACE_SEPARATOR)
        {
          /* Replace 0x20 by visible space, since we
           * don't draw a hex box for 0x20
           */
          if (unicode == 0x20)
            unicode = 0x2423;
          else
            {
              *glyph = NS_PANGO_GET_UNKNOWN_GLYPH (unicode);
              return TRUE;
            }
        }

      if ((context->show_flags & NS_PANGO_SHOW_IGNORABLES) != 0 &&
          ns_pango_is_default_ignorable (unicode))
        {
          if (ns_pango_get_ignorable (unicode))
            *glyph = NS_PANGO_GET_UNKNOWN_GLYPH (unicode);
          else
            *glyph = NS_PANGO_GLYPH_EMPTY;
          return TRUE;
        }

      if ((context->show_flags & NS_PANGO_SHOW_LINE_BREAKS) != 0 &&
          unicode == 0x2028)
        {
          /* Always mark LS as unknown. If it ends up
           * at the line end, NsPangoLayout takes care of
           * hiding them, and if they end up in the middle
           * of a line, we are in single paragraph mode
           * and want to show the LS
           */
          *glyph = NS_PANGO_GET_UNKNOWN_GLYPH (unicode);
          return TRUE;
        }
    }

  if (hb_font_get_nominal_glyph (context->parent, unicode, glyph))
    return TRUE;

  /* HarfBuzz knows how to synthesize other spaces from 0x20, so never
   * replace them with unknown glyphs, just tell HarfBuzz that we don't
   * have a glyph.
   *
   * For 0x20, on the other hand, we need to pretend that we have a glyph
   * and rely on our glyph extents code to provide a reasonable width for
   * NS_PANGO_GET_UNKNOWN_WIDTH (0x20).
   */
  if (g_unichar_type (unicode) == G_UNICODE_SPACE_SEPARATOR)
    {
      if (unicode == 0x20)
        {
          *glyph = NS_PANGO_GET_UNKNOWN_GLYPH (0x20);
          return TRUE;
        }

      return FALSE;
    }

  *glyph = NS_PANGO_GET_UNKNOWN_GLYPH (unicode);

  /* We draw our own invalid-Unicode shape, so prevent HarfBuzz
   * from using REPLACEMENT CHARACTER.
   */
  if (unicode > 0x10FFFF)
    return TRUE;

  return FALSE;
}

static hb_bool_t
ns_pango_hb_font_get_nominal_glyph (hb_font_t      *font,
                                 void           *font_data,
                                 hb_codepoint_t  unicode,
                                 hb_codepoint_t *glyph,
                                 void           *user_data G_GNUC_UNUSED)
{
  NsPangoHbShapeContext *context = (NsPangoHbShapeContext *) font_data;
  hb_bool_t found = ns_pango_nominal_or_unknown_glyph (context, unicode, glyph);

#ifdef NS_PANGO_HB_STAND_INS
  if (*glyph > 0xFFFF)
    *glyph = stand_in_for (context, *glyph);
#endif

  return found;
}

static hb_position_t
ns_pango_hb_font_get_glyph_h_advance (hb_font_t      *font,
                                   void           *font_data,
                                   hb_codepoint_t  glyph,
                                   void           *user_data G_GNUC_UNUSED)
{
  NsPangoHbShapeContext *context = (NsPangoHbShapeContext *) font_data;

  glyph = made_up_for (context, glyph);

  if (glyph & NS_PANGO_GLYPH_UNKNOWN_FLAG)
    {
      NsPangoRectangle logical;

      ns_pango_font_get_glyph_extents (context->font, glyph, NULL, &logical);
      return logical.width;
    }

  return hb_font_get_glyph_h_advance (context->parent, glyph);
}

static hb_position_t
ns_pango_hb_font_get_glyph_v_advance (hb_font_t      *font,
                                   void           *font_data,
                                   hb_codepoint_t  glyph,
                                   void           *user_data G_GNUC_UNUSED)
{
  NsPangoHbShapeContext *context = (NsPangoHbShapeContext *) font_data;

  glyph = made_up_for (context, glyph);

  if (glyph & NS_PANGO_GLYPH_UNKNOWN_FLAG)
    {
      NsPangoRectangle logical;

      ns_pango_font_get_glyph_extents (context->font, glyph, NULL, &logical);
      return logical.height;
    }

  return hb_font_get_glyph_v_advance (context->parent, glyph);
}

static hb_bool_t
ns_pango_hb_font_get_glyph_extents (hb_font_t          *font,
                                 void               *font_data,
                                 hb_codepoint_t      glyph,
                                 hb_glyph_extents_t *extents,
                                 void               *user_data G_GNUC_UNUSED)
{
  NsPangoHbShapeContext *context = (NsPangoHbShapeContext *) font_data;

  glyph = made_up_for (context, glyph);

  if (glyph & NS_PANGO_GLYPH_UNKNOWN_FLAG)
    {
      NsPangoRectangle ink;

      ns_pango_font_get_glyph_extents (context->font, glyph, &ink, NULL);

      extents->x_bearing = ink.x;
      extents->y_bearing = ink.y;
      extents->width     = ink.width;
      extents->height    = ink.height;

      return TRUE;
    }

  return hb_font_get_glyph_extents (context->parent, glyph, extents);
}

static hb_font_t *
ns_pango_font_get_hb_font_for_context (NsPangoFont           *font,
                                    NsPangoHbShapeContext *context)
{
  hb_font_t *hb_font;
  static hb_font_funcs_t *funcs;

  hb_font = ns_pango_font_get_hb_font (font);

  if (G_UNLIKELY (g_once_init_enter (&funcs)))
    {
      hb_font_funcs_t *f = hb_font_funcs_create ();

      hb_font_funcs_set_nominal_glyph_func (f, ns_pango_hb_font_get_nominal_glyph, NULL, NULL);
      hb_font_funcs_set_glyph_h_advance_func (f, ns_pango_hb_font_get_glyph_h_advance, NULL, NULL);
      hb_font_funcs_set_glyph_v_advance_func (f, ns_pango_hb_font_get_glyph_v_advance, NULL, NULL);
      hb_font_funcs_set_glyph_extents_func (f, ns_pango_hb_font_get_glyph_extents, NULL, NULL);

      hb_font_funcs_make_immutable (f);
      g_once_init_leave (&funcs, f);
    }

  context->font = font;
  context->parent = hb_font;
#ifdef NS_PANGO_HB_STAND_INS
  context->stand_in_base = hb_face_get_glyph_count (hb_font_get_face (hb_font));
  context->stand_ins = NULL;
  context->stand_in_index = NULL;
#endif

  hb_font = hb_font_create_sub_font (hb_font);
  hb_font_set_funcs (hb_font, funcs, context, NULL);

  return hb_font;
}

/* }}} */
/* {{{ Utilities */

static NsPangoShowFlags
find_show_flags (const NsPangoAnalysis *analysis)
{
  GSList *l;
  NsPangoShowFlags flags = 0;

  for (l = analysis->extra_attrs; l; l = l->next)
    {
      NsPangoAttribute *attr = l->data;

      if (attr->klass->type == NS_PANGO_ATTR_SHOW)
        flags |= ((NsPangoAttrInt*)attr)->value;
    }

  return flags;
}

static NsPangoTextTransform
find_text_transform (const NsPangoAnalysis *analysis)
{
  GSList *l;
  NsPangoTextTransform transform = NS_PANGO_TEXT_TRANSFORM_NONE;

  for (l = analysis->extra_attrs; l; l = l->next)
    {
      NsPangoAttribute *attr = l->data;

      if (attr->klass->type == NS_PANGO_ATTR_TEXT_TRANSFORM)
        transform = (NsPangoTextTransform) ((NsPangoAttrInt*)attr)->value;
    }

  return transform;
}

/* Which hyphen ns_pango_hb_shape will append, decided the same way and from the
 * same font, so that the shape cache can key on it. The nominal-glyph funcs the
 * sub-font installs do not change the answer for either candidate, so the
 * font's own hb_font is enough and no sub-font has to be created to ask.
 */
static NsPangoShapeHyphen
find_hyphen (const NsPangoAnalysis *analysis,
             NsPangoLogAttr        *log_attrs,
             int                    num_chars)
{
  hb_font_t *hb_font;
  hb_codepoint_t glyph;
  NsPangoShapeHyphen hyphen;

  if ((analysis->flags & NS_PANGO_ANALYSIS_FLAG_NEED_HYPHEN) == 0)
    return NS_PANGO_SHAPE_HYPHEN_NONE;

  hb_font = ns_pango_font_get_hb_font (analysis->font);

  if (hb_font_get_nominal_glyph (hb_font, 0x2010, &glyph))
    hyphen = NS_PANGO_SHAPE_HYPHEN_UNICODE;
  else if (hb_font_get_nominal_glyph (hb_font, '-', &glyph))
    hyphen = NS_PANGO_SHAPE_HYPHEN_ASCII;
  else
    hyphen = NS_PANGO_SHAPE_HYPHEN_MISSING;

  if (log_attrs != NULL && log_attrs[num_chars].break_removes_preceding)
    hyphen |= NS_PANGO_SHAPE_HYPHEN_TRIMMED;

  return hyphen;
}

/* Where an item may be cut, and where HarfBuzz says it may not.
 *
 * The cut rule reasons about Unicode -- a space on either side, an ideograph
 * that neither joins nor ligates -- and Unicode does not know what the font
 * does. Liberation Sans, which fontconfig hands out for Arial, kerns the space
 * against the letter that follows it and puts the adjustment on the space, so a
 * piece ending "Type " carries a width that depends on the word after it. The
 * font is the only thing that can answer this, and HarfBuzz asks it for us:
 * a cluster carrying HB_GLYPH_FLAG_UNSAFE_TO_CONCAT is one where changing the
 * text on one side changes the glyphs on the other, which is exactly what a
 * piece stored on its own and served next to different text would do.
 *
 * @starts is the list of candidate cut offsets, ascending; @unsafe[i] is set
 * when the shaper marked the cluster starting at @starts[i].
 */
typedef struct
{
  const guint32 *starts;
  guint          n;
  guint8         unsafe[NS_PANGO_SHAPE_MAX_SEGMENTS];
} CutSafety;

static void
mark_unsafe_cut (CutSafety *cuts,
                 int        cluster)
{
  guint lo = 0, hi = cuts->n;

  if (cluster < 0)
    return;

  while (lo < hi)
    {
      guint mid = (lo + hi) / 2;

      if (cuts->starts[mid] < (guint32) cluster)
        lo = mid + 1;
      else
        hi = mid;
    }

  if (lo < cuts->n && cuts->starts[lo] == (guint32) cluster)
    cuts->unsafe[lo] = TRUE;
}

static void
mark_every_cut_unsafe (CutSafety *cuts)
{
  for (guint i = 0; i < cuts->n; i++)
    cuts->unsafe[i] = TRUE;
}

static gboolean
font_has_color (hb_font_t *font)
{
  hb_face_t *face;

  face = hb_font_get_face (font);

#if HB_VERSION_ATLEAST (7, 0, 0)
  if (hb_ot_color_has_paint (face))
    return TRUE;
#endif

  return hb_ot_color_has_layers (face) ||
         hb_ot_color_has_png (face) ||
         hb_ot_color_has_svg (face);
}

static gboolean
glyph_has_color (hb_font_t      *font,
                 hb_codepoint_t  glyph)
{
  hb_face_t *face;
  hb_blob_t *blob;

  face = hb_font_get_face (font);

#if HB_VERSION_ATLEAST (7, 0, 0)
  if (hb_ot_color_glyph_has_paint (face, glyph))
    return TRUE;
#endif

  if (hb_ot_color_glyph_get_layers (face, glyph, 0, NULL, NULL) > 0)
    return TRUE;

  if (hb_ot_color_has_png (face))
    {
      blob = hb_ot_color_glyph_reference_png (font, glyph);
      if (blob)
        {
          guint length = hb_blob_get_length (blob);
          hb_blob_destroy (blob);
          if (length > 0)
            return TRUE;
        }
    }

  if (hb_ot_color_has_svg (face))
    {
      blob = hb_ot_color_glyph_reference_svg (face, glyph);
      if (blob)
        {
          guint length = hb_blob_get_length (blob);
          hb_blob_destroy (blob);
          if (length > 0)
            return TRUE;
        }
    }

  return FALSE;
}

/* }}} */

static void
ns_pango_hb_shape (const char          *item_text,
                int                  item_length,
                const char          *paragraph_text,
                int                  paragraph_length,
                const NsPangoAnalysis *analysis,
                NsPangoLogAttr        *log_attrs,
                int                  num_chars,
                NsPangoGlyphString    *glyphs,
                NsPangoShapeFlags      flags,
                CutSafety           *cuts)
{
  NsPangoHbShapeContext context = { 0, };
  hb_buffer_flags_t hb_buffer_flags;
  hb_font_t *hb_font;
  hb_buffer_t *hb_buffer;
  hb_direction_t hb_direction;
  hb_glyph_info_t *hb_glyph;
  hb_glyph_position_t *hb_position;
  int last_cluster;
  guint i, num_glyphs;
  unsigned int item_offset = item_text - paragraph_text;
  hb_feature_t features[32];
  unsigned int num_features = 0;
  NsPangoGlyphInfo *infos;
  NsPangoTextTransform transform;
  int hyphen_index;
  gboolean font_is_color;

  g_return_if_fail (analysis != NULL);
  g_return_if_fail (analysis->font != NULL);

  context.show_flags = find_show_flags (analysis);
  hb_font = ns_pango_font_get_hb_font_for_context (analysis->font, &context);
  hb_buffer = acquire_buffer ();

  transform = find_text_transform (analysis);

  hb_direction = NS_PANGO_GRAVITY_IS_VERTICAL (analysis->gravity) ? HB_DIRECTION_TTB : HB_DIRECTION_LTR;
  if (analysis->level % 2)
    hb_direction = HB_DIRECTION_REVERSE (hb_direction);
  if (NS_PANGO_GRAVITY_IS_IMPROPER (analysis->gravity))
    hb_direction = HB_DIRECTION_REVERSE (hb_direction);

  hb_buffer_flags = HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT;

  if (context.show_flags & NS_PANGO_SHOW_IGNORABLES)
    hb_buffer_flags |= HB_BUFFER_FLAG_PRESERVE_DEFAULT_IGNORABLES;

  /* Asked for only when something is going to cut this result up: HarfBuzz
   * does not produce the flag by default because working it out costs
   * something, and nothing but the shape cache reads it.
   */
  if (cuts != NULL)
    hb_buffer_flags |= HB_BUFFER_FLAG_PRODUCE_UNSAFE_TO_CONCAT;

  /* setup buffer */

  hb_buffer_set_direction (hb_buffer, hb_direction);
  hb_buffer_set_script (hb_buffer, (hb_script_t) g_unicode_script_to_iso15924 (analysis->script));
  hb_buffer_set_language (hb_buffer, hb_language_from_string (ns_pango_language_to_string (analysis->language), -1));
  hb_buffer_set_cluster_level (hb_buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
  hb_buffer_set_flags (hb_buffer, hb_buffer_flags);
  hb_buffer_set_invisible_glyph (hb_buffer, NS_PANGO_GLYPH_EMPTY);

  if (analysis->flags & NS_PANGO_ANALYSIS_FLAG_NEED_HYPHEN)
    {
      const char *p = paragraph_text + item_offset + item_length;
      int last_char_len = p - g_utf8_prev_char (p);

      hyphen_index = item_offset + item_length - last_char_len;

      if (log_attrs && log_attrs[num_chars].break_removes_preceding)
        item_length -= last_char_len;
    }

  /* Add pre-context */
  hb_buffer_add_utf8 (hb_buffer, paragraph_text, item_offset, item_offset, 0);

  if (transform == NS_PANGO_TEXT_TRANSFORM_NONE)
    {
      hb_buffer_add_utf8 (hb_buffer, paragraph_text, item_offset + item_length, item_offset, item_length);
    }
  else
    {
      const char *p;
      int i;

      /* Transform the item text according to text transform.
       * Note: we assume text transforms won't cross font boundaries
       */
      for (p = paragraph_text + item_offset, i = 0;
           p < paragraph_text + item_offset + item_length;
           p = g_utf8_next_char (p), i++)
        {
          int index = p - paragraph_text;
          gunichar ch = g_utf8_get_char (p);
          char *str = NULL;

          switch (transform)
            {
            case NS_PANGO_TEXT_TRANSFORM_LOWERCASE:
              if (g_unichar_isalnum (ch))
                str = g_utf8_strdown (p, g_utf8_next_char (p) - p);
              break;

            case NS_PANGO_TEXT_TRANSFORM_UPPERCASE:
              if (g_unichar_isalnum (ch))
                str = g_utf8_strup (p, g_utf8_next_char (p) - p);
              break;

            case NS_PANGO_TEXT_TRANSFORM_CAPITALIZE:
              if (log_attrs && log_attrs[i].is_word_start)
                ch = g_unichar_totitle (ch);
              break;

            case NS_PANGO_TEXT_TRANSFORM_NONE:
            default:
              g_assert_not_reached ();
            }

          if (str)
            {
              for (const char *q = str; *q; q = g_utf8_next_char (q))
                {
                  ch = g_utf8_get_char (q);
                  hb_buffer_add (hb_buffer, ch, index);
                }
              g_free (str);
            }
          else
            hb_buffer_add (hb_buffer, ch, index);
        }
    }

  /* Add post-context */
  hb_buffer_add_utf8 (hb_buffer, paragraph_text, paragraph_length, item_offset + item_length, 0);

  if (analysis->flags & NS_PANGO_ANALYSIS_FLAG_NEED_HYPHEN)
    {
      /* Insert either a Unicode or ASCII hyphen. We may
       * want to look for script-specific hyphens here.
       */
      hb_codepoint_t glyph;

      /* Note: We rely on hb_buffer_add clearing existing post-context */
      if (hb_font_get_nominal_glyph (hb_font, 0x2010, &glyph))
        hb_buffer_add (hb_buffer, 0x2010, hyphen_index);
      else if (hb_font_get_nominal_glyph (hb_font, '-', &glyph))
        hb_buffer_add (hb_buffer, '-', hyphen_index);
    }

  ns_pango_analysis_collect_features (analysis, features, G_N_ELEMENTS (features), &num_features);

  hb_shape (hb_font, hb_buffer, features, num_features);

  if (NS_PANGO_GRAVITY_IS_IMPROPER (analysis->gravity))
    hb_buffer_reverse (hb_buffer);

  /* buffer output */
  num_glyphs = hb_buffer_get_length (hb_buffer);
  hb_glyph = hb_buffer_get_glyph_infos (hb_buffer, NULL);
  ns_pango_glyph_string_set_size (glyphs, num_glyphs);
  infos = glyphs->glyphs;
  last_cluster = -1;
  font_is_color = font_has_color (hb_font);

  for (i = 0; i < num_glyphs; i++)
    {
      infos[i].glyph = made_up_for (&context, hb_glyph->codepoint);
      glyphs->log_clusters[i] = hb_glyph->cluster - item_offset;
      infos[i].attr.is_cluster_start = glyphs->log_clusters[i] != last_cluster;
      infos[i].attr.is_color = font_is_color && glyph_has_color (hb_font, hb_glyph->codepoint);
      if (cuts != NULL &&
          (hb_glyph_info_get_glyph_flags (hb_glyph) & HB_GLYPH_FLAG_UNSAFE_TO_CONCAT) != 0)
        mark_unsafe_cut (cuts, glyphs->log_clusters[i]);
      hb_glyph++;
      last_cluster = glyphs->log_clusters[i];
    }

  hb_position = hb_buffer_get_glyph_positions (hb_buffer, NULL);
  if (NS_PANGO_GRAVITY_IS_VERTICAL (analysis->gravity))
    for (i = 0; i < num_glyphs; i++)
      {
        /* 90 degrees rotation counter-clockwise. */
        infos[i].geometry.width    = - hb_position->y_advance;
        infos[i].geometry.x_offset = - hb_position->y_offset;
        infos[i].geometry.y_offset = - hb_position->x_offset;
        hb_position++;
      }
  else /* horizontal */
    for (i = 0; i < num_glyphs; i++)
      {
        infos[i].geometry.width    =   hb_position->x_advance;
        infos[i].geometry.x_offset =   hb_position->x_offset;
        infos[i].geometry.y_offset = - hb_position->y_offset;
        hb_position++;
      }

  release_buffer (hb_buffer);
  hb_font_destroy (hb_font);
  free_stand_ins (&context);
}

/* }}} */
/* {{{ Fallback shaping */

/* This is not meant to produce reasonable results */

static void
fallback_shape (const char          *text,
                unsigned int         length,
                const NsPangoAnalysis *analysis,
                NsPangoGlyphString    *glyphs)
{
  int n_chars;
  const char *p;
  int cluster = 0;
  int i;

  n_chars = text ? ns_pango_utf8_strlen (text, length) : 0;

  ns_pango_glyph_string_set_size (glyphs, n_chars);

  p = text;
  for (i = 0; i < n_chars; i++)
    {
      gunichar wc;
      NsPangoGlyph glyph;
      NsPangoRectangle logical_rect;

      wc = g_utf8_get_char (p);

      if (g_unichar_type (wc) != G_UNICODE_NON_SPACING_MARK)
        cluster = p - text;

      if (ns_pango_is_zero_width (wc))
        glyph = NS_PANGO_GLYPH_EMPTY;
      else
        glyph = NS_PANGO_GET_UNKNOWN_GLYPH (wc);

      ns_pango_font_get_glyph_extents (analysis->font, glyph, NULL, &logical_rect);

      glyphs->glyphs[i].glyph = glyph;

      glyphs->glyphs[i].geometry.x_offset = 0;
      glyphs->glyphs[i].geometry.y_offset = 0;
      glyphs->glyphs[i].geometry.width = logical_rect.width;

      glyphs->log_clusters[i] = cluster;

      p = g_utf8_next_char (p);
    }

  if (analysis->level & 1)
    ns_pango_glyph_string_reverse_range (glyphs, 0, glyphs->num_glyphs);
}

/*  }}} */
/* {{{ Shaping implementation */

static void
shape_internal_uncached (const char          *item_text,
                      int                  item_length,
                      const char          *paragraph_text,
                      int                  paragraph_length,
                      const NsPangoAnalysis *analysis,
                      NsPangoLogAttr        *log_attrs,
                      int                  num_chars,
                      NsPangoGlyphString    *glyphs,
                      NsPangoShapeFlags      flags,
                      CutSafety           *cuts)
{
  int i;
  int last_cluster;

  glyphs->num_glyphs = 0;

  if (item_length == -1)
    item_length = strlen (item_text);

  if (!paragraph_text)
    {
      paragraph_text = item_text;
      paragraph_length = item_length;
    }
  if (paragraph_length == -1)
    paragraph_length = strlen (paragraph_text);

  g_return_if_fail (paragraph_text <= item_text);
  g_return_if_fail (paragraph_text + paragraph_length >= item_text + item_length);

  if (analysis->font)
    {
      ns_pango_hb_shape (item_text, item_length,
                      paragraph_text, paragraph_length,
                      analysis,
                      log_attrs, num_chars,
                      glyphs, flags, cuts);

      if (G_UNLIKELY (glyphs->num_glyphs == 0))
        {
          /* If a font has been correctly chosen, but no glyphs are output,
           * there's probably something wrong with the font.
           *
           * Trying to be informative, we print out the font description,
           * and the text, but to not flood the terminal with
           * zillions of the message, we set a flag to only err once per
           * font.
           */
          GQuark warned_quark = g_quark_from_static_string ("pango-shape-fail-warned");

          if (!g_object_get_qdata (G_OBJECT (analysis->font), warned_quark))
            {
              NsPangoFontDescription *desc;
              char *font_name;

              desc = ns_pango_font_describe (analysis->font);
              font_name = ns_pango_font_description_to_string (desc);
              ns_pango_font_description_free (desc);

              g_warning ("shaping failure, expect ugly output. font='%s', text='%.*s'",
                         font_name, item_length, item_text);

              g_free (font_name);

              g_object_set_qdata (G_OBJECT (analysis->font), warned_quark,
                                  GINT_TO_POINTER (1));
            }
        }
    }
  else
    glyphs->num_glyphs = 0;

  if (G_UNLIKELY (!glyphs->num_glyphs))
    {
      /* Nothing asked the font, so nothing knows where this may be cut. */
      if (cuts != NULL)
        mark_every_cut_unsafe (cuts);

      fallback_shape (item_text, item_length, analysis, glyphs);
      if (G_UNLIKELY (!glyphs->num_glyphs))
        return;
    }

  /* make sure last_cluster is invalid */
  last_cluster = glyphs->log_clusters[0] - 1;
  for (i = 0; i < glyphs->num_glyphs; i++)
    {
      /* Set glyphs[i].attr.is_cluster_start based on log_clusters[] */
      if (glyphs->log_clusters[i] != last_cluster)
        {
          glyphs->glyphs[i].attr.is_cluster_start = TRUE;
          last_cluster = glyphs->log_clusters[i];
        }
      else
        glyphs->glyphs[i].attr.is_cluster_start = FALSE;


      /* Shift glyph if width is negative, and negate width.
       * This is useful for rotated font matrices and shouldn't
       * harm in normal cases.
       */
      if (glyphs->glyphs[i].geometry.width < 0)
        {
          glyphs->glyphs[i].geometry.width = -glyphs->glyphs[i].geometry.width;
          glyphs->glyphs[i].geometry.x_offset += glyphs->glyphs[i].geometry.width;
        }
    }

  /* Make sure glyphstring direction conforms to analysis->level */
  if (G_UNLIKELY ((analysis->level & 1) &&
                  glyphs->log_clusters[0] < glyphs->log_clusters[glyphs->num_glyphs - 1]))
    {
      g_warning ("Expected RTL run but got LTR. Fixing.");

      /* *Fix* it so we don't crash later */
      ns_pango_glyph_string_reverse_range (glyphs, 0, glyphs->num_glyphs);
    }

  if (flags & NS_PANGO_SHAPE_ROUND_POSITIONS)
    {
      if (analysis->font && ns_pango_font_is_hinted (analysis->font))
        {
          double x_scale_inv, y_scale_inv;
          double x_scale, y_scale;

          ns_pango_font_get_scale_factors (analysis->font, &x_scale_inv, &y_scale_inv);

          if (NS_PANGO_GRAVITY_IS_IMPROPER (analysis->gravity))
            {
              x_scale_inv = -x_scale_inv;
              y_scale_inv = -y_scale_inv;
            }

          x_scale = 1.0 / x_scale_inv;
          y_scale = 1.0 / y_scale_inv;

          if (x_scale == 1.0 && y_scale == 1.0)
            {
              for (i = 0; i < glyphs->num_glyphs; i++)
                glyphs->glyphs[i].geometry.width = NS_PANGO_UNITS_ROUND (glyphs->glyphs[i].geometry.width);
            }
          else
            {
    #if 0
                  if (NS_PANGO_GRAVITY_IS_VERTICAL (analysis->gravity))
                    {
                      /* XXX */
                      double tmp = x_scale;
                      x_scale = y_scale;
                      y_scale = -tmp;
                    }
    #endif
    #define HINT(value, scale_inv, scale) (NS_PANGO_UNITS_ROUND ((int) ((value) * scale)) * scale_inv)
    #define HINT_X(value) HINT ((value), x_scale, x_scale_inv)
    #define HINT_Y(value) HINT ((value), y_scale, y_scale_inv)
              for (i = 0; i < glyphs->num_glyphs; i++)
                {
                  glyphs->glyphs[i].geometry.width    = HINT_X (glyphs->glyphs[i].geometry.width);
                  glyphs->glyphs[i].geometry.x_offset = HINT_X (glyphs->glyphs[i].geometry.x_offset);
                  glyphs->glyphs[i].geometry.y_offset = HINT_Y (glyphs->glyphs[i].geometry.y_offset);
                }
    #undef HINT_Y
    #undef HINT_X
    #undef HINT
            }
        }
      else
        {
          for (i = 0; i < glyphs->num_glyphs; i++)
            {
              glyphs->glyphs[i].geometry.width =
                NS_PANGO_UNITS_ROUND (glyphs->glyphs[i].geometry.width);
              glyphs->glyphs[i].geometry.x_offset =
                NS_PANGO_UNITS_ROUND (glyphs->glyphs[i].geometry.x_offset);
              glyphs->glyphs[i].geometry.y_offset =
                NS_PANGO_UNITS_ROUND (glyphs->glyphs[i].geometry.y_offset);
            }
        }
    }
}

/* HarfBuzz emits a run in visual order, so an item that reads right to left
 * comes back with its last word first. Whichever way round it is, the glyphs
 * of one word stay together -- that is what the cut rule guarantees -- so the
 * pieces need only be walked in the order the shaper put them.
 */
static gboolean
item_runs_backwards (const NsPangoAnalysis *analysis)
{
  return ((analysis->level % 2) != 0) !=
         (NS_PANGO_GRAVITY_IS_IMPROPER (analysis->gravity) != 0);
}

/* Field by field, and not by total advance: two different glyph strings add up
 * to the same width often enough that a width check proves nothing.
 */
static gboolean
glyph_strings_agree (const NsPangoGlyphString *a,
                     const NsPangoGlyphString *b)
{
  if (a->num_glyphs != b->num_glyphs)
    return FALSE;

  for (int i = 0; i < a->num_glyphs; i++)
    {
      const NsPangoGlyphInfo *ga = &a->glyphs[i];
      const NsPangoGlyphInfo *gb = &b->glyphs[i];

      if (ga->glyph != gb->glyph ||
          ga->geometry.width != gb->geometry.width ||
          ga->geometry.x_offset != gb->geometry.x_offset ||
          ga->geometry.y_offset != gb->geometry.y_offset ||
          ga->attr.is_cluster_start != gb->attr.is_cluster_start ||
          ga->attr.is_color != gb->attr.is_color ||
          a->log_clusters[i] != b->log_clusters[i])
        return FALSE;
    }

  return TRUE;
}

/* Everything a piece needs to be looked up or stored, gathered once so that
 * the keys themselves can be built one at a time. A key is a couple of hundred
 * bytes, and an item can hold hundreds of pieces, so an array of them would put
 * a hundred kilobytes on the stack of every thread that shapes.
 */
typedef struct
{
  const char            *item_text;
  int                    item_length;
  const char            *paragraph_text;
  int                    paragraph_length;
  const NsPangoAnalysis *analysis;
  NsPangoShapeFlags      flags;
  guint                  show_flags;
  const hb_feature_t    *features;
  unsigned int           num_features;
  const guint32         *starts;
  guint                  n;
} SegmentPlan;

#define KEY_AT_ITEM_START (1u << 9)
#define KEY_AT_ITEM_END   (1u << 10)
#define KEY_WHOLE_ITEM    (KEY_AT_ITEM_START | KEY_AT_ITEM_END)

static gboolean
segment_key (const SegmentPlan *plan,
             guint              i,
             NsPangoShapeKey   *key)
{
  guint32 start = plan->starts[i];
  int len = (int) ((i + 1 < plan->n ? plan->starts[i + 1]
                                    : (guint32) plan->item_length) - start);
  guint open_edges = (i == 0 ? KEY_AT_ITEM_START : 0) |
                     (i + 1 == plan->n ? KEY_AT_ITEM_END : 0);

  return ns_pango_shape_cache_key_init (key, plan->analysis,
                                        plan->item_text + start, len,
                                        plan->paragraph_text, plan->paragraph_length,
                                        plan->flags, plan->show_flags,
                                        NS_PANGO_TEXT_TRANSFORM_NONE | open_edges,
                                        NS_PANGO_SHAPE_HYPHEN_NONE,
                                        plan->features, plan->num_features);
}

/* Divides a glyph string that was shaped whole along the item's cut points,
 * and stores each piece. Stores nothing at all if the glyphs of a piece are not
 * one unbroken stretch of the string, if a piece drew no glyphs at all, or if
 * the pieces do not lie in the order the shaper laid them down -- all of which
 * the cut rule is there to hold up, and none of which is worth trusting
 * without looking.
 *
 * A piece is stored only when the shaper agreed to both of the cuts that made
 * it. The cuts at the item's own two ends are not the shaper's to judge: what
 * lies beyond them is a different item, and whether the piece may be lifted out
 * of this paragraph at all is what context_independent() decides, per piece, in
 * segment_key(). A piece that touches an end of its item is keyed apart from
 * one that does not, because nothing was kerned across that end: served inside
 * another item, a trailing space would miss the kerning against the word after
 * it.
 *
 * Returns how many pieces it could not store. Zero means the item can be put
 * back together out of the cache; anything else means it cannot, and the caller
 * keeps the item whole as well.
 */
static guint
insert_segments (const SegmentPlan        *plan,
                 const NsPangoGlyphString *glyphs,
                 gboolean                  backwards,
                 const CutSafety          *cuts)
{
  int first[NS_PANGO_SHAPE_MAX_SEGMENTS];
  int count[NS_PANGO_SHAPE_MAX_SEGMENTS];
  guint n = plan->n;
  guint refused = 0;
  int seen = 0;

  for (guint i = 0; i < n; i++)
    first[i] = count[i] = 0;

  for (int g = 0; g < glyphs->num_glyphs; g++)
    {
      int cluster = glyphs->log_clusters[g];
      guint seg;

      if (cluster < 0 || cluster >= plan->item_length)
        return n;

      for (seg = n; seg > 0; seg--)
        if ((guint32) cluster >= plan->starts[seg - 1])
          break;
      if (seg == 0)
        return n;
      seg--;

      if (count[seg] == 0)
        first[seg] = g;
      else if (first[seg] + count[seg] != g)
        return n;
      count[seg]++;
    }

  for (guint i = 0; i < n; i++)
    seen += count[i];
  if (seen != glyphs->num_glyphs)
    return n;

  /* A piece that drew no glyphs at all did not vanish -- its characters were
   * swallowed by a cluster belonging to the piece before it, which is what
   * HarfBuzz does when a ligature spans a cut. That neighbour would then be
   * stored under its own bytes while carrying glyphs for text those bytes do
   * not contain, and serving it beside anything else would draw the swallowed
   * characters twice or not at all. The merged cluster also takes the cut's
   * offset out of the glyph string, so the UNSAFE_TO_CONCAT check below never
   * sees the cut to refuse it.
   *
   * Refusing before anything is stored, rather than counting the empty piece
   * against @refused, is the difference between the item being kept whole and
   * the bad neighbour already being in the table by the time we say so.
   */
  for (guint i = 0; i < n; i++)
    if (count[i] == 0)
      return n;

  for (guint i = 1; i < n; i++)
    {
      guint a = backwards ? n - i : i - 1;
      guint b = backwards ? n - i - 1 : i;

      if (first[a] > first[b])
        return n;
    }

  for (guint i = 0; i < n; i++)
    {
      NsPangoShapeKey key;

      if ((i > 0 && cuts->unsafe[i]) ||
          (i + 1 < n && cuts->unsafe[i + 1]) ||
          !segment_key (plan, i, &key))
        {
          refused++;
          continue;
        }

      ns_pango_shape_cache_insert_range (&key, glyphs,
                                         first[i], count[i], (int) plan->starts[i]);
    }

  return refused;
}

/* The key for the item taken whole, which is what is stored when the item's
 * pieces do not add up to the item -- because the shaper would not let it be
 * cut there, or because a piece cannot be lifted out of this paragraph. Only
 * reached with no transform and no hyphen, which is the condition
 * shape_by_segment() is called under.
 */
static gboolean
whole_item_key (const SegmentPlan *plan,
                NsPangoShapeKey   *key)
{
  return ns_pango_shape_cache_key_init (key, plan->analysis,
                                        plan->item_text, plan->item_length,
                                        plan->paragraph_text, plan->paragraph_length,
                                        plan->flags, plan->show_flags,
                                        NS_PANGO_TEXT_TRANSFORM_NONE | KEY_WHOLE_ITEM,
                                        NS_PANGO_SHAPE_HYPHEN_NONE,
                                        plan->features, plan->num_features);
}

/* Pieces first, because they survive the line breaker cutting the item
 * somewhere else; the item whole as a fallback, because a font that lets
 * nothing be cut still repeats whole paragraphs -- measured, then measured
 * again at another width, then painted.
 */
static gboolean
serve_from_cache (const SegmentPlan  *plan,
                  gboolean            backwards,
                  NsPangoGlyphString *glyphs)
{
  NsPangoShapeKey key;
  guint i;
  int at = 0;

  for (i = 0; i < plan->n; i++)
    {
      guint seg = backwards ? plan->n - 1 - i : i;
      int before = at;

      if (!segment_key (plan, seg, &key))
        break;
      if (!ns_pango_shape_cache_lookup_at (&key, glyphs, at, (int) plan->starts[seg]))
        break;

      at = glyphs->num_glyphs;
      if (at == before)
        break;
    }

  if (i == plan->n)
    return TRUE;

  return whole_item_key (plan, &key) &&
         ns_pango_shape_cache_lookup (&key, glyphs);
}

static void
store_in_cache (const SegmentPlan        *plan,
                const NsPangoGlyphString *glyphs,
                gboolean                  backwards,
                const CutSafety          *cuts)
{
  NsPangoShapeKey key;

  /* Every piece stored means the item can be assembled from them, whatever the
   * line breaker later cuts it down to, and the item whole would only be a
   * second copy of the same glyphs.
   */
  if (insert_segments (plan, glyphs, backwards, cuts) == 0)
    return;

  if (whole_item_key (plan, &key))
    ns_pango_shape_cache_insert (&key, glyphs);
}

static gboolean
shape_by_segment (const char            *item_text,
                  int                    item_length,
                  const char            *paragraph_text,
                  int                    paragraph_length,
                  const NsPangoAnalysis *analysis,
                  NsPangoLogAttr        *log_attrs,
                  int                    num_chars,
                  NsPangoGlyphString    *glyphs,
                  NsPangoShapeFlags      flags,
                  guint                  show_flags,
                  const hb_feature_t    *features,
                  unsigned int           num_features)
{
  guint32 starts[NS_PANGO_SHAPE_MAX_SEGMENTS];
  gboolean backwards = item_runs_backwards (analysis);
  SegmentPlan plan;
  CutSafety cuts;

  plan.item_text = item_text;
  plan.item_length = item_length;
  plan.paragraph_text = paragraph_text;
  plan.paragraph_length = paragraph_length;
  plan.analysis = analysis;
  plan.flags = flags;
  plan.show_flags = show_flags;
  plan.features = features;
  plan.num_features = num_features;
  plan.starts = starts;
  plan.n = ns_pango_shape_cache_split (item_text, item_length,
                                       starts, G_N_ELEMENTS (starts));

  if (plan.n < 2)
    return FALSE;

  cuts.starts = starts;
  cuts.n = plan.n;
  memset (cuts.unsafe, 0, plan.n * sizeof cuts.unsafe[0]);

  /* Verifying serves the item and then shapes it anyway, so that what the cache
   * handed back can be compared against what the shaper says now. The way this
   * cache can be wrong only shows up across paragraphs -- a piece stored beside
   * one word and served beside another -- so comparing a fresh shaping against
   * the entry it was just served from is the whole of the check.
   */
  if (G_UNLIKELY (ns_pango_shape_cache_verifying ()))
    {
      NsPangoGlyphString *served = ns_pango_glyph_string_new ();
      gboolean was_served = serve_from_cache (&plan, backwards, served);

      shape_internal_uncached (item_text, item_length,
                               paragraph_text, paragraph_length,
                               analysis, log_attrs, num_chars, glyphs, flags,
                               &cuts);

      if (was_served && !glyph_strings_agree (served, glyphs))
        g_warning ("shape cache mismatch on '%.*s'", item_length, item_text);

      ns_pango_glyph_string_free (served);

      store_in_cache (&plan, glyphs, backwards, &cuts);

      return TRUE;
    }

  if (serve_from_cache (&plan, backwards, glyphs))
    return TRUE;

  shape_internal_uncached (item_text, item_length,
                           paragraph_text, paragraph_length,
                           analysis, log_attrs, num_chars, glyphs, flags,
                           &cuts);

  store_in_cache (&plan, glyphs, backwards, &cuts);

  return TRUE;
}

static void
ns_pango_shape_internal (const char          *item_text,
                      int                  item_length,
                      const char          *paragraph_text,
                      int                  paragraph_length,
                      const NsPangoAnalysis *analysis,
                      NsPangoLogAttr        *log_attrs,
                      int                  num_chars,
                      NsPangoGlyphString    *glyphs,
                      NsPangoShapeFlags      flags)
{
  NsPangoShapeKey key;
  gboolean cacheable;
  guint show_flags;
  NsPangoTextTransform transform;
  guint transform_key;
  NsPangoShapeHyphen hyphen;
  hb_feature_t features[32];
  unsigned int num_features = 0;

  if (item_length == -1)
    item_length = strlen (item_text);

  if (!paragraph_text)
    {
      paragraph_text = item_text;
      paragraph_length = item_length;
    }
  if (paragraph_length == -1)
    paragraph_length = strlen (paragraph_text);

  if (!ns_pango_caches_enabled () || analysis->font == NULL)
    {
      shape_internal_uncached (item_text, item_length,
                               paragraph_text, paragraph_length,
                               analysis, log_attrs, num_chars, glyphs, flags,
                               NULL);
      return;
    }

  ns_pango_analysis_collect_features (analysis, features,
                                      G_N_ELEMENTS (features), &num_features);

  show_flags = find_show_flags (analysis);
  transform = find_text_transform (analysis);
  hyphen = find_hyphen (analysis, log_attrs, num_chars);

  /* Both of these make an item's shaping depend on where it ends rather than
   * only on what it says: a hyphen is appended to the last piece, and a
   * transform is fed to HarfBuzz character by character with no paragraph
   * around it, capitalising by an offset counted from the item's own start.
   * Neither survives being cut up, so neither is.
   */
  if (transform == NS_PANGO_TEXT_TRANSFORM_NONE &&
      hyphen == NS_PANGO_SHAPE_HYPHEN_NONE &&
      shape_by_segment (item_text, item_length,
                        paragraph_text, paragraph_length,
                        analysis, log_attrs, num_chars, glyphs, flags,
                        show_flags, features, num_features))
    return;

  /* Capitalising reads is_word_start out of the log attrs, and a caller that
   * brought none gets no capitalisation at all. Those two shape differently
   * under the same transform, so the key has to tell them apart.
   */
  transform_key = transform | KEY_WHOLE_ITEM;
  if (transform == NS_PANGO_TEXT_TRANSFORM_CAPITALIZE && log_attrs == NULL)
    transform_key |= 1u << 8;

  cacheable = ns_pango_shape_cache_key_init (&key, analysis, item_text, item_length,
                                             paragraph_text, paragraph_length,
                                             flags, show_flags, transform_key, hyphen,
                                             features, num_features);

  if (cacheable &&
      !ns_pango_shape_cache_verifying () &&
      ns_pango_shape_cache_lookup (&key, glyphs))
    return;

  shape_internal_uncached (item_text, item_length,
                           paragraph_text, paragraph_length,
                           analysis, log_attrs, num_chars, glyphs, flags,
                           NULL);

  if (!cacheable)
    return;

  if (ns_pango_shape_cache_verifying () &&
      !ns_pango_shape_cache_matches (&key, glyphs))
    g_warning ("shape cache mismatch on '%.*s'", item_length, item_text);

  ns_pango_shape_cache_insert (&key, glyphs);
}

/* }}} */
/* {{{ Public API */

/**
 * ns_pango_shape_full:
 * @item_text: valid UTF-8 text to shape.
 * @item_length: the length (in bytes) of @item_text. -1 means nul-terminated text.
 * @paragraph_text: (nullable): text of the paragraph (see details).
 * @paragraph_length: the length (in bytes) of @paragraph_text. -1 means nul-terminated text.
 * @analysis: `NsPangoAnalysis` structure from [func@Pango.itemize].
 * @glyphs: (out caller-allocates): glyph string in which to store results.
 *
 * Convert the characters in @text into glyphs.
 *
 * Given a segment of text and the corresponding `NsPangoAnalysis` structure
 * returned from [func@Pango.itemize], convert the characters into glyphs.
 * You may also pass in only a substring of the item from [func@Pango.itemize].
 *
 * This is similar to [func@Pango.shape], except it also can optionally take
 * the full paragraph text as input, which will then be used to perform
 * certain cross-item shaping interactions. If you have access to the broader
 * text of which @item_text is part of, provide the broader text as
 * @paragraph_text. If @paragraph_text is %NULL, item text is used instead.
 *
 * Some aspects of hyphen insertion and text transformation (in particular,
 * capitalization) require log attrs, and thus can only be handled by
 * [func@Pango.shape_item].
 *
 * Note that the extra attributes in the @analyis that is returned from
 * [func@Pango.itemize] have indices that are relative to the entire paragraph,
 * so you do not pass the full paragraph text as @paragraph_text, you need
 * to subtract the item offset from their indices before calling
 * [func@Pango.shape_full].
 *
 * Since: 1.32
 */
void
ns_pango_shape_full (const char          *item_text,
                  int                  item_length,
                  const char          *paragraph_text,
                  int                  paragraph_length,
                  const NsPangoAnalysis *analysis,
                  NsPangoGlyphString    *glyphs)
{
  ns_pango_shape_with_flags (item_text, item_length,
                          paragraph_text, paragraph_length,
                          analysis,
                          glyphs,
                          NS_PANGO_SHAPE_NONE);
}

/**
 * ns_pango_shape_with_flags:
 * @item_text: valid UTF-8 text to shape
 * @item_length: the length (in bytes) of @item_text.
 *     -1 means nul-terminated text.
 * @paragraph_text: (nullable): text of the paragraph (see details).
 * @paragraph_length: the length (in bytes) of @paragraph_text.
 *     -1 means nul-terminated text.
 * @analysis:  `NsPangoAnalysis` structure from [func@Pango.itemize]
 * @glyphs: (out caller-allocates): glyph string in which to store results
 * @flags: flags influencing the shaping process
 *
 * Convert the characters in @text into glyphs.
 *
 * Given a segment of text and the corresponding `NsPangoAnalysis` structure
 * returned from [func@Pango.itemize], convert the characters into glyphs.
 * You may also pass in only a substring of the item from [func@Pango.itemize].
 *
 * This is similar to [func@Pango.shape_full], except it also takes flags
 * that can influence the shaping process.
 *
 * Some aspects of hyphen insertion and text transformation (in particular,
 * capitalization) require log attrs, and thus can only be handled by
 * [func@Pango.shape_item].
 *
 * Note that the extra attributes in the @analyis that is returned from
 * [func@Pango.itemize] have indices that are relative to the entire paragraph,
 * so you do not pass the full paragraph text as @paragraph_text, you need
 * to subtract the item offset from their indices before calling
 * [func@Pango.shape_with_flags].
 *
 * Since: 1.44
 */
void
ns_pango_shape_with_flags (const char          *item_text,
                        int                  item_length,
                        const char          *paragraph_text,
                        int                  paragraph_length,
                        const NsPangoAnalysis *analysis,
                        NsPangoGlyphString    *glyphs,
                        NsPangoShapeFlags      flags)
{
  ns_pango_shape_internal (item_text, item_length,
                        paragraph_text, paragraph_length,
                        analysis, NULL, 0,
                        glyphs, flags);
}

/**
 * ns_pango_shape_item:
 * @item: `NsPangoItem` to shape
 * @paragraph_text: (nullable): text of the paragraph (see details).
 * @paragraph_length: the length (in bytes) of @paragraph_text.
 *     -1 means nul-terminated text.
 * @log_attrs: (nullable): array of `NsPangoLogAttr` for @item
 * @glyphs: (out caller-allocates): glyph string in which to store results
 * @flags: flags influencing the shaping process
 *
 * Convert the characters in @item into glyphs.
 *
 * This is similar to [func@Pango.shape_with_flags], except it takes a
 * `NsPangoItem` instead of separate @item_text and @analysis arguments.
 *
 * It also takes @log_attrs, which are needed for implementing some aspects
 * of hyphen insertion and text transforms (in particular, capitalization).
 *
 * Note that the extra attributes in the @analyis that is returned from
 * [func@Pango.itemize] have indices that are relative to the entire paragraph,
 * so you do not pass the full paragraph text as @paragraph_text, you need
 * to subtract the item offset from their indices before calling
 * [func@Pango.shape_with_flags].
 *
 * Since: 1.50
 */
void
ns_pango_shape_item (NsPangoItem        *item,
                  const char       *paragraph_text,
                  int               paragraph_length,
                  NsPangoLogAttr     *log_attrs,
                  NsPangoGlyphString *glyphs,
                  NsPangoShapeFlags   flags)
{
  ns_pango_shape_internal (paragraph_text + item->offset, item->length,
                        paragraph_text, paragraph_length,
                        &item->analysis,
                        log_attrs, item->num_chars,
                        glyphs, flags);
}

/* }}} */

/* vim:set foldmethod=marker expandtab: */
