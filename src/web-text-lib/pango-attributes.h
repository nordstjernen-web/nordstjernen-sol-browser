/* Pango
 * pango-attributes.h: Attributed text
 *
 * Copyright (C) 2000 Red Hat Software
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

#ifndef __NS_PANGO_ATTRIBUTES_H__
#define __NS_PANGO_ATTRIBUTES_H__

#include <web-text-lib/pango-font.h>
#include <web-text-lib/pango-color.h>
#include <glib-object.h>

G_BEGIN_DECLS


typedef struct _PangoAttribute        NsPangoAttribute;
typedef struct _PangoAttrClass        NsPangoAttrClass;

typedef struct _PangoAttrString       NsPangoAttrString;
typedef struct _PangoAttrLanguage     NsPangoAttrLanguage;
typedef struct _PangoAttrInt          NsPangoAttrInt;
typedef struct _PangoAttrSize         NsPangoAttrSize;
typedef struct _PangoAttrFloat        NsPangoAttrFloat;
typedef struct _PangoAttrColor        NsPangoAttrColor;
typedef struct _PangoAttrFontDesc     NsPangoAttrFontDesc;
typedef struct _PangoAttrShape        NsPangoAttrShape;
typedef struct _PangoAttrFontFeatures NsPangoAttrFontFeatures;

/**
 * NsPangoAttrType:
 * @NS_PANGO_ATTR_INVALID: does not happen
 * @NS_PANGO_ATTR_LANGUAGE: language ([struct@Pango.AttrLanguage])
 * @NS_PANGO_ATTR_FAMILY: font family name list ([struct@Pango.AttrString])
 * @NS_PANGO_ATTR_STYLE: font slant style ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_WEIGHT: font weight ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_VARIANT: font variant (normal or small caps) ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_STRETCH: font stretch ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_SIZE: font size in points scaled by %NS_PANGO_SCALE ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_FONT_DESC: font description ([struct@Pango.AttrFontDesc])
 * @NS_PANGO_ATTR_FOREGROUND: foreground color ([struct@Pango.AttrColor])
 * @NS_PANGO_ATTR_BACKGROUND: background color ([struct@Pango.AttrColor])
 * @NS_PANGO_ATTR_UNDERLINE: whether the text has an underline ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_STRIKETHROUGH: whether the text is struck-through ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_RISE: baseline displacement ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_SHAPE: shape ([struct@Pango.AttrShape])
 * @NS_PANGO_ATTR_SCALE: font size scale factor ([struct@Pango.AttrFloat])
 * @NS_PANGO_ATTR_FALLBACK: whether fallback is enabled ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_LETTER_SPACING: letter spacing ([struct@NsPangoAttrInt])
 * @NS_PANGO_ATTR_UNDERLINE_COLOR: underline color ([struct@Pango.AttrColor])
 * @NS_PANGO_ATTR_STRIKETHROUGH_COLOR: strikethrough color ([struct@Pango.AttrColor])
 * @NS_PANGO_ATTR_ABSOLUTE_SIZE: font size in pixels scaled by %NS_PANGO_SCALE ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_GRAVITY: base text gravity ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_GRAVITY_HINT: gravity hint ([struct@Pango.AttrInt])
 * @NS_PANGO_ATTR_FONT_FEATURES: OpenType font features ([struct@Pango.AttrFontFeatures]). Since 1.38
 * @NS_PANGO_ATTR_FOREGROUND_ALPHA: foreground alpha ([struct@Pango.AttrInt]). Since 1.38
 * @NS_PANGO_ATTR_BACKGROUND_ALPHA: background alpha ([struct@Pango.AttrInt]). Since 1.38
 * @NS_PANGO_ATTR_ALLOW_BREAKS: whether breaks are allowed ([struct@Pango.AttrInt]). Since 1.44
 * @NS_PANGO_ATTR_SHOW: how to render invisible characters ([struct@Pango.AttrInt]). Since 1.44
 * @NS_PANGO_ATTR_INSERT_HYPHENS: whether to insert hyphens at intra-word line breaks ([struct@Pango.AttrInt]). Since 1.44
 * @NS_PANGO_ATTR_OVERLINE: whether the text has an overline ([struct@Pango.AttrInt]). Since 1.46
 * @NS_PANGO_ATTR_OVERLINE_COLOR: overline color ([struct@Pango.AttrColor]). Since 1.46
 * @NS_PANGO_ATTR_LINE_HEIGHT: line height factor ([struct@Pango.AttrFloat]). Since: 1.50
 * @NS_PANGO_ATTR_ABSOLUTE_LINE_HEIGHT: line height ([struct@Pango.AttrInt]). Since: 1.50
 * @NS_PANGO_ATTR_WORD: override segmentation to classify the range of the attribute as a single word ([struct@Pango.AttrInt]). Since 1.50
 * @NS_PANGO_ATTR_SENTENCE: override segmentation to classify the range of the attribute as a single sentence ([struct@Pango.AttrInt]). Since 1.50
 * @NS_PANGO_ATTR_BASELINE_SHIFT: baseline displacement ([struct@Pango.AttrInt]). Since 1.50
 * @NS_PANGO_ATTR_FONT_SCALE: font-relative size change ([struct@Pango.AttrInt]). Since 1.50
 * @NS_PANGO_ATTR_WIDTH: font width ([struct@Pango.AttrInt]). Since: 1.58
 * @NS_PANGO_ATTR_WORD_SPACING: extra space at each word separator, as CSS
 *   `word-spacing` specifies it ([struct@Pango.AttrInt]). Since: 1.58
 *
 * The `NsPangoAttrType` distinguishes between different types of attributes.
 *
 * Along with the predefined values, it is possible to allocate additional
 * values for custom attributes using [func@AttrType.register]. The predefined
 * values are given below. The type of structure used to store the attribute is
 * listed in parentheses after the description.
 */
