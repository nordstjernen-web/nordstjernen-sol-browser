/* Pango
 * pangofc-font.c: Shared interfaces for fontconfig-based backends
 *
 * Copyright (C) 2003 Red Hat Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include "pango-font-private.h"
#include "pangofc-font-private.h"
#include "pangofc-fontmap.h"
#include "pangofc-fontmap-private.h"
#include "pangofc-private.h"
#include "pango-layout.h"
#include "pango-impl-utils.h"

#include <hb-ot.h>

enum {
  PROP_0,
  PROP_PATTERN,
  PROP_FONTMAP
};

typedef struct _PangoFcFontPrivate NsPangoFcFontPrivate;

struct _PangoFcFontPrivate
{
  NsPangoFcDecoder *decoder;
  NsPangoFcFontKey *key;
  NsPangoFontFace *face;
};

static gboolean ns_pango_fc_font_real_has_char  (NsPangoFcFont *font,
					      gunichar     wc);
static guint    ns_pango_fc_font_real_get_glyph (NsPangoFcFont *font,
					      gunichar     wc);

static void                  ns_pango_fc_font_finalize     (GObject          *object);
static void                  ns_pango_fc_font_set_property (GObject          *object,
							 guint             prop_id,
							 const GValue     *value,
							 GParamSpec       *pspec);
static void                  ns_pango_fc_font_get_property (GObject          *object,
							 guint             prop_id,
							 GValue           *value,
							 GParamSpec       *pspec);
static NsPangoCoverage *       ns_pango_fc_font_get_coverage (NsPangoFont        *font,
							 NsPangoLanguage    *language);
static NsPangoFontMetrics *    ns_pango_fc_font_get_metrics  (NsPangoFont        *font,
							 NsPangoLanguage    *language);
static NsPangoFontMap *        ns_pango_fc_font_get_font_map (NsPangoFont        *font);
static NsPangoFontDescription *ns_pango_fc_font_describe     (NsPangoFont        *font);
static NsPangoFontDescription *ns_pango_fc_font_describe_absolute (NsPangoFont        *font);
static void                  ns_pango_fc_font_get_features (NsPangoFont        *font,
                                                         hb_feature_t     *features,
                                                         guint             len,
                                                         guint            *num_features);
static hb_font_t *           ns_pango_fc_font_create_hb_font (NsPangoFont        *font);
static NsPangoLanguage **     _ns_pango_fc_font_get_languages  (NsPangoFont        *font);
static gboolean             _ns_pango_fc_font_is_hinted      (NsPangoFont        *font);
static void                 _ns_pango_fc_font_get_scale_factors (NsPangoFont     *font,
                                                              double        *x_scale,
                                                              double        *y_scale);
static void                 ns_pango_fc_font_get_matrix      (NsPangoFont        *font,
                                                           NsPangoMatrix      *matrix);
static int                  ns_pango_fc_font_get_absolute_size (NsPangoFont        *font);
static NsPangoVariant         ns_pango_fc_font_get_variant       (NsPangoFont        *font);
static NsPangoFontFace *      ns_pango_fc_font_get_face          (NsPangoFont        *font);


G_DEFINE_ABSTRACT_TYPE_WITH_CODE (NsPangoFcFont, ns_pango_fc_font, NS_TYPE_PANGO_FONT,
                                  G_ADD_PRIVATE (NsPangoFcFont))

static void
ns_pango_fc_font_class_init (NsPangoFcFontClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  NsPangoFontClass *font_class = NS_PANGO_FONT_CLASS (class);
  NsPangoFontClassPrivate *pclass;

  class->has_char = ns_pango_fc_font_real_has_char;
  class->get_glyph = ns_pango_fc_font_real_get_glyph;
  class->get_unknown_glyph = NULL;

  object_class->finalize = ns_pango_fc_font_finalize;
  object_class->set_property = ns_pango_fc_font_set_property;
  object_class->get_property = ns_pango_fc_font_get_property;
  font_class->describe = ns_pango_fc_font_describe;
  font_class->describe_absolute = ns_pango_fc_font_describe_absolute;
  font_class->get_coverage = ns_pango_fc_font_get_coverage;
  font_class->get_metrics = ns_pango_fc_font_get_metrics;
  font_class->get_font_map = ns_pango_fc_font_get_font_map;
  font_class->get_features = ns_pango_fc_font_get_features;
  font_class->create_hb_font = ns_pango_fc_font_create_hb_font;

  pclass = g_type_class_get_private ((GTypeClass *) class, NS_TYPE_PANGO_FONT);

  pclass->get_languages = _ns_pango_fc_font_get_languages;
  pclass->is_hinted = _ns_pango_fc_font_is_hinted;
  pclass->get_scale_factors = _ns_pango_fc_font_get_scale_factors;
  pclass->get_matrix = ns_pango_fc_font_get_matrix;
  pclass->get_absolute_size = ns_pango_fc_font_get_absolute_size;
  pclass->get_variant = ns_pango_fc_font_get_variant;
  pclass->get_face = ns_pango_fc_font_get_face;

  /**
   * NsPangoFcFont:pattern:
   *
   * The fontconfig pattern for this font.
   */
  g_object_class_install_property (object_class, PROP_PATTERN,
				   g_param_spec_pointer ("pattern",
							 "Pattern",
							 "The fontconfig pattern for this font",
							 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							 G_PARAM_STATIC_STRINGS));

  /**
   * NsPangoFcFont:fontmap:
   *
   * The NsPangoFc font map this font is associated with.
   */
  g_object_class_install_property (object_class, PROP_FONTMAP,
				   g_param_spec_object ("fontmap",
							"Font Map",
							"The NsPangoFc font map this font is associated with (Since: 1.26)",
							NS_TYPE_PANGO_FC_FONT_MAP,
							G_PARAM_READWRITE |
							G_PARAM_STATIC_STRINGS));
}