typedef enum
{
  NS_PANGO_ATTR_INVALID,           /* 0 is an invalid attribute type */
  NS_PANGO_ATTR_LANGUAGE,          /* NsPangoAttrLanguage */
  NS_PANGO_ATTR_FAMILY,            /* NsPangoAttrString */
  NS_PANGO_ATTR_STYLE,             /* NsPangoAttrInt */
  NS_PANGO_ATTR_WEIGHT,            /* NsPangoAttrInt */
  NS_PANGO_ATTR_VARIANT,           /* NsPangoAttrInt */
  NS_PANGO_ATTR_STRETCH,           /* NsPangoAttrInt */
  NS_PANGO_ATTR_SIZE,              /* NsPangoAttrSize */
  NS_PANGO_ATTR_FONT_DESC,         /* NsPangoAttrFontDesc */
  NS_PANGO_ATTR_FOREGROUND,        /* NsPangoAttrColor */
  NS_PANGO_ATTR_BACKGROUND,        /* NsPangoAttrColor */
  NS_PANGO_ATTR_UNDERLINE,         /* NsPangoAttrInt */
  NS_PANGO_ATTR_STRIKETHROUGH,     /* NsPangoAttrInt */
  NS_PANGO_ATTR_RISE,              /* NsPangoAttrInt */
  NS_PANGO_ATTR_SHAPE,             /* NsPangoAttrShape */
  NS_PANGO_ATTR_SCALE,             /* NsPangoAttrFloat */
  NS_PANGO_ATTR_FALLBACK,          /* NsPangoAttrInt */
  NS_PANGO_ATTR_LETTER_SPACING,    /* NsPangoAttrInt */
  NS_PANGO_ATTR_UNDERLINE_COLOR,   /* NsPangoAttrColor */
  NS_PANGO_ATTR_STRIKETHROUGH_COLOR,/* NsPangoAttrColor */
  NS_PANGO_ATTR_ABSOLUTE_SIZE,     /* NsPangoAttrSize */
  NS_PANGO_ATTR_GRAVITY,           /* NsPangoAttrInt */
  NS_PANGO_ATTR_GRAVITY_HINT,      /* NsPangoAttrInt */
  NS_PANGO_ATTR_FONT_FEATURES,     /* NsPangoAttrFontFeatures */
  NS_PANGO_ATTR_FOREGROUND_ALPHA,  /* NsPangoAttrInt */
  NS_PANGO_ATTR_BACKGROUND_ALPHA,  /* NsPangoAttrInt */
  NS_PANGO_ATTR_ALLOW_BREAKS,      /* NsPangoAttrInt */
  NS_PANGO_ATTR_SHOW,              /* NsPangoAttrInt */
  NS_PANGO_ATTR_INSERT_HYPHENS,    /* NsPangoAttrInt */
  NS_PANGO_ATTR_OVERLINE,          /* NsPangoAttrInt */
  NS_PANGO_ATTR_OVERLINE_COLOR,    /* NsPangoAttrColor */
  NS_PANGO_ATTR_LINE_HEIGHT,       /* NsPangoAttrFloat */
  NS_PANGO_ATTR_ABSOLUTE_LINE_HEIGHT, /* NsPangoAttrInt */
  NS_PANGO_ATTR_TEXT_TRANSFORM,    /* NsPangoAttrInt */
  NS_PANGO_ATTR_WORD,              /* NsPangoAttrInt */
  NS_PANGO_ATTR_SENTENCE,          /* NsPangoAttrInt */
  NS_PANGO_ATTR_BASELINE_SHIFT,    /* NsPangoAttrSize */
  NS_PANGO_ATTR_FONT_SCALE,        /* NsPangoAttrInt */
  NS_PANGO_ATTR_WIDTH,             /* NsPangoAttrInt */
  NS_PANGO_ATTR_WORD_SPACING,      /* NsPangoAttrInt */
} NsPangoAttrType;

/**
 * NsPangoUnderline:
 * @NS_PANGO_UNDERLINE_NONE: no underline should be drawn
 * @NS_PANGO_UNDERLINE_SINGLE: a single underline should be drawn
 * @NS_PANGO_UNDERLINE_DOUBLE: a double underline should be drawn
 * @NS_PANGO_UNDERLINE_LOW: a single underline should be drawn at a
 *   position beneath the ink extents of the text being
 *   underlined. This should be used only for underlining
 *   single characters, such as for keyboard accelerators.
 *   %NS_PANGO_UNDERLINE_SINGLE should be used for extended
 *   portions of text.
 * @NS_PANGO_UNDERLINE_ERROR: an underline indicating an error should
 *   be drawn below. The exact style of rendering is up to the
 *   `NsPangoRenderer` in use, but typical styles include wavy
 *   or dotted lines.
 *   This underline is typically used to indicate an error such
 *   as a possible mispelling; in some cases a contrasting color
 *   may automatically be used. This type of underlining is
 *   available since Pango 1.4.
 * @NS_PANGO_UNDERLINE_SINGLE_LINE: Like @NS_PANGO_UNDERLINE_SINGLE, but
 *   drawn continuously across multiple runs. This type
 *   of underlining is available since Pango 1.46.
 * @NS_PANGO_UNDERLINE_DOUBLE_LINE: Like @NS_PANGO_UNDERLINE_DOUBLE, but
 *   drawn continuously across multiple runs. This type
 *   of underlining is available since Pango 1.46.
 * @NS_PANGO_UNDERLINE_ERROR_LINE: Like @NS_PANGO_UNDERLINE_ERROR, but
 *   drawn continuously across multiple runs. This type
 *   of underlining is available since Pango 1.46.
 *
 * The `NsPangoUnderline` enumeration is used to specify whether text
 * should be underlined, and if so, the type of underlining.
 */
typedef enum {
  NS_PANGO_UNDERLINE_NONE,
  NS_PANGO_UNDERLINE_SINGLE,
  NS_PANGO_UNDERLINE_DOUBLE,
  NS_PANGO_UNDERLINE_LOW,
  NS_PANGO_UNDERLINE_ERROR,
  NS_PANGO_UNDERLINE_SINGLE_LINE,
  NS_PANGO_UNDERLINE_DOUBLE_LINE,
  NS_PANGO_UNDERLINE_ERROR_LINE
} NsPangoUnderline;


/**
 * NsPangoOverline:
 * @NS_PANGO_OVERLINE_NONE: no overline should be drawn
 * @NS_PANGO_OVERLINE_SINGLE: Draw a single line above the ink
 *   extents of the text being underlined.
 *
 * The `NsPangoOverline` enumeration is used to specify whether text
 * should be overlined, and if so, the type of line.
 *
 * Since: 1.46
 */