static void
ns_pango_fc_font_init (NsPangoFcFont *fcfont)
{
  fcfont->priv = ns_pango_fc_font_get_instance_private (fcfont);
}

static void
free_metrics_info (NsPangoFcMetricsInfo *info)
{
  ns_pango_font_metrics_unref (info->metrics);
  g_slice_free (NsPangoFcMetricsInfo, info);
}

static void
ns_pango_fc_font_finalize (GObject *object)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (object);
  NsPangoFcFontPrivate *priv = fcfont->priv;
  NsPangoFontMap *fontmap;

  g_slist_foreach (fcfont->metrics_by_lang, (GFunc)free_metrics_info, NULL);
  g_slist_free (fcfont->metrics_by_lang);

  fontmap = fcfont->fontmap;
  if (fontmap)
    {
      g_object_remove_weak_pointer (G_OBJECT (fontmap), (gpointer *) &fcfont->fontmap);
      _ns_pango_fc_font_map_remove (NS_PANGO_FC_FONT_MAP (fontmap), fcfont);
    }

  ns_pango_font_description_free (fcfont->description);
  FcPatternDestroy (fcfont->font_pattern);

  if (priv->decoder)
    _ns_pango_fc_font_set_decoder (fcfont, NULL);

  g_clear_object (&priv->face);

  G_OBJECT_CLASS (ns_pango_fc_font_parent_class)->finalize (object);
}

static gboolean
pattern_is_hinted (FcPattern *pattern)
{
  FcBool hinting;

  if (FcPatternGetBool (pattern,
			FC_HINTING, 0, &hinting) != FcResultMatch)
    hinting = FcTrue;

  return hinting;
}

static gboolean
pattern_is_transformed (FcPattern *pattern)
{
  FcMatrix *fc_matrix;

  if (FcPatternGetMatrix (pattern, FC_MATRIX, 0, &fc_matrix) == FcResultMatch)
    {
      return fc_matrix->xx != 1 || fc_matrix->xy != 0 ||
             fc_matrix->yx != 0 || fc_matrix->yy != 1;
    }
  else
    return FALSE;
}