typedef enum {
  NS_PANGO_OVERLINE_NONE,
  NS_PANGO_OVERLINE_SINGLE
} NsPangoOverline;

/**
 * NsPangoShowFlags:
 * @NS_PANGO_SHOW_NONE: No special treatment for invisible characters
 * @NS_PANGO_SHOW_SPACES: Render spaces, tabs and newlines visibly
 * @NS_PANGO_SHOW_LINE_BREAKS: Render line breaks visibly
 * @NS_PANGO_SHOW_IGNORABLES: Render default-ignorable Unicode
 *   characters visibly
 *
 * These flags affect how Pango treats characters that are normally
 * not visible in the output.
 *
 * Since: 1.44
 */
typedef enum {
  NS_PANGO_SHOW_NONE        = 0,
  NS_PANGO_SHOW_SPACES      = 1 << 0,
  NS_PANGO_SHOW_LINE_BREAKS = 1 << 1,
  NS_PANGO_SHOW_IGNORABLES  = 1 << 2
} NsPangoShowFlags;

/**
 * NsPangoTextTransform:
 * @NS_PANGO_TEXT_TRANSFORM_NONE: Leave text unchanged
 * @NS_PANGO_TEXT_TRANSFORM_LOWERCASE: Display letters and numbers as lowercase
 * @NS_PANGO_TEXT_TRANSFORM_UPPERCASE: Display letters and numbers as uppercase
 * @NS_PANGO_TEXT_TRANSFORM_CAPITALIZE: Display the first character of a word
 *   in titlecase
 *
 * An enumeration that affects how Pango treats characters during shaping.
 *
 * Since: 1.50
 */
typedef enum {
  NS_PANGO_TEXT_TRANSFORM_NONE,
  NS_PANGO_TEXT_TRANSFORM_LOWERCASE,
  NS_PANGO_TEXT_TRANSFORM_UPPERCASE,
  NS_PANGO_TEXT_TRANSFORM_CAPITALIZE,
} NsPangoTextTransform;

/**
 * NsPangoBaselineShift:
 * @NS_PANGO_BASELINE_SHIFT_NONE: Leave the baseline unchanged
 * @NS_PANGO_BASELINE_SHIFT_SUPERSCRIPT: Shift the baseline to the superscript position,
 *   relative to the previous run
 * @NS_PANGO_BASELINE_SHIFT_SUBSCRIPT: Shift the baseline to the subscript position,
 *   relative to the previous run
 *
 * An enumeration that affects baseline shifts between runs.
 *
 * Since: 1.50
 */
typedef enum {
  NS_PANGO_BASELINE_SHIFT_NONE,
  NS_PANGO_BASELINE_SHIFT_SUPERSCRIPT,
  NS_PANGO_BASELINE_SHIFT_SUBSCRIPT,
} NsPangoBaselineShift;

/**
 * NsPangoFontScale:
 * @NS_PANGO_FONT_SCALE_NONE: Leave the font size unchanged
 * @NS_PANGO_FONT_SCALE_SUPERSCRIPT: Change the font to a size suitable for superscripts
 * @NS_PANGO_FONT_SCALE_SUBSCRIPT: Change the font to a size suitable for subscripts
 * @NS_PANGO_FONT_SCALE_SMALL_CAPS: Change the font to a size suitable for Small Caps
 *
 * An enumeration that affects font sizes for superscript
 * and subscript positioning and for (emulated) Small Caps.
 *
 * Since: 1.50
 */
typedef enum {
  NS_PANGO_FONT_SCALE_NONE,
  NS_PANGO_FONT_SCALE_SUPERSCRIPT,
  NS_PANGO_FONT_SCALE_SUBSCRIPT,
  NS_PANGO_FONT_SCALE_SMALL_CAPS,
} NsPangoFontScale;

/**
 * NS_PANGO_ATTR_INDEX_FROM_TEXT_BEGINNING:
 *
 * Value for @start_index in `NsPangoAttribute` that indicates
 * the beginning of the text.
 *
 * Since: 1.24
 */
#define NS_PANGO_ATTR_INDEX_FROM_TEXT_BEGINNING ((guint)0)

/**
 * NS_PANGO_ATTR_INDEX_TO_TEXT_END: (value 4294967295)
 *
 * Value for @end_index in `NsPangoAttribute` that indicates
 * the end of the text.
 *
 * Since: 1.24
 */
#define NS_PANGO_ATTR_INDEX_TO_TEXT_END ((guint)(G_MAXUINT + 0))

/**
 * NsPangoAttribute: (copy-func ns_pango_attribute_copy) (free-func ns_pango_attribute_destroy)
 * @klass: the class structure holding information about the type of the attribute
 * @start_index: the start index of the range (in bytes).
 * @end_index: end index of the range (in bytes). The character at this index
 *   is not included in the range.
 *
 * The `NsPangoAttribute` structure represents the common portions of all
 * attributes.
 *
 * Particular types of attributes include this structure as their initial
 * portion. The common portion of the attribute holds the range to which
 * the value in the type-specific part of the attribute applies and should
 * be initialized using [method@Pango.Attribute.init]. By default, an attribute
 * will have an all-inclusive range of [0,%G_MAXUINT].
 */
struct _PangoAttribute
{
  const NsPangoAttrClass *klass;
  guint start_index;
  guint end_index;
};

/**
 * NsPangoAttrFilterFunc:
 * @attribute: a Pango attribute
 * @user_data: user data passed to the function
 *
 * Type of a function filtering a list of attributes.
 *
 * Return value: %TRUE if the attribute should be selected for
 *   filtering, %FALSE otherwise.
 */
typedef gboolean (*NsPangoAttrFilterFunc) (NsPangoAttribute *attribute,
                                         gpointer        user_data);

/**
 * NsPangoAttrDataCopyFunc:
 * @user_data: user data to copy
 *
 * Type of a function that can duplicate user data for an attribute.
 *
 * Return value: new copy of @user_data.
 **/
typedef gpointer (*NsPangoAttrDataCopyFunc) (gconstpointer user_data);

/**
 * NsPangoAttrClass:
 * @type: the type ID for this attribute
 * @copy: function to duplicate an attribute of this type
 *   (see [method@Pango.Attribute.copy])
 * @destroy: function to free an attribute of this type
 *   (see [method@Pango.Attribute.destroy])
 * @equal: function to check two attributes of this type for equality
 *   (see [method@Pango.Attribute.equal])
 *
 * The `NsPangoAttrClass` structure stores the type and operations for
 * a particular type of attribute.
 *
 * The functions in this structure should not be called directly. Instead,
 * one should use the wrapper functions provided for `NsPangoAttribute`.
 */
struct _PangoAttrClass
{
  /*< public >*/
  NsPangoAttrType type;
  NsPangoAttribute * (*copy) (const NsPangoAttribute *attr);
  void             (*destroy) (NsPangoAttribute *attr);
  gboolean         (*equal) (const NsPangoAttribute *attr1, const NsPangoAttribute *attr2);
};

/**
 * NsPangoAttrString:
 * @attr: the common portion of the attribute
 * @value: the string which is the value of the attribute
 *
 * The `NsPangoAttrString` structure is used to represent attributes with
 * a string value.
 */
struct _PangoAttrString
{
  NsPangoAttribute attr;
  char *value;
};
/**
 * NsPangoAttrLanguage:
 * @attr: the common portion of the attribute
 * @value: the `NsPangoLanguage` which is the value of the attribute
 *
 * The `NsPangoAttrLanguage` structure is used to represent attributes that
 * are languages.
 */
struct _PangoAttrLanguage
{
  NsPangoAttribute attr;
  NsPangoLanguage *value;
};
/**
 * NsPangoAttrInt:
 * @attr: the common portion of the attribute
 * @value: the value of the attribute
 *
 * The `NsPangoAttrInt` structure is used to represent attributes with
 * an integer or enumeration value.
 */
struct _PangoAttrInt
{
  NsPangoAttribute attr;
  int value;
};
/**
 * NsPangoAttrFloat:
 * @attr: the common portion of the attribute
 * @value: the value of the attribute
 *
 * The `NsPangoAttrFloat` structure is used to represent attributes with
 * a float or double value.
 */