static void
ns_pango_fc_font_set_property (GObject       *object,
                            guint          prop_id,
                            const GValue  *value,
                            GParamSpec    *pspec)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (object);

  switch (prop_id)
    {
    case PROP_PATTERN:
      {
        FcPattern *pattern = g_value_get_pointer (value);

        g_return_if_fail (pattern != NULL);
        g_return_if_fail (fcfont->font_pattern == NULL);

        FcPatternReference (pattern);
        fcfont->font_pattern = pattern;
        fcfont->description = font_description_from_pattern (pattern, TRUE, TRUE);
        fcfont->is_hinted = pattern_is_hinted (pattern);
        fcfont->is_transformed = pattern_is_transformed (pattern);
      }
      goto set_decoder;

    case PROP_FONTMAP:
      {
        NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (g_value_get_object (value));

        g_return_if_fail (fcfont->fontmap == NULL);

        fcfont->fontmap = (NsPangoFontMap *) fcfontmap;
        if (fcfontmap)
          g_object_add_weak_pointer (G_OBJECT (fcfontmap), (gpointer *) &fcfont->fontmap);
      }
      goto set_decoder;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
    }

set_decoder:
  /* set decoder if both pattern and fontmap are set now */
  if (fcfont->font_pattern && fcfont->fontmap)
    _ns_pango_fc_font_set_decoder (fcfont,
				ns_pango_fc_font_map_find_decoder  ((NsPangoFcFontMap *) fcfont->fontmap,
								 fcfont->font_pattern));
}