struct _PangoAttrFloat
{
  NsPangoAttribute attr;
  double value;
};
/**
 * NsPangoAttrColor:
 * @attr: the common portion of the attribute
 * @color: the `NsPangoColor` which is the value of the attribute
 *
 * The `NsPangoAttrColor` structure is used to represent attributes that
 * are colors.
 */
struct _PangoAttrColor
{
  NsPangoAttribute attr;
  NsPangoColor color;
};

/**
 * NsPangoAttrSize:
 * @attr: the common portion of the attribute
 * @size: size of font, in units of 1/%NS_PANGO_SCALE of a point (for
 *   %NS_PANGO_ATTR_SIZE) or of a device unit (for %NS_PANGO_ATTR_ABSOLUTE_SIZE)
 * @absolute: whether the font size is in device units or points.
 *   This field is only present for compatibility with Pango-1.8.0
 *   (%NS_PANGO_ATTR_ABSOLUTE_SIZE was added in 1.8.1); and always will
 *   be %FALSE for %NS_PANGO_ATTR_SIZE and %TRUE for %NS_PANGO_ATTR_ABSOLUTE_SIZE.
 *
 * The `NsPangoAttrSize` structure is used to represent attributes which
 * set font size.
 */
struct _PangoAttrSize
{
  NsPangoAttribute attr;
  int size;
  guint absolute : 1;
};

/**
 * NsPangoAttrShape:
 * @attr: the common portion of the attribute
 * @ink_rect: the ink rectangle to restrict to
 * @logical_rect: the logical rectangle to restrict to
 * @data: user data set (see [func@Pango.AttrShape.new_with_data])
 * @copy_func: copy function for the user data
 * @destroy_func: destroy function for the user data
 *
 * The `NsPangoAttrShape` structure is used to represent attributes which
 * impose shape restrictions.
 */
struct _PangoAttrShape
{
  NsPangoAttribute attr;
  NsPangoRectangle ink_rect;
  NsPangoRectangle logical_rect;

  gpointer              data;
  NsPangoAttrDataCopyFunc copy_func;
  GDestroyNotify        destroy_func;
};

/**
 * NsPangoAttrFontDesc:
 * @attr: the common portion of the attribute
 * @desc: the font description which is the value of this attribute
 *
 * The `NsPangoAttrFontDesc` structure is used to store an attribute that
 * sets all aspects of the font description at once.
 */
struct _PangoAttrFontDesc
{
  NsPangoAttribute attr;
  NsPangoFontDescription *desc;
};

/**
 * NsPangoAttrFontFeatures:
 * @attr: the common portion of the attribute
 * @features: the features, as a string in CSS syntax
 *
 * The `NsPangoAttrFontFeatures` structure is used to represent OpenType
 * font features as an attribute.
 *
 * Since: 1.38
 */
struct _PangoAttrFontFeatures
{
  NsPangoAttribute attr;
  gchar *features;
};


NS_PANGO_AVAILABLE_IN_1_20
void                    ns_pango_attribute_init                    (NsPangoAttribute             *attr,
                                                                 const NsPangoAttrClass       *klass);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attribute_copy                    (const NsPangoAttribute       *attr);
NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attribute_destroy                 (NsPangoAttribute             *attr);
NS_PANGO_AVAILABLE_IN_ALL
gboolean                ns_pango_attribute_equal                   (const NsPangoAttribute       *attr1,
                                                                 const NsPangoAttribute       *attr2) G_GNUC_PURE;

NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_language_new                 (NsPangoLanguage              *language);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_family_new                   (const char                 *family);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_foreground_new               (guint16                     red,
                                                                 guint16                     green,
                                                                 guint16                     blue);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_background_new               (guint16                     red,
                                                                 guint16                     green,
                                                                 guint16                     blue);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_size_new                     (int                         size);
NS_PANGO_AVAILABLE_IN_1_8
NsPangoAttribute *        ns_pango_attr_size_new_absolute            (int                         size);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_style_new                    (NsPangoStyle                  style);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_weight_new                   (NsPangoWeight                 weight);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_variant_new                  (NsPangoVariant                variant);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_stretch_new                  (NsPangoStretch                stretch);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_font_desc_new                (const NsPangoFontDescription *desc);

NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_underline_new                (NsPangoUnderline              underline);
NS_PANGO_AVAILABLE_IN_1_8
NsPangoAttribute *        ns_pango_attr_underline_color_new          (guint16                     red,
                                                                 guint16                     green,
                                                                 guint16                     blue);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_strikethrough_new            (gboolean                    strikethrough);
NS_PANGO_AVAILABLE_IN_1_8
NsPangoAttribute *        ns_pango_attr_strikethrough_color_new      (guint16                     red,
                                                                 guint16                     green,
                                                                 guint16                     blue);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_rise_new                     (int                         rise);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttribute *        ns_pango_attr_font_scale_new               (NsPangoFontScale              scale);
NS_PANGO_AVAILABLE_IN_1_4
NsPangoAttribute *        ns_pango_attr_fallback_new                 (gboolean                    enable_fallback);
NS_PANGO_AVAILABLE_IN_1_6
NsPangoAttribute *        ns_pango_attr_letter_spacing_new           (int                         letter_spacing);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_shape_new                    (const NsPangoRectangle        *ink_rect,
                                                                 const NsPangoRectangle        *logical_rect);
NS_PANGO_AVAILABLE_IN_1_8
NsPangoAttribute *        ns_pango_attr_shape_new_with_data          (const NsPangoRectangle        *ink_rect,
                                                                 const NsPangoRectangle        *logical_rect,
                                                                 gpointer                     data,
                                                                 NsPangoAttrDataCopyFunc        copy_func,
                                                                 GDestroyNotify               destroy_func);
NS_PANGO_AVAILABLE_IN_1_38
NsPangoAttribute *        ns_pango_attr_font_features_new            (const char                  *features);
NS_PANGO_AVAILABLE_IN_1_38
NsPangoAttribute *        ns_pango_attr_foreground_alpha_new         (guint16                      alpha);
NS_PANGO_AVAILABLE_IN_1_38
NsPangoAttribute *        ns_pango_attr_background_alpha_new         (guint16                      alpha);
/**
 * NS_PANGO_BREAKS_ANYWHERE:
 *
 * A value for [func@Pango.attr_allow_breaks_new] that makes every
 * character break in the range a line break, as CSS word-break: break-all.
 */
#define NS_PANGO_BREAKS_ANYWHERE 2

/**
 * NS_PANGO_BREAKS_WORDS:
 *
 * A value for [func@Pango.attr_allow_breaks_new] that keeps
 * %NS_PANGO_WRAP_WORD_CHAR from breaking inside the words of the range.
 */
#define NS_PANGO_BREAKS_WORDS 3

NS_PANGO_AVAILABLE_IN_1_44
NsPangoAttribute *        ns_pango_attr_allow_breaks_new             (gboolean                     allow_breaks);


NS_PANGO_AVAILABLE_IN_1_44
NsPangoAttribute *        ns_pango_attr_insert_hyphens_new           (gboolean                     insert_hyphens);
NS_PANGO_AVAILABLE_IN_1_46
NsPangoAttribute *        ns_pango_attr_overline_new                 (NsPangoOverline                overline);
NS_PANGO_AVAILABLE_IN_1_46
NsPangoAttribute *        ns_pango_attr_overline_color_new           (guint16                      red,
                                                                 guint16                      green,
                                                                 guint16                      blue);
NS_PANGO_AVAILABLE_IN_1_44
NsPangoAttribute *        ns_pango_attr_show_new                     (NsPangoShowFlags               flags);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttribute *        ns_pango_attr_line_height_new              (double                       factor);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttribute *        ns_pango_attr_line_height_new_absolute     (int                          height);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttribute *        ns_pango_attr_text_transform_new           (NsPangoTextTransform transform);

NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrString       * ns_pango_attribute_as_string               (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrLanguage     * ns_pango_attribute_as_language             (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrInt          * ns_pango_attribute_as_int                  (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrSize         * ns_pango_attribute_as_size                 (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrFloat        * ns_pango_attribute_as_float                (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrColor        * ns_pango_attribute_as_color                (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrFontDesc     * ns_pango_attribute_as_font_desc            (NsPangoAttribute              *attr);
NS_PANGO_AVAILABLE_IN_1_50
NsPangoAttrFontFeatures * ns_pango_attribute_as_font_features        (NsPangoAttribute              *attr);

/* Attribute lists */

typedef struct _PangoAttrList     NsPangoAttrList;
typedef struct _PangoAttrIterator NsPangoAttrIterator;


/**
 * NsPangoAttrIterator:
 *
 * A `NsPangoAttrIterator` is used to iterate through a `NsPangoAttrList`.
 *
 * A new iterator is created with [method@Pango.AttrList.get_iterator].
 * Once the iterator is created, it can be advanced through the style
 * changes in the text using [method@Pango.AttrIterator.next]. At each
 * style change, the range of the current style segment and the attributes
 * currently in effect can be queried.
 */

/**
 * NsPangoAttrList:
 *
 * A `NsPangoAttrList` represents a list of attributes that apply to a section
 * of text.
 *
 * The attributes in a `NsPangoAttrList` are, in general, allowed to overlap in
 * an arbitrary fashion. However, if the attributes are manipulated only through
 * [method@Pango.AttrList.change], the overlap between properties will meet
 * stricter criteria.
 *
 * Since the `NsPangoAttrList` structure is stored as a linear list, it is not
 * suitable for storing attributes for large amounts of text. In general, you
 * should not use a single `NsPangoAttrList` for more than one paragraph of text.
 */


NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttrList *         ns_pango_attr_list_new             (void);
NS_PANGO_AVAILABLE_IN_1_10
NsPangoAttrList *         ns_pango_attr_list_ref             (NsPangoAttrList         *list);
NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attr_list_unref           (NsPangoAttrList         *list);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttrList *         ns_pango_attr_list_copy            (NsPangoAttrList         *list);
NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attr_list_insert          (NsPangoAttrList         *list,
                                                         NsPangoAttribute        *attr);
NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attr_list_insert_before   (NsPangoAttrList         *list,
                                                         NsPangoAttribute        *attr);

NS_PANGO_AVAILABLE_IN_1_2
NsPangoAttrList *         ns_pango_attr_list_filter          (NsPangoAttrList         *list,
                                                         NsPangoAttrFilterFunc    func,
                                                         gpointer               data);

NS_PANGO_AVAILABLE_IN_1_44
GSList *                ns_pango_attr_list_get_attributes  (NsPangoAttrList         *list);

NS_PANGO_AVAILABLE_IN_1_46
gboolean                ns_pango_attr_list_equal           (NsPangoAttrList         *list,
                                                         NsPangoAttrList         *other_list);



NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttrIterator *     ns_pango_attr_list_get_iterator    (NsPangoAttrList         *list);

NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attr_iterator_range       (NsPangoAttrIterator     *iterator,
                                                         int                   *start,
                                                         int                   *end);
NS_PANGO_AVAILABLE_IN_ALL
gboolean                ns_pango_attr_iterator_next        (NsPangoAttrIterator     *iterator);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttrIterator *     ns_pango_attr_iterator_copy        (NsPangoAttrIterator     *iterator);
NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attr_iterator_destroy     (NsPangoAttrIterator     *iterator);
NS_PANGO_AVAILABLE_IN_ALL
NsPangoAttribute *        ns_pango_attr_iterator_get         (NsPangoAttrIterator     *iterator,
                                                         NsPangoAttrType          type);
NS_PANGO_AVAILABLE_IN_ALL
void                    ns_pango_attr_iterator_get_font    (NsPangoAttrIterator     *iterator,
                                                         NsPangoFontDescription  *desc,
                                                         NsPangoLanguage        **language,
                                                         GSList               **extra_attrs);
NS_PANGO_AVAILABLE_IN_1_2
GSList *                ns_pango_attr_iterator_get_attrs   (NsPangoAttrIterator     *iterator);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsPangoAttribute, ns_pango_attribute_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsPangoAttrList, ns_pango_attr_list_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NsPangoAttrIterator, ns_pango_attr_iterator_destroy)

G_END_DECLS

#endif /* __NS_PANGO_ATTRIBUTES_H__ */