static void
ns_pango_fc_font_get_property (GObject       *object,
			    guint          prop_id,
			    GValue        *value,
			    GParamSpec    *pspec)
{
  switch (prop_id)
    {
    case PROP_PATTERN:
      {
	NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (object);
	g_value_set_pointer (value, fcfont->font_pattern);
      }
      break;
    case PROP_FONTMAP:
      {
	NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (object);

	g_value_set_object (value, fcfont->fontmap);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static NsPangoFontDescription *
ns_pango_fc_font_describe (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = (NsPangoFcFont *)font;

  return ns_pango_font_description_copy (fcfont->description);
}

static NsPangoFontDescription *
ns_pango_fc_font_describe_absolute (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = (NsPangoFcFont *)font;
  NsPangoFontDescription *desc = ns_pango_font_description_copy (fcfont->description);
  double size;

  if (FcPatternGetDouble (fcfont->font_pattern, FC_PIXEL_SIZE, 0, &size) == FcResultMatch)
    ns_pango_font_description_set_absolute_size (desc, size * NS_PANGO_SCALE);

  return desc;
}

static int
ns_pango_fc_font_get_absolute_size (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = (NsPangoFcFont *)font;
  double size;

  if (FcPatternGetDouble (fcfont->font_pattern, FC_PIXEL_SIZE, 0, &size) == FcResultMatch)
    return (int) (size * NS_PANGO_SCALE);

  return 0;
}

static NsPangoVariant
ns_pango_fc_font_get_variant (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = (NsPangoFcFont *)font;
  return ns_pango_font_description_get_variant (fcfont->description);
}

static NsPangoCoverage *
ns_pango_fc_font_get_coverage (NsPangoFont     *font,
			    NsPangoLanguage *language G_GNUC_UNUSED)
{
  NsPangoFcFont *fcfont = (NsPangoFcFont *)font;
  NsPangoFcFontPrivate *priv = fcfont->priv;
  FcCharSet *charset;

  if (priv->decoder)
    {
      charset = ns_pango_fc_decoder_get_charset (priv->decoder, fcfont);
      return _ns_pango_fc_font_map_fc_to_coverage (charset);
    }

  if (!fcfont->fontmap)
    return ns_pango_coverage_new ();

  return _ns_pango_fc_font_map_get_coverage (NS_PANGO_FC_FONT_MAP (fcfont->fontmap), fcfont);
}

/* For Xft, it would be slightly more efficient to simply to
 * call Xft, and also more robust against changes in Xft.
 * But for now, we simply use the same code for all backends.
 *
 * The code in this function is partly based on code from Xft,
 * Copyright 2000 Keith Packard
 */
static void
get_face_metrics (NsPangoFcFont      *fcfont,
                  NsPangoFontMetrics *metrics)
{
  hb_font_t *hb_font = ns_pango_font_get_hb_font (NS_PANGO_FONT (fcfont));
  hb_font_extents_t extents;
  hb_position_t position;

  FcMatrix *fc_matrix;
  gboolean have_transform = FALSE;

  hb_font_get_extents_for_direction (hb_font, HB_DIRECTION_LTR, &extents);

  if  (FcPatternGetMatrix (fcfont->font_pattern,
                           FC_MATRIX, 0, &fc_matrix) == FcResultMatch)
    {
      have_transform = (fc_matrix->xx != 1 || fc_matrix->xy != 0 ||
                        fc_matrix->yx != 0 || fc_matrix->yy != 1);
    }

  if (have_transform)
    {
      metrics->descent =  - extents.descender * fc_matrix->yy;
      metrics->ascent = extents.ascender * fc_matrix->yy;
      metrics->height = (extents.ascender - extents.descender + extents.line_gap) * fc_matrix->yy;
    }
  else
    {
      metrics->descent = - extents.descender;
      metrics->ascent = extents.ascender;
      metrics->height = extents.ascender - extents.descender + extents.line_gap;
    }

  if (hb_ot_metrics_get_position (hb_font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &position) &&
      position != 0)
    metrics->underline_thickness = position;
  else
    metrics->underline_thickness = NS_PANGO_SCALE;

  if (hb_ot_metrics_get_position (hb_font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &position) &&
      position != 0)
    metrics->underline_position = position;
  else
    metrics->underline_position = - NS_PANGO_SCALE;

  if (hb_ot_metrics_get_position (hb_font, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &position) &&
      position != 0)
    metrics->strikethrough_thickness = position;
  else
    metrics->strikethrough_thickness = NS_PANGO_SCALE;

  if (hb_ot_metrics_get_position (hb_font, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &position) &&
      position != 0)
    metrics->strikethrough_position = position;
  else
    metrics->strikethrough_position = metrics->ascent / 2;
}

NsPangoFontMetrics *
ns_pango_fc_font_create_base_metrics_for_context (NsPangoFcFont   *fcfont,
					       NsPangoContext  *context)
{
  NsPangoFontMetrics *metrics;
  metrics = ns_pango_font_metrics_new ();

  get_face_metrics (fcfont, metrics);

  return metrics;
}

static int
max_glyph_width (NsPangoLayout *layout)
{
  int max_width = 0;
  GSList *l, *r;

  for (l = ns_pango_layout_get_lines_readonly (layout); l; l = l->next)
    {
      NsPangoLayoutLine *line = l->data;

      for (r = line->runs; r; r = r->next)
	{
	  NsPangoGlyphString *glyphs = ((NsPangoGlyphItem *)r->data)->glyphs;
	  int i;

	  for (i = 0; i < glyphs->num_glyphs; i++)
	    if (glyphs->glyphs[i].geometry.width > max_width)
	      max_width = glyphs->glyphs[i].geometry.width;
	}
    }

  return max_width;
}

static NsPangoFontMetrics *
ns_pango_fc_font_get_metrics (NsPangoFont     *font,
			   NsPangoLanguage *language)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);
  NsPangoFcMetricsInfo *info = NULL; /* Quiet gcc */
  GSList *tmp_list;
  static int in_get_metrics;

  const char *sample_str = ns_pango_language_get_sample_string (language);

  tmp_list = fcfont->metrics_by_lang;
  while (tmp_list)
    {
      info = tmp_list->data;

      if (info->sample_str == sample_str)    /* We _don't_ need strcmp */
	break;

      tmp_list = tmp_list->next;
    }

  if (!tmp_list)
    {
      NsPangoFontMap *fontmap;
      NsPangoContext *context;

      fontmap = fcfont->fontmap;
      if (!fontmap)
	return ns_pango_font_metrics_new ();

      info = g_slice_new0 (NsPangoFcMetricsInfo);

      /* Note: we need to add info to the list before calling
       * into NsPangoLayout below, to prevent recursion
       */
      fcfont->metrics_by_lang = g_slist_prepend (fcfont->metrics_by_lang,
						 info);

      info->sample_str = sample_str;

      context = ns_pango_font_map_create_context (fontmap);
      ns_pango_context_set_language (context, language);

      info->metrics = ns_pango_fc_font_create_base_metrics_for_context (fcfont, context);

      if (!in_get_metrics)
        {
          /* Compute derived metrics */
          NsPangoLayout *layout;
          NsPangoRectangle extents;
          NsPangoFontDescription *desc = ns_pango_font_describe_with_absolute_size (font);
          gulong sample_str_width;

          in_get_metrics = 1;

          layout = ns_pango_layout_new (context);
          ns_pango_layout_set_font_description (layout, desc);
          ns_pango_font_description_free (desc);

          ns_pango_layout_set_text (layout, sample_str, -1);
          ns_pango_layout_get_extents (layout, NULL, &extents);

          sample_str_width = ns_pango_utf8_strwidth (sample_str);
          g_assert (sample_str_width > 0);
          info->metrics->approximate_char_width = extents.width / sample_str_width;

          ns_pango_layout_set_text (layout, "0123456789", -1);
          info->metrics->approximate_digit_width = max_glyph_width (layout);

          g_object_unref (layout);

          in_get_metrics = 0;
        }

      g_object_unref (context);
    }

  return ns_pango_font_metrics_ref (info->metrics);
}

static NsPangoFontMap *
ns_pango_fc_font_get_font_map (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);

  /* MT-unsafe.  Oh well...  The API is unsafe. */
  return fcfont->fontmap;
}

static gboolean
ns_pango_fc_font_real_has_char (NsPangoFcFont *font,
			     gunichar     wc)
{
  FcCharSet *charset;

  if (FcPatternGetCharSet (font->font_pattern,
			   FC_CHARSET, 0, &charset) != FcResultMatch)
    return FALSE;

  return FcCharSetHasChar (charset, wc);
}

static guint
ns_pango_fc_font_real_get_glyph (NsPangoFcFont *font,
			      gunichar     wc)
{
  hb_font_t *hb_font = ns_pango_font_get_hb_font (NS_PANGO_FONT (font));
  hb_codepoint_t glyph = NS_PANGO_GET_UNKNOWN_GLYPH (wc);

  hb_font_get_nominal_glyph (hb_font, wc, &glyph);

  return glyph;
}


void
_ns_pango_fc_font_shutdown (NsPangoFcFont *font)
{
  g_return_if_fail (NS_PANGO_IS_FC_FONT (font));

  if (NS_PANGO_FC_FONT_GET_CLASS (font)->shutdown)
    NS_PANGO_FC_FONT_GET_CLASS (font)->shutdown (font);
}

/**
 * _ns_pango_fc_font_set_decoder:
 * @font: a `NsPangoFcFont`
 * @decoder: a `NsPangoFcDecoder` to set for this font
 *
 * This sets a custom decoder for this font.
 *
 * Any previous decoder will be released before this one is set.
 *
 * Since: 1.6
 */
void
_ns_pango_fc_font_set_decoder (NsPangoFcFont    *font,
			    NsPangoFcDecoder *decoder)
{
  NsPangoFcFontPrivate *priv = font->priv;

  if (priv->decoder)
    g_object_unref (priv->decoder);

  priv->decoder = decoder;

  if (priv->decoder)
    g_object_ref (priv->decoder);
}

NsPangoFcFontKey *
_ns_pango_fc_font_get_font_key (NsPangoFcFont *fcfont)
{
  NsPangoFcFontPrivate *priv = fcfont->priv;

  return priv->key;
}

void
_ns_pango_fc_font_set_font_key (NsPangoFcFont    *fcfont,
			     NsPangoFcFontKey *key)
{
  NsPangoFcFontPrivate *priv = fcfont->priv;

  priv->key = key;
}

static void
ns_pango_fc_font_get_features (NsPangoFont    *font,
                            hb_feature_t *features,
                            guint         len,
                            guint        *num_features)
{
  /* Setup features from fontconfig pattern. */
  NsPangoFcFont *fc_font = NS_PANGO_FC_FONT (font);
  if (fc_font->font_pattern)
    {
      char *s;
      while (*num_features < len &&
             FcResultMatch == FcPatternGetString (fc_font->font_pattern,
                                                  FC_FONT_FEATURES,
                                                  *num_features,
                                                  (FcChar8 **) &s))
        {
          gboolean ret = hb_feature_from_string (s, -1, &features[*num_features]);
          features[*num_features].start = 0;
          features[*num_features].end   = (unsigned int) -1;
          if (ret)
            (*num_features)++;
        }
    }
}

extern gpointer get_gravity_class (void);

static NsPangoGravity
ns_pango_fc_font_key_get_gravity (NsPangoFcFontKey *key)
{
  const FcPattern *pattern;
  NsPangoGravity gravity = NS_PANGO_GRAVITY_SOUTH;
  FcChar8 *s;

  pattern = ns_pango_fc_font_key_get_pattern (key);
  if (FcPatternGetString (pattern, NS_PANGO_FC_GRAVITY, 0, (FcChar8 **)&s) == FcResultMatch)
    {
      GEnumValue *value = g_enum_get_value_by_nick (get_gravity_class (), (char *)s);
      gravity = value->value;
    }

  return gravity;
}

static void
get_font_size (NsPangoFcFontKey *key,
               double         *pixel_size,
               double         *point_size)
{
  const FcPattern *pattern;
  double dpi;

  pattern = ns_pango_fc_font_key_get_pattern (key);
  if (FcPatternGetDouble (pattern, FC_SIZE, 0, point_size) != FcResultMatch)
    *point_size = 13.;

  if (FcPatternGetDouble (pattern, FC_PIXEL_SIZE, 0, pixel_size) != FcResultMatch)
    {
      if (FcPatternGetDouble (pattern, FC_DPI, 0, &dpi) != FcResultMatch)
        dpi = 72.;

      *pixel_size = *point_size * dpi / 72.;
    }
}

static hb_font_t *
ns_pango_fc_font_create_hb_font (NsPangoFont *font)
{
  NsPangoFcFont *fc_font = NS_PANGO_FC_FONT (font);
  NsPangoFcFontKey *key;
  hb_face_t *hb_face;
  hb_font_t *hb_font;
  double x_scale_inv, y_scale_inv;
  double x_scale, y_scale;
  double pixel_size;
  double point_size;
  double slant G_GNUC_UNUSED;
  double embolden G_GNUC_UNUSED;

  x_scale_inv = y_scale_inv = 1.0;
  pixel_size = 1.0;
  point_size = 1.0;
  slant = 0.0;
  embolden = 0.0;

  key = _ns_pango_fc_font_get_font_key (fc_font);
  if (key)
    {
      const FcPattern *pattern = ns_pango_fc_font_key_get_pattern (key);
      const NsPangoMatrix *ctm;
      NsPangoMatrix font_matrix;
      NsPangoGravity gravity;
      FcMatrix fc_matrix, *fc_matrix_val;
      FcBool fc_embolden;
      double x, y;
      int i;

      /* Fontconfig asks for a bold face to be synthesised when the family has
       * none, which is what CSS calls font-synthesis: weight, and cairo obliges
       * with FT_GlyphSlot_Embolden. The outline thickens in place and the
       * advances stay those of the regular face, as in Blink, so synthesised
       * bold text breaks into lines exactly where pages laid out against
       * Chrome expect it to.
       */
      if (FcPatternGetBool (pattern, FC_EMBOLDEN, 0, &fc_embolden) == FcResultMatch &&
          fc_embolden)
        embolden = 1.0 / 24.0;

      ctm = ns_pango_fc_font_key_get_matrix (key);
      ns_pango_matrix_get_font_scale_factors (ctm, &x_scale_inv, &y_scale_inv);

      FcMatrixInit (&fc_matrix);
      for (i = 0; FcPatternGetMatrix (pattern, FC_MATRIX, i, &fc_matrix_val) == FcResultMatch; i++)
        FcMatrixMultiply (&fc_matrix, &fc_matrix, fc_matrix_val);

      font_matrix.xx = fc_matrix.xx;
      font_matrix.yx = - fc_matrix.yx;
      font_matrix.xy = fc_matrix.xy;
      font_matrix.yy = - fc_matrix.yy;

      ns_pango_matrix_get_font_scale_factors (&font_matrix, &x, &y);
      slant = ns_pango_matrix_get_slant_ratio (&font_matrix);

      x_scale_inv /= x;
      y_scale_inv /= y;

      gravity = ns_pango_fc_font_key_get_gravity (key);
      if (NS_PANGO_GRAVITY_IS_IMPROPER (gravity))
        {
          x_scale_inv = -x_scale_inv;
          y_scale_inv = -y_scale_inv;
        }

      get_font_size (key, &pixel_size, &point_size);
    }

  x_scale = 1. / x_scale_inv;
  y_scale = 1. / y_scale_inv;

  hb_face = ns_pango_fc_font_map_get_hb_face (NS_PANGO_FC_FONT_MAP (fc_font->fontmap), fc_font);

  hb_font = hb_font_create (hb_face);
  hb_font_set_scale (hb_font,
                     pixel_size * NS_PANGO_SCALE * x_scale,
                     pixel_size * NS_PANGO_SCALE * y_scale);
  hb_font_set_ptem (hb_font, point_size);

#if HB_VERSION_ATLEAST (3, 3, 0)
  hb_font_set_synthetic_slant (hb_font, slant);
#endif

#if HB_VERSION_ATLEAST (7, 0, 0)
  hb_font_set_synthetic_bold (hb_font, embolden, embolden, TRUE);
#endif

  if (key)
    {
      const FcPattern *pattern = ns_pango_fc_font_key_get_pattern (key);
      const char *variations;
      int index;
      unsigned int n_axes;
      hb_ot_var_axis_info_t *axes;
      float *coords;
      int i;

      n_axes = hb_ot_var_get_axis_infos (hb_face, 0, NULL, NULL);
      if (n_axes == 0)
        goto done;

      axes = g_new0 (hb_ot_var_axis_info_t, n_axes);
      coords = g_new (float, n_axes);

      hb_ot_var_get_axis_infos (hb_face, 0, &n_axes, axes);
      for (i = 0; i < n_axes; i++)
        {
          coords[axes[i].axis_index] = axes[i].default_value;
          if (axes[i].tag == HB_TAG ('o', 'p', 's', 'z'))
            coords[axes[i].axis_index] = point_size;
        }

      if (FcPatternGetInteger (pattern, FC_INDEX, 0, &index) == FcResultMatch &&
          index != 0)
        {
          unsigned int instance = (index >> 16) - 1;
          hb_ot_var_named_instance_get_design_coords (hb_face, instance, &n_axes, coords);
        }

      if (FcPatternGetString (pattern, FC_FONT_VARIATIONS, 0, (FcChar8 **)&variations) == FcResultMatch)
        ns_pango_parse_variations (variations, axes, n_axes, coords);

      variations = ns_pango_fc_font_key_get_variations (key);
      if (variations)
        ns_pango_parse_variations (variations, axes, n_axes, coords);

      hb_font_set_var_coords_design (hb_font, coords, n_axes);

      g_free (coords);
      g_free (axes);
    }

done:
  return hb_font;
}

static NsPangoLanguage **
_ns_pango_fc_font_get_languages (NsPangoFont *font)
{
  NsPangoFcFont * fcfont = NS_PANGO_FC_FONT (font);

  if (!fcfont->fontmap)
    return NULL;

  return _ns_pango_fc_font_map_get_languages (NS_PANGO_FC_FONT_MAP (fcfont->fontmap), fcfont);
}

gboolean
_ns_pango_fc_font_is_hinted (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);

  return fcfont->is_hinted;
}

void
_ns_pango_fc_font_get_scale_factors (NsPangoFont *font,
                                  double    *x_scale,
                                  double    *y_scale)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);
  NsPangoFcFontPrivate *priv = fcfont->priv;

  ns_pango_matrix_get_font_scale_factors (ns_pango_fc_font_key_get_matrix (priv->key), x_scale, y_scale);
}

static void
ns_pango_fc_font_get_matrix (NsPangoFont   *font,
                          NsPangoMatrix *matrix)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);
  FcMatrix fc_matrix, *fc_matrix_val;

  FcMatrixInit (&fc_matrix);
  for (int i = 0; FcPatternGetMatrix (fcfont->font_pattern, FC_MATRIX, i, &fc_matrix_val) == FcResultMatch; i++)
    FcMatrixMultiply (&fc_matrix, &fc_matrix, fc_matrix_val);

  matrix->xx = fc_matrix.xx;
  matrix->xy = - fc_matrix.xy;
  matrix->yx = - fc_matrix.yx;
  matrix->yy = fc_matrix.yy;
  matrix->x0 = 0.;
  matrix->y0 = 0.;
}

static NsPangoFontFace *
ns_pango_fc_font_get_face (NsPangoFont *font)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);
  NsPangoFcFontPrivate *priv = fcfont->priv;

  return priv->face;
}

void
ns_pango_fc_font_set_face (NsPangoFcFont   *fcfont,
                        NsPangoFontFace *face)
{
  NsPangoFcFontPrivate *priv = fcfont->priv;

  g_set_object (&priv->face, face);
}
