/* Pango
 * pangofc-fontmap.c: Base fontmap type for fontconfig-based backends
 *
 * Copyright (C) 2000-2003 Red Hat, Inc.
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

/**
 * NsPangoFcFontMap:
 *
 * `NsPangoFcFontMap` is a base class for font map implementations using the
 * Fontconfig and FreeType libraries.
 *
 * It is used in the Xft and FreeType backends shipped with Pango,
 * but can also be used when creating new backends. Any backend
 * deriving from this base class will take advantage of the wide
 * range of shapers implemented using FreeType that come with Pango.
 */
#define FONTSET_CACHE_SIZE 256

#include "config.h"
#include <math.h>

#include <gio/gio.h>

#include "pango-context.h"
#include "pango-font-private.h"
#include "pango-fontmap-private.h"
#include "pangofc-fontmap-private.h"
#include "pangofc-private.h"
#include "pango-impl-utils.h"
#include "pango-coverage-private.h"
#include "pango-trace-private.h"
#include <hb-ft.h>
#include <fontconfig/fcfreetype.h>


/* Overview:
 *
 * All programming is a practice in caching data.  NsPangoFcFontMap is the
 * major caching container of a Pango system on a Linux desktop.  Here is
 * a short overview of how it all works.
 *
 * In short, Fontconfig search patterns are constructed and a fontset loaded
 * using them.  Here is how we achieve that:
 *
 * - All FcPattern's referenced by any object in the fontmap are uniquified
 *   and cached in the fontmap.  This both speeds lookups based on patterns
 *   faster, and saves memory.  This is handled by fontmap->priv->pattern_hash.
 *   The patterns are cached indefinitely.
 *
 * - The results of a FcFontSort() are used to populate fontsets.  However,
 *   FcFontSort() relies on the search pattern only, which includes the font
 *   size but not the full font matrix.  The fontset however depends on the
 *   matrix.  As a result, multiple fontsets may need results of the
 *   FcFontSort() on the same input pattern (think rotating text).  As such,
 *   we cache FcFontSort() results in fontmap->priv->patterns_hash which
 *   is a refcounted structure.  This level of abstraction also allows for
 *   optimizations like calling FcFontMatch() instead of FcFontSort(), and
 *   only calling FcFontSort() if any patterns other than the first match
 *   are needed.  Another possible optimization would be to call FcFontSort()
 *   without trimming, and do the trimming lazily as we go.  Only pattern sets
 *   already referenced by a fontset are cached.
 *
 * - A number of most-recently-used fontsets are cached and reused when
 *   needed.  This is achieved using fontmap->priv->fontset_hash and
 *   fontmap->priv->fontset_cache.
 *
 * - All fonts created by any of our fontsets are also cached and reused.
 *   This is what fontmap->priv->font_hash does.
 *
 * - Data that only depends on the font file and face index is cached and
 *   reused by multiple fonts.  This includes coverage and cmap cache info.
 *   This is done using fontmap->priv->font_face_data_hash.
 *
 * Upon a cache_clear() request, all caches are emptied.  All objects (fonts,
 * fontsets, faces, families) having a reference from outside will still live
 * and may reference the fontmap still, but will not be reused by the fontmap.
 *
 *
 * Todo:
 *
 * - Make NsPangoCoverage a GObject and subclass it as NsPangoFcCoverage which
 *   will directly use FcCharset. (#569622)
 *
 * - Lazy trimming of FcFontSort() results.  Requires fontconfig with
 *   FcCharSetMerge().
 */

typedef enum {
  /* Initial state; Fontconfig is not initialized yet */
  DEFAULT_CONFIG_NOT_INITIALIZED,

  /* We have a thread doing Fontconfig initialization in the background */
  DEFAULT_CONFIG_INITIALIZING,

  /* FcInit() finished and its default configuration is loaded */
  DEFAULT_CONFIG_INITIALIZED
} DefaultConfig;

/* We call FcInit in a thread and set fc_initialized
 * when done, and are protected by a mutex. The thread
 * signals the cond when FcInit is done.
 */
static GMutex fc_init_mutex;
static GCond fc_init_cond;
static DefaultConfig fc_initialized = DEFAULT_CONFIG_NOT_INITIALIZED;


typedef struct _PangoFcFontFaceData NsPangoFcFontFaceData;
typedef struct _PangoFcFace         NsPangoFcFace;
typedef struct _PangoFcFamily       NsPangoFcFamily;
typedef struct _PangoFcFindFuncInfo NsPangoFcFindFuncInfo;
typedef struct _PangoFcPatterns     NsPangoFcPatterns;
typedef struct _PangoFcFontset      NsPangoFcFontset;

#define NS_PANGO_FC_TYPE_FAMILY            (ns_pango_fc_family_get_type ())
#define NS_PANGO_FC_FAMILY(object)         (G_TYPE_CHECK_INSTANCE_CAST ((object), NS_PANGO_FC_TYPE_FAMILY, NsPangoFcFamily))
#define NS_PANGO_FC_IS_FAMILY(object)      (G_TYPE_CHECK_INSTANCE_TYPE ((object), NS_PANGO_FC_TYPE_FAMILY))

#define NS_PANGO_FC_TYPE_FACE              (ns_pango_fc_face_get_type ())
#define NS_PANGO_FC_FACE(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), NS_PANGO_FC_TYPE_FACE, NsPangoFcFace))
#define NS_PANGO_FC_IS_FACE(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), NS_PANGO_FC_TYPE_FACE))

#define NS_PANGO_FC_TYPE_FONTSET           (ns_pango_fc_fontset_get_type ())
#define NS_PANGO_FC_FONTSET(object)        (G_TYPE_CHECK_INSTANCE_CAST ((object), NS_PANGO_FC_TYPE_FONTSET, NsPangoFcFontset))
#define NS_PANGO_FC_IS_FONTSET(object)     (G_TYPE_CHECK_INSTANCE_TYPE ((object), NS_PANGO_FC_TYPE_FONTSET))

struct _PangoFcFontMapPrivate
{
  GHashTable *fontset_hash;	/* Maps NsPangoFcFontsetKey -> NsPangoFcFontset  */
  GQueue *fontset_cache;	/* Recently used fontsets */

  GHashTable *font_hash;	/* Maps NsPangoFcFontKey -> NsPangoFcFont */

  GHashTable *patterns_hash;	/* Maps FcPattern -> NsPangoFcPatterns */

  /* pattern_hash is used to make sure we only store one copy of
   * each identical pattern. (Speeds up lookup).
   */
  GHashTable *pattern_hash;

  GHashTable *font_face_data_hash; /* Maps font file name/id -> data */

  /* List of all families available */
  NsPangoFcFamily **families;
  int n_families;		/* -1 == uninitialized */

  double dpi;

  /* Decoders */
  GSList *findfuncs;

  guint closed : 1;

  FcConfig *config;
  FcFontSet *fonts;

  GAsyncQueue *queue;
};

struct _PangoFcFontFaceData
{
  /* Key */
  char *filename;
  int id;            /* needed to handle TTC files with multiple faces */

  /* Data */
  FcPattern *pattern;  /* Referenced pattern that owns filename */
  NsPangoCoverage *coverage;
  NsPangoLanguage **languages;

  hb_face_t *hb_face;
};

struct _PangoFcFace
{
  NsPangoFontFace parent_instance;

  NsPangoFcFamily *family;
  char *style;
  FcPattern *pattern;

  guint fake : 1;
  guint regular : 1;
};

struct _PangoFcFamily
{
  NsPangoFontFamily parent_instance;

  NsPangoFcFontMap *fontmap;
  char *family_name;

  FcFontSet *patterns;
  NsPangoFcFace **faces;
  int n_faces;		/* -1 == uninitialized */

  int spacing;  /* FC_SPACING */
  gboolean variable;
};

struct _PangoFcFindFuncInfo
{
  NsPangoFcDecoderFindFunc findfunc;
  gpointer               user_data;
  GDestroyNotify         dnotify;
  gpointer               ddata;
};

static GType    ns_pango_fc_family_get_type     (void);
static GType    ns_pango_fc_face_get_type       (void);
static GType    ns_pango_fc_fontset_get_type    (void);

static void          ns_pango_fc_font_map_finalize      (GObject                      *object);
static NsPangoFont *   ns_pango_fc_font_map_load_font     (NsPangoFontMap                 *fontmap,
						       NsPangoContext                 *context,
						       const NsPangoFontDescription   *description);
static NsPangoFontset *ns_pango_fc_font_map_load_fontset  (NsPangoFontMap                 *fontmap,
						       NsPangoContext                 *context,
						       const NsPangoFontDescription   *desc,
						       NsPangoLanguage                *language);
static void          ns_pango_fc_font_map_list_families (NsPangoFontMap                 *fontmap,
						       NsPangoFontFamily            ***families,
						       int                          *n_families);
static NsPangoFontFamily *ns_pango_fc_font_map_get_family (NsPangoFontMap *fontmap,
                                                      const char   *name);

static double ns_pango_fc_font_map_get_resolution (NsPangoFcFontMap *fcfontmap,
						NsPangoContext   *context);
static NsPangoFont *ns_pango_fc_font_map_new_font   (NsPangoFcFontMap    *fontmap,
						NsPangoFcFontsetKey *fontset_key,
						FcPattern         *match);
static NsPangoFont * ns_pango_fc_font_map_new_font_from_key (NsPangoFcFontMap    *fcfontmap,
                                                        NsPangoFcFontKey    *key);

static NsPangoFontFace *ns_pango_fc_font_map_get_face (NsPangoFontMap *fontmap,
                                                  NsPangoFont    *font);

static void ns_pango_fc_font_map_changed (NsPangoFontMap *fontmap);

static NsPangoFont * ns_pango_fc_font_map_reload_font (NsPangoFontMap *fontmap,
                                                  NsPangoFont    *font,
                                                  double        scale,
                                                  NsPangoContext *context,
                                                  const char   *variations);

static gboolean  ns_pango_fc_font_map_add_font_file (NsPangoFontMap  *fontmap,
                                                  const char    *filename,
                                                  GError       **error);

static guint    ns_pango_fc_font_face_data_hash  (NsPangoFcFontFaceData *key);
static gboolean ns_pango_fc_font_face_data_equal (NsPangoFcFontFaceData *key1,
					       NsPangoFcFontFaceData *key2);

static void               ns_pango_fc_fontset_key_init  (NsPangoFcFontsetKey          *key,
						      NsPangoFcFontMap             *fcfontmap,
						      NsPangoContext               *context,
						      const NsPangoFontDescription *desc,
						      NsPangoLanguage              *language);
static NsPangoFcFontsetKey *ns_pango_fc_fontset_key_copy  (const NsPangoFcFontsetKey *key);
static void               ns_pango_fc_fontset_key_free  (NsPangoFcFontsetKey       *key);
static guint              ns_pango_fc_fontset_key_hash  (const NsPangoFcFontsetKey *key);
static gboolean           ns_pango_fc_fontset_key_equal (const NsPangoFcFontsetKey *key_a,
						      const NsPangoFcFontsetKey *key_b);

static void               ns_pango_fc_font_key_init     (NsPangoFcFontKey       *key,
						      NsPangoFcFontMap       *fcfontmap,
						      NsPangoFcFontsetKey    *fontset_key,
						      FcPattern            *pattern);
static void               ns_pango_fc_font_key_init_from_key (NsPangoFcFontKey       *key,
                                                           const NsPangoFcFontKey *orig);
static NsPangoFcFontKey    *ns_pango_fc_font_key_copy     (const NsPangoFcFontKey *key);
static void               ns_pango_fc_font_key_free     (NsPangoFcFontKey       *key);
static guint              ns_pango_fc_font_key_hash     (const NsPangoFcFontKey *key);
static gboolean           ns_pango_fc_font_key_equal    (const NsPangoFcFontKey *key_a,
						      const NsPangoFcFontKey *key_b);

static NsPangoFcPatterns *ns_pango_fc_patterns_new   (FcPattern       *pat,
						 NsPangoFcFontMap  *fontmap);
static NsPangoFcPatterns *ns_pango_fc_patterns_ref   (NsPangoFcPatterns *pats);
static void             ns_pango_fc_patterns_unref (NsPangoFcPatterns *pats);
static FcPattern       *ns_pango_fc_patterns_get_pattern      (NsPangoFcPatterns *pats);
static FcPattern       *ns_pango_fc_patterns_get_font_pattern (NsPangoFcPatterns *pats,
							    int              i,
							    gboolean        *prepare);

static FcPattern *uniquify_pattern (NsPangoFcFontMap *fcfontmap,
				    FcPattern      *pattern);

static void ensure_families (NsPangoFcFontMap *fcfontmap);
static void ensure_faces (NsPangoFcFamily *family);
static int compare_face_pattern (FcPattern *p1,
                                 FcPattern *p2);

gpointer get_gravity_class (void);

gpointer
get_gravity_class (void)
{
  static GEnumClass *class = NULL; /* MT-safe */

  if (g_once_init_enter (&class))
    g_once_init_leave (&class, (gpointer)g_type_class_ref (NS_TYPE_PANGO_GRAVITY));

  return class;
}

static guint
ns_pango_fc_font_face_data_hash (NsPangoFcFontFaceData *key)
{
  return g_str_hash (key->filename) ^ key->id;
}

static gboolean
ns_pango_fc_font_face_data_equal (NsPangoFcFontFaceData *key1,
			       NsPangoFcFontFaceData *key2)
{
  return key1->id == key2->id &&
	 (key1 == key2 || 0 == strcmp (key1->filename, key2->filename));
}

static void
ns_pango_fc_font_face_data_free (NsPangoFcFontFaceData *data)
{
  FcPatternDestroy (data->pattern);

  if (data->coverage)
    g_object_unref (data->coverage);

  g_free (data->languages);

  hb_face_destroy (data->hb_face);

  g_slice_free (NsPangoFcFontFaceData, data);
}

/* Fowler / Noll / Vo (FNV) Hash (http://www.isthe.com/chongo/tech/comp/fnv/)
 *
 * Not necessarily better than a lot of other hashes, but should be OK, and
 * well tested with binary data.
 */

#define FNV_32_PRIME ((guint32)0x01000193)
#define FNV1_32_INIT ((guint32)0x811c9dc5)

static guint32
hash_bytes_fnv (unsigned char *buffer,
		int            len,
		guint32        hval)
{
  while (len--)
    {
      hval *= FNV_32_PRIME;
      hval ^= *buffer++;
    }

  return hval;
}

static void
get_context_matrix (NsPangoContext *context,
		    NsPangoMatrix *matrix)
{
  const NsPangoMatrix *set_matrix;
  const NsPangoMatrix identity = NS_PANGO_MATRIX_INIT;

  set_matrix = context ? ns_pango_context_get_matrix (context) : NULL;
  *matrix = set_matrix ? *set_matrix : identity;
  matrix->x0 = matrix->y0 = 0.;
}

static int
get_scaled_size (NsPangoFcFontMap             *fcfontmap,
		 NsPangoContext               *context,
		 const NsPangoFontDescription *desc)
{
  double size = ns_pango_font_description_get_size (desc);

  if (!ns_pango_font_description_get_size_is_absolute (desc))
    {
      double dpi = ns_pango_fc_font_map_get_resolution (fcfontmap, context);

      size = size * dpi / 72.;
    }

  return .5 + ns_pango_matrix_get_font_scale_factor (ns_pango_context_get_matrix (context)) * size;
}



struct _PangoFcFontsetKey {
  NsPangoFcFontMap *fontmap;
  NsPangoLanguage *language;
  NsPangoFontDescription *desc;
  NsPangoMatrix matrix;
  int pixelsize;
  double resolution;
  gpointer context_key;
  char *variations;
  char *features;
};

struct _PangoFcFontKey {
  NsPangoFcFontMap *fontmap;
  FcPattern *pattern;
  NsPangoMatrix matrix;
  gpointer context_key;
  char *variations;
  char *features;
};

static void
ns_pango_fc_fontset_key_init (NsPangoFcFontsetKey          *key,
			   NsPangoFcFontMap             *fcfontmap,
			   NsPangoContext               *context,
			   const NsPangoFontDescription *desc,
			   NsPangoLanguage              *language)
{
  if (!language && context)
    language = ns_pango_context_get_language (context);

  key->fontmap = fcfontmap;
  get_context_matrix (context, &key->matrix);
  key->pixelsize = get_scaled_size (fcfontmap, context, desc);
  key->resolution = ns_pango_fc_font_map_get_resolution (fcfontmap, context);
  key->language = language;
  key->variations = g_strdup (ns_pango_font_description_get_variations (desc));
  key->features = g_strdup (ns_pango_font_description_get_features (desc));
  key->desc = ns_pango_font_description_copy_static (desc);
  ns_pango_font_description_unset_fields (key->desc, NS_PANGO_FONT_MASK_SIZE | NS_PANGO_FONT_MASK_VARIATIONS | NS_PANGO_FONT_MASK_FEATURES);

  if (context && NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->context_key_get)
    key->context_key = (gpointer)NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->context_key_get (fcfontmap, context);
  else
    key->context_key = NULL;
}

static gboolean
ns_pango_fc_fontset_key_equal (const NsPangoFcFontsetKey *key_a,
			    const NsPangoFcFontsetKey *key_b)
{
  if (key_a->language == key_b->language &&
      key_a->pixelsize == key_b->pixelsize &&
      key_a->resolution == key_b->resolution &&
      ((key_a->variations == NULL && key_b->variations == NULL) ||
       (key_a->variations && key_b->variations && (strcmp (key_a->variations, key_b->variations) == 0))) &&
      ((key_a->features == NULL && key_b->features == NULL) ||
       (key_a->features && key_b->features && (strcmp (key_a->features, key_b->features) == 0))) &&
      ns_pango_font_description_equal (key_a->desc, key_b->desc) &&
      0 == memcmp (&key_a->matrix, &key_b->matrix, 4 * sizeof (double)))
    {
      if (key_a->context_key)
	return NS_PANGO_FC_FONT_MAP_GET_CLASS (key_a->fontmap)->context_key_equal (key_a->fontmap,
										key_a->context_key,
										key_b->context_key);
      else
        return key_a->context_key == key_b->context_key;
    }
  else
    return FALSE;
}

static guint
ns_pango_fc_fontset_key_hash (const NsPangoFcFontsetKey *key)
{
    guint32 hash = FNV1_32_INIT;

    /* We do a bytewise hash on the doubles */
    hash = hash_bytes_fnv ((unsigned char *)(&key->matrix), sizeof (double) * 4, hash);
    hash = hash_bytes_fnv ((unsigned char *)(&key->resolution), sizeof (double), hash);

    hash ^= key->pixelsize;

    if (key->variations)
      hash ^= g_str_hash (key->variations);

    if (key->features)
      hash ^= g_str_hash (key->features);

    if (key->context_key)
      hash ^= NS_PANGO_FC_FONT_MAP_GET_CLASS (key->fontmap)->context_key_hash (key->fontmap,
									    key->context_key);

    return (hash ^
	    GPOINTER_TO_UINT (key->language) ^
	    ns_pango_font_description_hash (key->desc));
}

static void
ns_pango_fc_fontset_key_free (NsPangoFcFontsetKey *key)
{
  ns_pango_font_description_free (key->desc);
  g_free (key->variations);
  g_free (key->features);

  if (key->context_key)
    NS_PANGO_FC_FONT_MAP_GET_CLASS (key->fontmap)->context_key_free (key->fontmap,
								  key->context_key);

  g_slice_free (NsPangoFcFontsetKey, key);
}

static NsPangoFcFontsetKey *
ns_pango_fc_fontset_key_copy (const NsPangoFcFontsetKey *old)
{
  NsPangoFcFontsetKey *key = g_slice_new (NsPangoFcFontsetKey);

  key->fontmap = old->fontmap;
  key->language = old->language;
  key->desc = ns_pango_font_description_copy (old->desc);
  key->matrix = old->matrix;
  key->pixelsize = old->pixelsize;
  key->resolution = old->resolution;
  key->variations = g_strdup (old->variations);
  key->features = g_strdup (old->features);

  if (old->context_key)
    key->context_key = NS_PANGO_FC_FONT_MAP_GET_CLASS (key->fontmap)->context_key_copy (key->fontmap,
										     old->context_key);
  else
    key->context_key = NULL;

  return key;
}

/**
 * ns_pango_fc_fontset_key_get_language:
 * @key: the fontset key
 *
 * Gets the language member of @key.
 *
 * Returns: the language
 *
 * Since: 1.24
 */
NsPangoLanguage *
ns_pango_fc_fontset_key_get_language (const NsPangoFcFontsetKey *key)
{
  return key->language;
}

/**
 * ns_pango_fc_fontset_key_get_matrix:
 * @key: the fontset key
 *
 * Gets the matrix member of @key.
 *
 * Returns: the matrix, which is owned by @key and should not be modified.
 *
 * Since: 1.24
 */
const NsPangoMatrix *
ns_pango_fc_fontset_key_get_matrix (const NsPangoFcFontsetKey *key)
{
  return &key->matrix;
}

/**
 * ns_pango_fc_fontset_key_get_context_key:
 * @key: the font key
 *
 * Gets the context key member of @key.
 *
 * Returns: the context key, which is owned by @key and should not be modified.
 *
 * Since: 1.24
 */
gpointer
ns_pango_fc_fontset_key_get_context_key (const NsPangoFcFontsetKey *key)
{
  return key->context_key;
}

/*
 * NsPangoFcFontKey
 */

static gboolean
ns_pango_fc_font_key_equal (const NsPangoFcFontKey *key_a,
			 const NsPangoFcFontKey *key_b)
{
  if (key_a->pattern == key_b->pattern &&
      ((key_a->variations == NULL && key_b->variations == NULL) ||
       (key_a->variations && key_b->variations && (strcmp (key_a->variations, key_b->variations) == 0))) &&
      ((key_a->features == NULL && key_b->features == NULL) ||
       (key_a->features && key_b->features && (strcmp (key_a->features, key_b->features) == 0))) &&
      0 == memcmp (&key_a->matrix, &key_b->matrix, 4 * sizeof (double)))
    {
      if (key_a->context_key && key_b->context_key)
	return NS_PANGO_FC_FONT_MAP_GET_CLASS (key_a->fontmap)->context_key_equal (key_a->fontmap,
										key_a->context_key,
										key_b->context_key);
      else
        return key_a->context_key == key_b->context_key;
    }
  else
    return FALSE;
}

static guint
ns_pango_fc_font_key_hash (const NsPangoFcFontKey *key)
{
    guint32 hash = FNV1_32_INIT;

    /* We do a bytewise hash on the doubles */
    hash = hash_bytes_fnv ((unsigned char *)(&key->matrix), sizeof (double) * 4, hash);

    if (key->variations)
      hash ^= g_str_hash (key->variations);

    if (key->features)
      hash ^= g_str_hash (key->features);

    if (key->context_key)
      hash ^= NS_PANGO_FC_FONT_MAP_GET_CLASS (key->fontmap)->context_key_hash (key->fontmap,
									    key->context_key);

    return (hash ^ GPOINTER_TO_UINT (key->pattern));
}

static void
ns_pango_fc_font_key_free (NsPangoFcFontKey *key)
{
  if (key->pattern)
    FcPatternDestroy (key->pattern);

  if (key->context_key)
    NS_PANGO_FC_FONT_MAP_GET_CLASS (key->fontmap)->context_key_free (key->fontmap,
								  key->context_key);

  g_free (key->variations);
  g_free (key->features);

  g_slice_free (NsPangoFcFontKey, key);
}

static NsPangoFcFontKey *
ns_pango_fc_font_key_copy (const NsPangoFcFontKey *old)
{
  NsPangoFcFontKey *key = g_slice_new (NsPangoFcFontKey);

  key->fontmap = old->fontmap;
  FcPatternReference (old->pattern);
  key->pattern = old->pattern;
  key->matrix = old->matrix;
  key->variations = g_strdup (old->variations);
  key->features = g_strdup (old->features);
  if (old->context_key)
    key->context_key = NS_PANGO_FC_FONT_MAP_GET_CLASS (key->fontmap)->context_key_copy (key->fontmap,
										     old->context_key);
  else
    key->context_key = NULL;

  return key;
}

static void
ns_pango_fc_font_key_init_from_key (NsPangoFcFontKey       *key,
                                 const NsPangoFcFontKey *orig)
{
  key->fontmap = orig->fontmap;
  key->pattern = orig->pattern;
  key->matrix = orig->matrix;
  key->variations = orig->variations;
  key->features = orig->features;
  key->context_key = orig->context_key;
}

static void
ns_pango_fc_font_key_init (NsPangoFcFontKey    *key,
			NsPangoFcFontMap    *fcfontmap,
			NsPangoFcFontsetKey *fontset_key,
			FcPattern         *pattern)
{
  key->fontmap = fcfontmap;
  key->pattern = pattern;
  key->matrix = *ns_pango_fc_fontset_key_get_matrix (fontset_key);
  key->variations = fontset_key->variations;
  key->features = fontset_key->features;
  key->context_key = ns_pango_fc_fontset_key_get_context_key (fontset_key);
}

/* Public API */

/**
 * ns_pango_fc_font_key_get_pattern:
 * @key: the font key
 *
 * Gets the fontconfig pattern member of @key.
 *
 * Returns: the pattern, which is owned by @key and should not be modified.
 *
 * Since: 1.24
 */
const FcPattern *
ns_pango_fc_font_key_get_pattern (const NsPangoFcFontKey *key)
{
  return key->pattern;
}

/**
 * ns_pango_fc_font_key_get_matrix:
 * @key: the font key
 *
 * Gets the matrix member of @key.
 *
 * Returns: the matrix, which is owned by @key and should not be modified.
 *
 * Since: 1.24
 */
const NsPangoMatrix *
ns_pango_fc_font_key_get_matrix (const NsPangoFcFontKey *key)
{
  return &key->matrix;
}

/**
 * ns_pango_fc_font_key_get_context_key:
 * @key: the font key
 *
 * Gets the context key member of @key.
 *
 * Returns: the context key, which is owned by @key and should not be modified.
 *
 * Since: 1.24
 */
gpointer
ns_pango_fc_font_key_get_context_key (const NsPangoFcFontKey *key)
{
  return key->context_key;
}

const char *
ns_pango_fc_font_key_get_variations (const NsPangoFcFontKey *key)
{
  return key->variations;
}

/*
 * NsPangoFcPatterns
 */

struct _PangoFcPatterns {
  NsPangoFcFontMap *fontmap;

  /* match and fontset are initialized in a thread,
   * and are protected by a mutex. The thread signals
   * the cond when match or fontset become available.
   */
  GMutex mutex;
  GCond cond;

  FcPattern *pattern;
  FcPattern *match;
  FcFontSet *fontset;
};

static FcFontSet *
font_set_copy (FcFontSet *fontset)
{
  FcFontSet *copy;
  int i;

  if (!fontset)
    return NULL;

  copy = malloc (sizeof (FcFontSet));
  copy->sfont = copy->nfont = fontset->nfont;
  copy->fonts = malloc (sizeof (FcPattern *) * copy->nfont);
  if (copy->nfont > 0)
    memcpy (copy->fonts, fontset->fonts, sizeof (FcPattern *) * copy->nfont);
  for (i = 0; i < copy->nfont; i++)
    FcPatternReference (copy->fonts[i]);

  return copy;
}

typedef enum {
  FC_INIT,
  FC_MATCH,
  FC_SORT,
  FC_END,
} FcOp;

typedef struct {
  FcOp op;
  FcConfig *config;
  FcFontSet *fonts;
  FcPattern *pattern;
  NsPangoFcPatterns *patterns;
} ThreadData;

static FcFontSet *ns_pango_fc_font_map_get_config_fonts (NsPangoFcFontMap *fcfontmap);

static ThreadData *
thread_data_new (FcOp             op,
                 NsPangoFcPatterns *patterns)
{
  ThreadData *td;

  td = g_new0 (ThreadData, 1);

  td->op = op;

  if (!patterns)
    return td;

  /* We don't want the fontmap dying on us */
  g_object_ref (patterns->fontmap);

  td->patterns = ns_pango_fc_patterns_ref (patterns);
  td->pattern = FcPatternDuplicate (patterns->pattern);

  td->config = FcConfigReference (ns_pango_fc_font_map_get_config (patterns->fontmap));
  td->fonts = font_set_copy (ns_pango_fc_font_map_get_config_fonts (patterns->fontmap));

  return td;
}

static void
thread_data_free (gpointer data)
{
  ThreadData *td = data;
  NsPangoFcFontMap *fontmap = td->patterns ? td->patterns->fontmap : NULL;

  g_clear_pointer (&td->fonts, FcFontSetDestroy);
  if (td->pattern)
    FcPatternDestroy (td->pattern);
  if (td->config)
    FcConfigDestroy (td->config);
  if (td->patterns)
    ns_pango_fc_patterns_unref (td->patterns);
  g_free (td);

  g_clear_object (&fontmap);
}

static gpointer
init_in_thread (gpointer task_data)
{
  ThreadData *td = task_data;
  gint64 before G_GNUC_UNUSED;

  before = NS_PANGO_TRACE_CURRENT_TIME;

  FcInit ();

  ns_pango_trace_mark (before, "FcInit", NULL);

  g_mutex_lock (&fc_init_mutex);
  fc_initialized = DEFAULT_CONFIG_INITIALIZED;
  g_cond_broadcast (&fc_init_cond);
  g_mutex_unlock (&fc_init_mutex);

  thread_data_free (td);

  return NULL;
}

static gpointer
sort_in_thread (gpointer task_data)
{
  ThreadData *td = task_data;
  FcResult result;
  FcFontSet *fontset;
  gint64 before G_GNUC_UNUSED;

  before = NS_PANGO_TRACE_CURRENT_TIME;

  fontset = FcFontSetSort (td->config,
                           &td->fonts, 1,
                           td->pattern,
                           FcTrue,
                           NULL,
                           &result);

  ns_pango_trace_mark (before, "FcFontSetSort", NULL);

  g_mutex_lock (&td->patterns->mutex);
  td->patterns->fontset = fontset;
  g_cond_signal (&td->patterns->cond);
  g_mutex_unlock (&td->patterns->mutex);

  thread_data_free (td);

  return NULL;
}

static gpointer
match_in_thread (gpointer task_data)
{
  ThreadData *td = task_data;
  FcResult result;
  FcPattern *match;
  gint64 before G_GNUC_UNUSED;

  before = NS_PANGO_TRACE_CURRENT_TIME;

  match = FcFontSetMatch (td->config,
                          &td->fonts, 1,
                          td->pattern,
                          &result);

  ns_pango_trace_mark (before, "FcFontSetMatch", NULL);

  g_mutex_lock (&td->patterns->mutex);
  td->patterns->match = match;
  g_cond_signal (&td->patterns->cond);
  g_mutex_unlock (&td->patterns->mutex);

  if (result == FcResultNoMatch)
    sort_in_thread (td);
  else
    thread_data_free (td);

  return NULL;
}

static gpointer
fc_thread_func (gpointer data)
{
  GAsyncQueue *queue = data;
  gboolean done = FALSE;

  while (!done)
    {
      ThreadData *td = g_async_queue_pop (queue);

      switch (td->op)
        {
        case FC_INIT:
          init_in_thread (td);
          break;

        case FC_MATCH:
          match_in_thread (td);
          break;

        case FC_SORT:
          sort_in_thread (td);
          break;

        case FC_END:
          thread_data_free (td);
          done = TRUE;
          break;

        default:
          g_assert_not_reached ();
        }
    }

  g_async_queue_unref (queue);

  ns_pango_trace_mark (NS_PANGO_TRACE_CURRENT_TIME, "end fontconfig thread", NULL);

  return NULL;
}

static NsPangoFcPatterns *
ns_pango_fc_patterns_new (FcPattern *pat, NsPangoFcFontMap *fontmap)
{
  NsPangoFcPatterns *pats;

  pat = uniquify_pattern (fontmap, pat);
  pats = g_hash_table_lookup (fontmap->priv->patterns_hash, pat);
  if (pats)
    return ns_pango_fc_patterns_ref (pats);

  pats = g_atomic_rc_box_new0 (NsPangoFcPatterns);

  pats->fontmap = fontmap;

  FcPatternReference (pat);
  pats->pattern = pat;

  g_mutex_init (&pats->mutex);
  g_cond_init (&pats->cond);

  g_async_queue_push (fontmap->priv->queue, thread_data_new (FC_MATCH, pats));

  g_hash_table_insert (fontmap->priv->patterns_hash, pats->pattern, pats);

  return pats;
}

static NsPangoFcPatterns *
ns_pango_fc_patterns_ref (NsPangoFcPatterns *pats)
{
  return g_atomic_rc_box_acquire (pats);
}

static void
free_patterns (gpointer data)
{
  NsPangoFcPatterns *pats = data;

  /* Only remove from fontmap hash if we are in it.  This is not necessarily
   * the case after a cache_clear() call. */
  if (pats->fontmap->priv->patterns_hash &&
      pats == g_hash_table_lookup (pats->fontmap->priv->patterns_hash, pats->pattern))
    g_hash_table_remove (pats->fontmap->priv->patterns_hash, pats->pattern);

  if (pats->pattern)
    FcPatternDestroy (pats->pattern);

  if (pats->match)
    FcPatternDestroy (pats->match);

  if (pats->fontset)
    FcFontSetDestroy (pats->fontset);

  g_cond_clear (&pats->cond);
  g_mutex_clear (&pats->mutex);
}

static void
ns_pango_fc_patterns_unref (NsPangoFcPatterns *pats)
{
  g_atomic_rc_box_release_full (pats, free_patterns);
}

static FcPattern *
ns_pango_fc_patterns_get_pattern (NsPangoFcPatterns *pats)
{
  return pats->pattern;
}

static gboolean
ns_pango_fc_is_supported_font_format (FcPattern* pattern)
{
  FcResult res;
  const char *file;
  const char *fontwrapper;
  const char *fontformat;

  /* Patterns without FC_FILE are problematic, since our caching is based
   * on filenames.
   */
  res = FcPatternGetString (pattern, FC_FILE, 0, (FcChar8 **)(void*)&file);
  if (res != FcResultMatch)
    return FALSE;

  /* Harfbuzz supports only SFNT fonts. */
  res = FcPatternGetString (pattern, FC_FONT_WRAPPER, 0, (FcChar8 **)(void*)&fontwrapper);
  if (res == FcResultMatch)
    return strcmp (fontwrapper, "SFNT") == 0;

  res = FcPatternGetString (pattern, FC_FONTFORMAT, 0, (FcChar8 **)(void*)&fontformat);
   if (res != FcResultMatch)
     return FALSE;

  /* FIXME: "CFF" is used for both CFF in OpenType and bare CFF files, but
   * HarfBuzz does not support the later and FontConfig does not seem
   * to have a way to tell them apart.
   */
  if (g_ascii_strcasecmp (fontformat, "TrueType") == 0 ||
      g_ascii_strcasecmp (fontformat, "CFF") == 0)
    return TRUE;

  return FALSE;
}

static FcPattern *
pattern_set_order (FcPattern *pat,
                   int        order)
{
  int o;

  if (FcPatternGetInteger (pat, FC_ORDER, 0, &o) == FcResultMatch &&
      o == order)
    {
      FcPatternReference (pat);
      return pat;
    }

  pat = FcPatternDuplicate (pat);
  FcPatternRemove (pat, FC_ORDER, 0);
  FcPatternAddInteger (pat, FC_ORDER, order);

  return pat;
}

static FcFontSet *
filter_by_format (FcFontSet **sets, int nsets)
{
  FcFontSet *result;

  result = FcFontSetCreate ();

  for (int set = 0; set < nsets; set++)
    {
      FcFontSet *fontset = sets[set];
      int i;

      if (!fontset)
        continue;

      for (i = 0; i < fontset->nfont; i++)
        {
          FcPattern *pat = fontset->fonts[i];

          if (!ns_pango_fc_is_supported_font_format (pat))
            continue;

          FcFontSetAdd (result, pattern_set_order (pat, set));
        }
    }

  return result;
}

static FcPattern *
ns_pango_fc_patterns_get_font_pattern (NsPangoFcPatterns *pats, int i, gboolean *prepare)
{
  FcPattern *match = NULL;
  FcFontSet *fontset = NULL;

  if (i == 0)
    {
      gint64 before G_GNUC_UNUSED;
      gboolean waited = FALSE;

      before = NS_PANGO_TRACE_CURRENT_TIME;

      g_mutex_lock (&pats->mutex);

      while (!pats->match && !pats->fontset)
        {
          waited = TRUE;
          g_cond_wait (&pats->cond, &pats->mutex);
        }

      match = pats->match;
      fontset = pats->fontset;

      g_mutex_unlock (&pats->mutex);

      if (waited)
        ns_pango_trace_mark (before, "wait for FcFontMatch", NULL);

      if (match)
        {
          *prepare = FALSE;
          return match;
        }
    }
  else
    {
      gint64 before G_GNUC_UNUSED;
      gboolean waited = FALSE;

      before = NS_PANGO_TRACE_CURRENT_TIME;

      if (!pats->fontset)
        g_async_queue_push (pats->fontmap->priv->queue, thread_data_new (FC_SORT, pats));

      g_mutex_lock (&pats->mutex);

      while (!pats->fontset)
        {
          waited = TRUE;
          g_cond_wait (&pats->cond, &pats->mutex);
        }

      fontset = pats->fontset;

      g_mutex_unlock (&pats->mutex);

      if (waited)
        ns_pango_trace_mark (before, "wait for FcFontSort", NULL);
    }

  if (fontset)
    {
      if (i < fontset->nfont)
        {
          *prepare = TRUE;
          return fontset->fonts[i];
        }
    }

  return NULL;
}


/*
 * NsPangoFcFontset
 */

static void              ns_pango_fc_fontset_finalize     (GObject                 *object);
static NsPangoLanguage *   ns_pango_fc_fontset_get_language (NsPangoFontset            *fontset);
static  NsPangoFont *      ns_pango_fc_fontset_get_font     (NsPangoFontset            *fontset,
							guint                    wc);
static void              ns_pango_fc_fontset_foreach      (NsPangoFontset            *fontset,
							NsPangoFontsetForeachFunc  func,
							gpointer                 data);

struct _PangoFcFontset
{
  NsPangoFontset parent_instance;

  NsPangoFcFontsetKey *key;

  NsPangoFcPatterns *patterns;
  int patterns_i;

  GPtrArray *fonts;
  GPtrArray *coverages;

  GList *cache_link;
};

typedef NsPangoFontsetClass NsPangoFcFontsetClass;

G_DEFINE_TYPE (NsPangoFcFontset, ns_pango_fc_fontset, NS_TYPE_PANGO_FONTSET)

static NsPangoFcFontset *
ns_pango_fc_fontset_new (NsPangoFcFontsetKey *key,
		      NsPangoFcPatterns   *patterns)
{
  NsPangoFcFontset *fontset;

  fontset = g_object_new (NS_PANGO_FC_TYPE_FONTSET, NULL);

  fontset->key = ns_pango_fc_fontset_key_copy (key);
  fontset->patterns = ns_pango_fc_patterns_ref (patterns);

  return fontset;
}

static NsPangoFcFontsetKey *
ns_pango_fc_fontset_get_key (NsPangoFcFontset *fontset)
{
  return fontset->key;
}

static gboolean
ns_pango_fc_pattern_weight (FcPattern *pattern, double *weight)
{
  FcValue value;

  if (FcPatternGet (pattern, FC_WEIGHT, 0, &value) != FcResultMatch)
    return FALSE;
  if (value.type == FcTypeInteger)
    *weight = value.u.i;
  else if (value.type == FcTypeDouble)
    *weight = value.u.d;
  else if (value.type == FcTypeRange)
    {
      double begin;
      if (!FcRangeGetDouble (value.u.r, &begin, weight))
        return FALSE;
    }
  else
    return FALSE;
  return TRUE;
}

static gboolean
ns_pango_fc_needs_synthetic_bold (FcPattern *request, FcPattern *font)
{
  double wanted, available;
  FcBool embolden;

  if (FcPatternGetBool (font, FC_EMBOLDEN, 0, &embolden) == FcResultMatch)
    return FALSE;
  return ns_pango_fc_pattern_weight (request, &wanted) &&
         ns_pango_fc_pattern_weight (font, &available) &&
         wanted >= FC_WEIGHT_DEMIBOLD && available < FC_WEIGHT_DEMIBOLD;
}

static NsPangoFont *
ns_pango_fc_fontset_load_next_font (NsPangoFcFontset *fontset)
{
  FcPattern *pattern, *font_pattern;
  NsPangoFont *font;
  gboolean prepare;

  pattern = ns_pango_fc_patterns_get_pattern (fontset->patterns);
  font_pattern = ns_pango_fc_patterns_get_font_pattern (fontset->patterns,
						     fontset->patterns_i++,
						     &prepare);
  if (G_UNLIKELY (!font_pattern))
    return NULL;

  if (prepare)
    {
      font_pattern = FcFontRenderPrepare (fontset->key->fontmap->priv->config, pattern, font_pattern);

      if (G_UNLIKELY (!font_pattern))
	return NULL;
    }

  if (ns_pango_fc_needs_synthetic_bold (pattern, font_pattern))
    {
      if (!prepare)
        {
          font_pattern = FcPatternDuplicate (font_pattern);
          if (G_UNLIKELY (!font_pattern))
            return NULL;
          prepare = TRUE;
        }
      FcPatternAddBool (font_pattern, FC_EMBOLDEN, FcTrue);
    }

  font = ns_pango_fc_font_map_new_font (fontset->key->fontmap,
                                     fontset->key,
                                     font_pattern);

  if (prepare)
    FcPatternDestroy (font_pattern);

  return font;
}

static NsPangoFont *
ns_pango_fc_fontset_get_font_at (NsPangoFcFontset *fontset,
			      unsigned int    i)
{
  while (i >= fontset->fonts->len)
    {
      NsPangoFont *font = ns_pango_fc_fontset_load_next_font (fontset);
      g_ptr_array_add (fontset->fonts, font);
      g_ptr_array_add (fontset->coverages, NULL);
      if (!font)
        return NULL;
    }

  return g_ptr_array_index (fontset->fonts, i);
}

static void
ns_pango_fc_fontset_class_init (NsPangoFcFontsetClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  NsPangoFontsetClass *fontset_class = NS_PANGO_FONTSET_CLASS (class);

  object_class->finalize = ns_pango_fc_fontset_finalize;

  fontset_class->get_font = ns_pango_fc_fontset_get_font;
  fontset_class->get_language = ns_pango_fc_fontset_get_language;
  fontset_class->foreach = ns_pango_fc_fontset_foreach;
}

static void
ns_pango_fc_fontset_init (NsPangoFcFontset *fontset)
{
  fontset->fonts = g_ptr_array_new ();
  fontset->coverages = g_ptr_array_new ();
}

static void
ns_pango_fc_fontset_finalize (GObject *object)
{
  NsPangoFcFontset *fontset = NS_PANGO_FC_FONTSET (object);
  unsigned int i;

  for (i = 0; i < fontset->fonts->len; i++)
  {
    NsPangoFont *font = g_ptr_array_index(fontset->fonts, i);
    if (font)
      g_object_unref (font);
  }
  g_ptr_array_free (fontset->fonts, TRUE);

  for (i = 0; i < fontset->coverages->len; i++)
    {
      NsPangoCoverage *coverage = g_ptr_array_index (fontset->coverages, i);
      if (coverage)
	g_object_unref (coverage);
    }
  g_ptr_array_free (fontset->coverages, TRUE);

  if (fontset->key)
    ns_pango_fc_fontset_key_free (fontset->key);

  if (fontset->patterns)
    ns_pango_fc_patterns_unref (fontset->patterns);

  G_OBJECT_CLASS (ns_pango_fc_fontset_parent_class)->finalize (object);
}

static NsPangoLanguage *
ns_pango_fc_fontset_get_language (NsPangoFontset  *fontset)
{
  NsPangoFcFontset *fcfontset = NS_PANGO_FC_FONTSET (fontset);

  return ns_pango_fc_fontset_key_get_language (ns_pango_fc_fontset_get_key (fcfontset));
}

static NsPangoFont *
ns_pango_fc_fontset_get_font (NsPangoFontset  *fontset,
			   guint          wc)
{
  NsPangoFcFontset *fcfontset = NS_PANGO_FC_FONTSET (fontset);
  NsPangoCoverageLevel best_level = NS_PANGO_COVERAGE_NONE;
  NsPangoCoverageLevel level;
  NsPangoFont *font;
  NsPangoCoverage *coverage;
  int result = -1;
  unsigned int i;

  for (i = 0;
       ns_pango_fc_fontset_get_font_at (fcfontset, i);
       i++)
    {
      coverage = g_ptr_array_index (fcfontset->coverages, i);

      if (coverage == NULL)
	{
	  font = g_ptr_array_index (fcfontset->fonts, i);

	  coverage = ns_pango_font_get_coverage (font, fcfontset->key->language);
	  g_ptr_array_index (fcfontset->coverages, i) = coverage;
	}

      level = ns_pango_coverage_get (coverage, wc);

      if (result == -1 || level > best_level)
	{
	  result = i;
	  best_level = level;
	  if (level == NS_PANGO_COVERAGE_EXACT)
	    break;
	}
    }

  if (G_UNLIKELY (result == -1))
    return NULL;

  font = g_ptr_array_index (fcfontset->fonts, result);
  return g_object_ref (font);
}

static void
ns_pango_fc_fontset_foreach (NsPangoFontset           *fontset,
			  NsPangoFontsetForeachFunc func,
			  gpointer                data)
{
  NsPangoFcFontset *fcfontset = NS_PANGO_FC_FONTSET (fontset);
  NsPangoFont *font;
  unsigned int i;

  for (i = 0;
       (font = ns_pango_fc_fontset_get_font_at (fcfontset, i));
       i++)
    {
      if ((*func) (fontset, font, data))
	return;
    }
}


/*
 * NsPangoFcFontMap
 */

static GType
ns_pango_fc_font_map_get_item_type (GListModel *list)
{
  return NS_TYPE_PANGO_FONT_FAMILY;
}

static guint
ns_pango_fc_font_map_get_n_items (GListModel *list)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (list);

  ensure_families (fcfontmap);

  return fcfontmap->priv->n_families;
}

static gpointer
ns_pango_fc_font_map_get_item (GListModel *list,
                            guint       position)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (list);

  ensure_families (fcfontmap);

  if (position < fcfontmap->priv->n_families)
    return g_object_ref (fcfontmap->priv->families[position]);

  return NULL;
}

static void
ns_pango_fc_font_map_list_model_init (GListModelInterface *iface)
{
  iface->get_item_type = ns_pango_fc_font_map_get_item_type;
  iface->get_n_items = ns_pango_fc_font_map_get_n_items;
  iface->get_item = ns_pango_fc_font_map_get_item;
}

G_DEFINE_ABSTRACT_TYPE_WITH_CODE (NsPangoFcFontMap, ns_pango_fc_font_map, NS_TYPE_PANGO_FONT_MAP,
                                  G_ADD_PRIVATE (NsPangoFcFontMap)
                                  G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL, ns_pango_fc_font_map_list_model_init))

static void
start_fontconfig_thread (NsPangoFcFontMap *fcfontmap)
{
  GThread *thread;

  g_mutex_lock (&fc_init_mutex);

  thread = g_thread_new ("[pango] fontconfig", fc_thread_func, g_async_queue_ref (fcfontmap->priv->queue));
  g_thread_unref (thread);

  if (fc_initialized == DEFAULT_CONFIG_NOT_INITIALIZED)
    {
      fc_initialized = DEFAULT_CONFIG_INITIALIZING;

      g_async_queue_push (fcfontmap->priv->queue, thread_data_new (FC_INIT, NULL));
    }

  g_mutex_unlock (&fc_init_mutex);
}

static void
wait_for_fc_init (void)
{
  gint64 before G_GNUC_UNUSED;
  gboolean waited = FALSE;

  before = NS_PANGO_TRACE_CURRENT_TIME;

  g_mutex_lock (&fc_init_mutex);
  while (fc_initialized < DEFAULT_CONFIG_INITIALIZED)
    {
      waited = TRUE;
      g_cond_wait (&fc_init_cond, &fc_init_mutex);
    }
  g_mutex_unlock (&fc_init_mutex);

  if (waited)
    ns_pango_trace_mark (before, "wait for FcInit", NULL);
}

static void
ns_pango_fc_font_map_init (NsPangoFcFontMap *fcfontmap)
{
  NsPangoFcFontMapPrivate *priv;

  priv = fcfontmap->priv = ns_pango_fc_font_map_get_instance_private (fcfontmap);

  priv->n_families = -1;

  priv->font_hash = g_hash_table_new ((GHashFunc)ns_pango_fc_font_key_hash,
				      (GEqualFunc)ns_pango_fc_font_key_equal);

  priv->fontset_hash = g_hash_table_new_full ((GHashFunc)ns_pango_fc_fontset_key_hash,
					      (GEqualFunc)ns_pango_fc_fontset_key_equal,
					      NULL,
					      (GDestroyNotify)g_object_unref);
  priv->fontset_cache = g_queue_new ();

  priv->patterns_hash = g_hash_table_new (NULL, NULL);

  priv->pattern_hash = g_hash_table_new_full ((GHashFunc) FcPatternHash,
					      (GEqualFunc) FcPatternEqual,
					      (GDestroyNotify) FcPatternDestroy,
					      NULL);

  priv->font_face_data_hash = g_hash_table_new_full ((GHashFunc)ns_pango_fc_font_face_data_hash,
						     (GEqualFunc)ns_pango_fc_font_face_data_equal,
						     (GDestroyNotify)ns_pango_fc_font_face_data_free,
						     NULL);
  priv->dpi = -1;

  priv->queue = g_async_queue_new ();

  start_fontconfig_thread (fcfontmap);
}

static void
ns_pango_fc_font_map_fini (NsPangoFcFontMap *fcfontmap)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  int i;

  g_clear_pointer (&priv->fonts, FcFontSetDestroy);

  g_queue_free (priv->fontset_cache);
  priv->fontset_cache = NULL;

  g_hash_table_destroy (priv->fontset_hash);
  priv->fontset_hash = NULL;

  g_hash_table_destroy (priv->patterns_hash);
  priv->patterns_hash = NULL;

  g_hash_table_destroy (priv->font_hash);
  priv->font_hash = NULL;

  g_hash_table_destroy (priv->font_face_data_hash);
  priv->font_face_data_hash = NULL;

  g_hash_table_destroy (priv->pattern_hash);
  priv->pattern_hash = NULL;

  for (i = 0; i < priv->n_families; i++)
    g_object_unref (priv->families[i]);
  g_free (priv->families);
  priv->n_families = -1;
  priv->families = NULL;

  g_async_queue_push (fcfontmap->priv->queue, thread_data_new (FC_END, NULL));

  g_async_queue_unref (priv->queue);
  priv->queue = NULL;
}

static void
ns_pango_fc_font_map_class_init (NsPangoFcFontMapClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  NsPangoFontMapClass *fontmap_class = NS_PANGO_FONT_MAP_CLASS (class);
  NsPangoFontMapClassPrivate *pclass;

  object_class->finalize = ns_pango_fc_font_map_finalize;
  fontmap_class->load_font = ns_pango_fc_font_map_load_font;
  fontmap_class->load_fontset = ns_pango_fc_font_map_load_fontset;
  fontmap_class->list_families = ns_pango_fc_font_map_list_families;
  fontmap_class->get_family = ns_pango_fc_font_map_get_family;
  fontmap_class->get_face = ns_pango_fc_font_map_get_face;
  fontmap_class->shape_engine_type = NS_PANGO_RENDER_TYPE_FC;
  fontmap_class->changed = ns_pango_fc_font_map_changed;

  pclass = g_type_class_get_private ((GTypeClass *) class, NS_TYPE_PANGO_FONT_MAP);

  pclass->reload_font = ns_pango_fc_font_map_reload_font;
  pclass->add_font_file = ns_pango_fc_font_map_add_font_file;
}


/**
 * ns_pango_fc_font_map_find_decoder:
 * @fcfontmap: The `NsPangoFcFontMap` to use.
 * @pattern: The `FcPattern` to find the decoder for.
 *
 * Finds the decoder to use for @pattern.
 *
 * Decoders can be added to a font map using
 * [method@NsPangoFc.FontMap.add_decoder_find_func].
 *
 * Returns: (transfer full) (nullable): a newly created `NsPangoFcDecoder`
 *   object or %NULL if no decoder is set for @pattern.
 *
 * Since: 1.26
 */
NsPangoFcDecoder *
ns_pango_fc_font_map_find_decoder (NsPangoFcFontMap *fcfontmap,
                                FcPattern      *pattern)
{
  GSList *l;

  g_return_val_if_fail (NS_PANGO_IS_FC_FONT_MAP (fcfontmap), NULL);
  g_return_val_if_fail (pattern != NULL, NULL);

  for (l = fcfontmap->priv->findfuncs; l && l->data; l = l->next)
    {
      NsPangoFcFindFuncInfo *info = l->data;
      NsPangoFcDecoder *decoder;

      decoder = info->findfunc (pattern, info->user_data);
      if (decoder)
	return decoder;
    }

  return NULL;
}

static void
ns_pango_fc_font_map_finalize (GObject *object)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (object);

  ns_pango_fc_font_map_shutdown (fcfontmap);

  if (fcfontmap->substitute_destroy)
    fcfontmap->substitute_destroy (fcfontmap->substitute_data);

  if (fcfontmap->priv->config)
    FcConfigDestroy (fcfontmap->priv->config);

  G_OBJECT_CLASS (ns_pango_fc_font_map_parent_class)->finalize (object);
}

/* Add a mapping from key to fcfont */
static void
ns_pango_fc_font_map_add (NsPangoFcFontMap *fcfontmap,
		       NsPangoFcFontKey *key,
		       NsPangoFcFont    *fcfont)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFontKey *key_copy;

  key_copy = ns_pango_fc_font_key_copy (key);
  _ns_pango_fc_font_set_font_key (fcfont, key_copy);
  g_hash_table_insert (priv->font_hash, key_copy, fcfont);
}

static NsPangoFont *
ns_pango_fc_font_map_reload_font (NsPangoFontMap *fontmap,
                               NsPangoFont    *font,
                               double        scale,
                               NsPangoContext *context,
                               const char   *variations)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (fontmap);
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);
  NsPangoFcFontKey key;
  FcPattern *pattern = NULL;
  double point_size;
  double pixel_size;
  NsPangoFont *scaled;

  ns_pango_fc_font_key_init_from_key (&key, _ns_pango_fc_font_get_font_key (fcfont));

  if (scale != 1.0)
    {
      pattern = FcPatternDuplicate (key.pattern);

      if (FcPatternGetDouble (pattern, FC_SIZE, 0, &point_size) != FcResultMatch)
        point_size = 13.;

      if (FcPatternGetDouble (pattern, FC_PIXEL_SIZE, 0, &pixel_size) != FcResultMatch)
        {
          double dpi;

          if (FcPatternGetDouble (pattern, FC_DPI, 0, &dpi) != FcResultMatch)
            dpi = 72.;

          pixel_size = point_size * dpi / 72.;
        }

      FcPatternRemove (pattern, FC_PIXEL_SIZE, 0);
      FcPatternAddDouble (pattern, FC_PIXEL_SIZE, pixel_size * scale);
    }

  if (context)
    {
      get_context_matrix (context, &key.matrix);
      if (NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->context_key_get)
        key.context_key = (gpointer) NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->context_key_get (fcfontmap, context);
    }

  if (variations)
    {
      if (!pattern)
        pattern = FcPatternDuplicate (key.pattern);

      FcPatternRemove (pattern, FC_FONT_VARIATIONS, 0);
      FcPatternAddString (pattern, FC_FONT_VARIATIONS, (FcChar8*) variations);

      key.variations = (char *) variations;
    }

  if (pattern)
    key.pattern = uniquify_pattern (fcfontmap, pattern);

  scaled = ns_pango_fc_font_map_new_font_from_key (fcfontmap, &key);

  if (pattern)
    FcPatternDestroy (pattern);

  return scaled;
}

/* Remove mapping from fcfont->key to fcfont */
/* Closely related to shutdown_font() */
void
_ns_pango_fc_font_map_remove (NsPangoFcFontMap *fcfontmap,
			   NsPangoFcFont    *fcfont)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFontKey *key;

  key = _ns_pango_fc_font_get_font_key (fcfont);
  if (key)
    {
      /* Only remove from fontmap hash if we are in it.  This is not necessarily
       * the case after a cache_clear() call. */
      if (priv->font_hash &&
	  fcfont == g_hash_table_lookup (priv->font_hash, key))
        {
	  g_hash_table_remove (priv->font_hash, key);
	}
      _ns_pango_fc_font_set_font_key (fcfont, NULL);
      ns_pango_fc_font_key_free (key);
    }
}

static NsPangoFcFamily *
create_family (NsPangoFcFontMap *fcfontmap,
	       const char     *family_name,
	       int             spacing,
               gboolean        variable)
{
  NsPangoFcFamily *family = g_object_new (NS_PANGO_FC_TYPE_FAMILY, NULL);
  family->fontmap = fcfontmap;
  family->family_name = g_strdup (family_name);
  family->spacing = spacing;
  family->variable = variable;
  family->patterns = FcFontSetCreate ();

  return family;
}

static gboolean
is_alias_family (const char *family_name)
{
  switch (family_name[0])
    {
    case 'c':
    case 'C':
      return (g_ascii_strcasecmp (family_name, "cursive") == 0);
    case 'f':
    case 'F':
      return (g_ascii_strcasecmp (family_name, "fantasy") == 0);
    case 'm':
    case 'M':
      return (g_ascii_strcasecmp (family_name, "monospace") == 0);
    case 's':
    case 'S':
      return (g_ascii_strcasecmp (family_name, "sans") == 0 ||
	      g_ascii_strcasecmp (family_name, "serif") == 0 ||
	      g_ascii_strcasecmp (family_name, "system-ui") == 0);
    default:
      return FALSE;
    }

  return FALSE;
}

static int
compare_font_family_names (const void *a, const void *b)
{
  const NsPangoFcFamily *family_a = *(const NsPangoFcFamily **)a;
  const NsPangoFcFamily *family_b = *(const NsPangoFcFamily **)b;
  
  return g_strcmp0 (family_a->family_name, family_b->family_name);
}

static void
ensure_families (NsPangoFcFontMap *fcfontmap)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  FcFontSet *fontset;
  int i;
  int count;

  wait_for_fc_init ();

  if (priv->n_families < 0)
    {
      FcObjectSet *os = FcObjectSetBuild (FC_FAMILY, FC_SPACING, FC_STYLE, FC_WEIGHT, FC_WIDTH, FC_SLANT,
                                          FC_VARIABLE,
                                          FC_FONTFORMAT,
                                          NULL);
      FcPattern *pat = FcPatternCreate ();
      GHashTable *temp_family_hash;
      FcFontSet *fonts;

      fonts = ns_pango_fc_font_map_get_config_fonts (fcfontmap);
      fontset = FcFontSetList (priv->config, &fonts, 1, pat, os);

      FcPatternDestroy (pat);
      FcObjectSetDestroy (os);

      priv->families = g_new (NsPangoFcFamily *, fontset->nfont + 4); /* 4 standard aliases */
      temp_family_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

      count = 0;
      for (i = 0; i < fontset->nfont; i++)
	{
	  char *s;
	  FcResult res;
	  int spacing;
          int variable;
	  NsPangoFcFamily *temp_family;

	  res = FcPatternGetString (fontset->fonts[i], FC_FAMILY, 0, (FcChar8 **)(void*)&s);
	  g_assert (res == FcResultMatch);

	  temp_family = g_hash_table_lookup (temp_family_hash, s);
	  if (!is_alias_family (s) && !temp_family)
	    {
	      res = FcPatternGetInteger (fontset->fonts[i], FC_SPACING, 0, &spacing);
	      g_assert (res == FcResultMatch || res == FcResultNoMatch);
	      if (res == FcResultNoMatch)
		spacing = FC_PROPORTIONAL;

	      temp_family = create_family (fcfontmap, s, spacing, FALSE);
	      g_hash_table_insert (temp_family_hash, g_strdup (s), temp_family);
	      priv->families[count++] = temp_family;
	    }

	  if (temp_family)
	    {
              variable = FALSE;
              res = FcPatternGetBool (fontset->fonts[i], FC_VARIABLE, 0, &variable);
              if (res == FcResultMatch && variable)
                temp_family->variable = TRUE;

	      FcPatternReference (fontset->fonts[i]);
	      FcFontSetAdd (temp_family->patterns, fontset->fonts[i]);
	    }
	}

      FcFontSetDestroy (fontset);
      g_hash_table_destroy (temp_family_hash);

      priv->families[count++] = create_family (fcfontmap, "Sans", FC_PROPORTIONAL, FALSE);
      priv->families[count++] = create_family (fcfontmap, "Serif", FC_PROPORTIONAL, FALSE);
      priv->families[count++] = create_family (fcfontmap, "Monospace", FC_MONO, FALSE);
      priv->families[count++] = create_family (fcfontmap, "System-ui", FC_PROPORTIONAL, FALSE);

      qsort (priv->families, count, sizeof (NsPangoFcFamily *), compare_font_family_names);

      priv->n_families = count;
    }
}


static void
ns_pango_fc_font_map_list_families (NsPangoFontMap      *fontmap,
				 NsPangoFontFamily ***families,
				 int               *n_families)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (fontmap);
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;

  if (priv->closed)
    {
      if (families)
	*families = NULL;
      if (n_families)
	*n_families = 0;

      return;
    }

  ensure_families (fcfontmap);

  if (n_families)
    *n_families = priv->n_families;

  if (families)
    *families = g_memdup2 (priv->families, priv->n_families * sizeof (NsPangoFontFamily *));
}

static NsPangoFontFamily *
ns_pango_fc_font_map_get_family (NsPangoFontMap *fontmap,
                              const char   *name)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (fontmap);
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  int i;

  if (priv->closed)
    return NULL;

  ensure_families (fcfontmap);

  for (i = 0; i < priv->n_families; i++)
    {
      NsPangoFontFamily *family = NS_PANGO_FONT_FAMILY (priv->families[i]);
      if (strcmp (name, ns_pango_font_family_get_name (family)) == 0)
        return family;
    }

  return NULL;
}

static double
ns_pango_fc_convert_weight_to_fc (NsPangoWeight ns_pango_weight)
{
  return FcWeightFromOpenTypeDouble (ns_pango_weight);
}

static int
ns_pango_fc_convert_slant_to_fc (NsPangoStyle ns_pango_style)
{
  switch (ns_pango_style)
    {
    case NS_PANGO_STYLE_NORMAL:
      return FC_SLANT_ROMAN;
    case NS_PANGO_STYLE_ITALIC:
      return FC_SLANT_ITALIC;
    case NS_PANGO_STYLE_OBLIQUE:
      return FC_SLANT_OBLIQUE;
    default:
      return FC_SLANT_ROMAN;
    }
}

static double
ns_pango_fc_convert_width_to_fc (NsPangoWidth ns_pango_width)
{
  return (double) ns_pango_width / 10.0;
}

static FcPattern *
ns_pango_fc_make_pattern (const  NsPangoFontDescription *description,
		       NsPangoLanguage               *language,
		       int                          pixel_size,
		       double                       dpi,
                       const char                  *variations,
                       const char                  *features)
{
  FcPattern *pattern;
  const char *prgname;
  int slant;
  double weight;
  NsPangoGravity gravity;
  NsPangoVariant variant;
  NsPangoFontColor color;
  char **families;
  int i;
  double width;
  char *combined_features;

  prgname = g_get_prgname ();
  slant = ns_pango_fc_convert_slant_to_fc (ns_pango_font_description_get_style (description));
  weight = ns_pango_fc_convert_weight_to_fc (ns_pango_font_description_get_weight (description));
  width = ns_pango_fc_convert_width_to_fc (ns_pango_font_description_get_width (description));

  gravity = ns_pango_font_description_get_gravity (description);
  variant = ns_pango_font_description_get_variant (description);
  color = ns_pango_font_description_get_color (description);

  /* The reason for passing in FC_SIZE as well as FC_PIXEL_SIZE is
   * to work around a bug in libgnomeprint where it doesn't look
   * for FC_PIXEL_SIZE. See http://bugzilla.gnome.org/show_bug.cgi?id=169020
   *
   * The reason for passing FC_ORDER == 1000 is that we want
   * to prefer application fonts over system fonts regardless of version.
   * To make that happen, we increase the order of the pattern by 1 everytime
   * we override an existing face with a newly added pattern.
   *
   * Putting FC_SIZE in here slightly reduces the efficiency
   * of caching of patterns and fonts when working with multiple different
   * dpi values.
   *
   * Do not pass FC_VERTICAL_LAYOUT true as HarfBuzz shaping assumes false.
   */
  pattern = FcPatternBuild (NULL,
                            NS_PANGO_FC_VERSION, FcTypeInteger, ns_pango_version(),
                            FC_WEIGHT, FcTypeDouble, weight,
                            FC_SLANT,  FcTypeInteger, slant,
                            FC_WIDTH,  FcTypeDouble, width,
                            FC_VARIABLE,  FcTypeBool, FcDontCare,
                            FC_DPI, FcTypeDouble, dpi,
                            FC_SIZE,  FcTypeDouble,  pixel_size * (72. / 1024. / dpi),
                            FC_PIXEL_SIZE,  FcTypeDouble,  pixel_size / 1024.,
                            FC_ORDER, FcTypeInteger, 1000,
                            FC_COLOR, FcTypeBool, color,
                            NULL);

  if (variations)
    FcPatternAddString (pattern, FC_FONT_VARIATIONS, (FcChar8*) variations);

  if (ns_pango_font_description_get_family (description))
    {
      families = g_strsplit (ns_pango_font_description_get_family (description), ",", -1);

      for (i = 0; families[i]; i++)
        FcPatternAddString (pattern, FC_FAMILY, (FcChar8*) families[i]);

      g_strfreev (families);
    }

  if (language)
    FcPatternAddString (pattern, FC_LANG, (FcChar8 *) ns_pango_language_to_string (language));

  if (gravity != NS_PANGO_GRAVITY_SOUTH)
    {
      GEnumValue *value = g_enum_get_value (get_gravity_class (), gravity);
      FcPatternAddString (pattern, NS_PANGO_FC_GRAVITY, (FcChar8*) value->value_nick);
    }

  if (prgname)
    FcPatternAddString (pattern, FC_PRGNAME, (FcChar8*) prgname);

  combined_features = ns_pango_combine_features (variant, features);
  if (*combined_features)
    {
      char **feat = g_strsplit (combined_features, ",", -1);

      for (int i = 0; feat[i]; i++)
        FcPatternAddString (pattern, FC_FONT_FEATURES, (FcChar8*) feat[i]);

      g_strfreev (feat);
    }

  g_free (combined_features);

  return pattern;
}

static FcPattern *
uniquify_pattern (NsPangoFcFontMap *fcfontmap,
		  FcPattern      *pattern)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  FcPattern *old_pattern;

  old_pattern = g_hash_table_lookup (priv->pattern_hash, pattern);
  if (old_pattern)
    {
      return old_pattern;
    }
  else
    {
      FcPatternReference (pattern);
      g_hash_table_insert (priv->pattern_hash, pattern, pattern);
      return pattern;
    }
}

static NsPangoFont *
ns_pango_fc_font_map_new_font_from_key (NsPangoFcFontMap *fcfontmap,
                                     NsPangoFcFontKey *key)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFontMapClass *class = NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap);
  NsPangoFcFont *fcfont;

  if (priv->closed)
    return NULL;

  fcfont = g_hash_table_lookup (priv->font_hash, key);
  if (fcfont)
    return g_object_ref (NS_PANGO_FONT (fcfont));

  class = NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap);

  if (class->create_font)
    fcfont = class->create_font (fcfontmap, key);
  else
    g_warning ("%s needs to implement create_font", G_OBJECT_TYPE_NAME (fcfontmap));

  if (!fcfont)
    return NULL;

  ns_pango_fc_font_set_face (fcfont,
                          ns_pango_fc_font_map_get_face (NS_PANGO_FONT_MAP (fcfontmap),
                                                      NS_PANGO_FONT (fcfont)));
  ns_pango_fc_font_map_add (fcfontmap, key, fcfont);

  return (NsPangoFont *)fcfont;
}

static NsPangoFont *
ns_pango_fc_font_map_new_font (NsPangoFcFontMap    *fcfontmap,
			    NsPangoFcFontsetKey *fontset_key,
			    FcPattern         *match)
{
  NsPangoFcFontMapClass *class;
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  FcPattern *pattern;
  NsPangoFcFont *fcfont;
  NsPangoFcFontKey key;

  if (priv->closed)
    return NULL;

  match = uniquify_pattern (fcfontmap, match);

  ns_pango_fc_font_key_init (&key, fcfontmap, fontset_key, match);

  fcfont = g_hash_table_lookup (priv->font_hash, &key);
  if (fcfont)
    return g_object_ref (NS_PANGO_FONT (fcfont));

  class = NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap);

  if (class->create_font)
    {
      fcfont = class->create_font (fcfontmap, &key);
    }
  else
    {
      const NsPangoMatrix *ns_pango_matrix = ns_pango_fc_fontset_key_get_matrix (fontset_key);
      FcMatrix fc_matrix, *fc_matrix_val;
      int i;

      /* Fontconfig has the Y axis pointing up, Pango, down.
       */
      fc_matrix.xx = ns_pango_matrix->xx;
      fc_matrix.xy = - ns_pango_matrix->xy;
      fc_matrix.yx = - ns_pango_matrix->yx;
      fc_matrix.yy = ns_pango_matrix->yy;

      pattern = FcPatternDuplicate (match);

      for (i = 0; FcPatternGetMatrix (pattern, FC_MATRIX, i, &fc_matrix_val) == FcResultMatch; i++)
	FcMatrixMultiply (&fc_matrix, &fc_matrix, fc_matrix_val);

      FcPatternDel (pattern, FC_MATRIX);
      FcPatternAddMatrix (pattern, FC_MATRIX, &fc_matrix);

      fcfont = class->new_font (fcfontmap, uniquify_pattern (fcfontmap, pattern));

      FcPatternDestroy (pattern);
    }

  if (!fcfont)
    return NULL;

  ns_pango_fc_font_set_face (fcfont,
                          ns_pango_fc_font_map_get_face (NS_PANGO_FONT_MAP (fcfontmap),
                                                      NS_PANGO_FONT (fcfont)));

  /* In case the backend didn't set the fontmap */
  if (!fcfont->fontmap)
    g_object_set (fcfont,
		  "fontmap", fcfontmap,
		  NULL);

  /* cache it on fontmap */
  ns_pango_fc_font_map_add (fcfontmap, &key, fcfont);

  return (NsPangoFont *)fcfont;
}

static NsPangoFontFace *
ns_pango_fc_font_map_get_face (NsPangoFontMap *fontmap,
                            NsPangoFont    *font)
{
  NsPangoFcFont *fcfont = NS_PANGO_FC_FONT (font);
  FcResult res;
  const char *s;
  NsPangoFcFamily *family;

  res = FcPatternGetString (fcfont->font_pattern, FC_FAMILY, 0, (FcChar8 **) &s);
  g_assert (res == FcResultMatch);

  family = (NsPangoFcFamily *) ns_pango_fc_font_map_get_family (fontmap, s);
  if (family)
    {
      ensure_faces (family);

      for (int i = 0; i < family->n_faces; i++)
        {
          if (compare_face_pattern (family->faces[i]->pattern, fcfont->font_pattern) == 0)
            return NS_PANGO_FONT_FACE (family->faces[i]);
        }
    }

  return NULL;
}

static void
ns_pango_fc_default_substitute (NsPangoFcFontMap    *fontmap,
			     NsPangoFcFontsetKey *fontsetkey,
			     FcPattern         *pattern)
{
  if (NS_PANGO_FC_FONT_MAP_GET_CLASS (fontmap)->fontset_key_substitute)
    NS_PANGO_FC_FONT_MAP_GET_CLASS (fontmap)->fontset_key_substitute (fontmap, fontsetkey, pattern);
  else if (NS_PANGO_FC_FONT_MAP_GET_CLASS (fontmap)->default_substitute)
    NS_PANGO_FC_FONT_MAP_GET_CLASS (fontmap)->default_substitute (fontmap, pattern);
}

static double
ns_pango_fc_font_map_get_resolution (NsPangoFcFontMap *fcfontmap,
				  NsPangoContext   *context)
{
  if (NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->get_resolution)
    return NS_PANGO_FC_FONT_MAP_GET_CLASS (fcfontmap)->get_resolution (fcfontmap, context);

  if (fcfontmap->priv->dpi < 0)
    {
      FcResult result = FcResultNoMatch;
      FcPattern *tmp = FcPatternBuild (NULL,
				       FC_FAMILY, FcTypeString, "Sans",
				       FC_SIZE,   FcTypeDouble, 10.,
				       NULL);
      if (tmp)
	{
	  ns_pango_fc_default_substitute (fcfontmap, NULL, tmp);
	  result = FcPatternGetDouble (tmp, FC_DPI, 0, &fcfontmap->priv->dpi);
	  FcPatternDestroy (tmp);
	}

      if (result != FcResultMatch)
	{
	  g_warning ("Error getting DPI from fontconfig, using 72.0");
	  fcfontmap->priv->dpi = 72.0;
	}
    }

  return fcfontmap->priv->dpi;
}

static FcPattern *
ns_pango_fc_fontset_key_make_pattern (NsPangoFcFontsetKey *key)
{
  return ns_pango_fc_make_pattern (key->desc,
				key->language,
				key->pixelsize,
				key->resolution,
                                key->variations,
                                key->features);
}

static NsPangoFcPatterns *
ns_pango_fc_font_map_get_patterns (NsPangoFontMap      *fontmap,
				NsPangoFcFontsetKey *key)
{
  NsPangoFcFontMap *fcfontmap = (NsPangoFcFontMap *)fontmap;
  NsPangoFcPatterns *patterns;
  FcPattern *pattern;

  wait_for_fc_init ();

  pattern = ns_pango_fc_fontset_key_make_pattern (key);
  ns_pango_fc_default_substitute (fcfontmap, key, pattern);

  patterns = ns_pango_fc_patterns_new (pattern, fcfontmap);

  FcPatternDestroy (pattern);

  return patterns;
}

static gboolean
get_first_font (NsPangoFontset  *fontset G_GNUC_UNUSED,
		NsPangoFont     *font,
		gpointer       data)
{
  *(NsPangoFont **)data = font;

  return TRUE;
}

static NsPangoFont *
ns_pango_fc_font_map_load_font (NsPangoFontMap               *fontmap,
			     NsPangoContext               *context,
			     const NsPangoFontDescription *description)
{
  NsPangoLanguage *language;
  NsPangoFontset *fontset;
  NsPangoFont *font = NULL;

  if (context)
    language = ns_pango_context_get_language (context);
  else
    language = NULL;

  fontset = ns_pango_font_map_load_fontset (fontmap, context, description, language);

  if (fontset)
    {
      ns_pango_fontset_foreach (fontset, get_first_font, &font);

      if (font)
	g_object_ref (font);

      g_object_unref (fontset);
    }

  return font;
}

static void
ns_pango_fc_fontset_cache (NsPangoFcFontset *fontset,
			NsPangoFcFontMap *fcfontmap)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  GQueue *cache = priv->fontset_cache;

  if (fontset->cache_link)
    {
      if (fontset->cache_link == cache->head)
        return;

      /* Already in cache, move to head
       */
      if (fontset->cache_link == cache->tail)
	cache->tail = fontset->cache_link->prev;

      cache->head = g_list_remove_link (cache->head, fontset->cache_link);
      cache->length--;
    }
  else
    {
      /* Add to cache initially
       */
      if (cache->length == FONTSET_CACHE_SIZE)
	{
	  NsPangoFcFontset *tmp_fontset = g_queue_pop_tail (cache);
	  tmp_fontset->cache_link = NULL;
	  g_hash_table_remove (priv->fontset_hash, tmp_fontset->key);
	}

      fontset->cache_link = g_list_prepend (NULL, fontset);
    }

  g_queue_push_head_link (cache, fontset->cache_link);
}

static NsPangoFontset *
ns_pango_fc_font_map_load_fontset (NsPangoFontMap                 *fontmap,
				NsPangoContext                 *context,
				const NsPangoFontDescription   *desc,
				NsPangoLanguage                *language)
{
  NsPangoFcFontMap *fcfontmap = (NsPangoFcFontMap *)fontmap;
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFontset *fontset;
  NsPangoFcFontsetKey key;

  ns_pango_fc_fontset_key_init (&key, fcfontmap, context, desc, language);

  fontset = g_hash_table_lookup (priv->fontset_hash, &key);

  if (G_UNLIKELY (!fontset))
    {
      NsPangoFcPatterns *patterns = ns_pango_fc_font_map_get_patterns (fontmap, &key);

      if (!patterns)
	return NULL;

      fontset = ns_pango_fc_fontset_new (&key, patterns);
      g_hash_table_insert (priv->fontset_hash, ns_pango_fc_fontset_get_key (fontset), fontset);

      ns_pango_fc_patterns_unref (patterns);
    }

  ns_pango_fc_fontset_cache (fontset, fcfontmap);

  ns_pango_font_description_free (key.desc);
  g_free (key.variations);
  g_free (key.features);

  return g_object_ref (NS_PANGO_FONTSET (fontset));
}

/**
 * ns_pango_fc_font_map_cache_clear:
 * @fcfontmap: a `NsPangoFcFontMap`
 *
 * Clear all cached information and fontsets for this font map.
 *
 * This should be called whenever there is a change in the
 * output of the default_substitute() virtual function of the
 * font map, or if fontconfig has been reinitialized to new
 * configuration.
 *
 * Since: 1.4
 */
void
ns_pango_fc_font_map_cache_clear (NsPangoFcFontMap *fcfontmap)
{
  guint removed, added;

  if (G_UNLIKELY (fcfontmap->priv->closed))
    return;

  removed = fcfontmap->priv->n_families;

  ns_pango_fc_font_map_fini (fcfontmap);
  ns_pango_fc_font_map_init (fcfontmap);

  ensure_families (fcfontmap);

  added = fcfontmap->priv->n_families;

  g_list_model_items_changed (G_LIST_MODEL (fcfontmap), 0, removed, added);
  if (removed != added)
    g_object_notify (G_OBJECT (fcfontmap), "n-items");

  ns_pango_font_map_changed (NS_PANGO_FONT_MAP (fcfontmap));
}

static void
ns_pango_fc_font_map_changed (NsPangoFontMap *fontmap)
{
  /* we emit GListModel::changed in ns_pango_fc_font_map_cache_clear() */
}

/**
 * ns_pango_fc_font_map_config_changed:
 * @fcfontmap: a `NsPangoFcFontMap`
 *
 * Informs font map that the fontconfig configuration (i.e.,
 * the `FcConfig` object) used by this font map has changed.
 *
 * This currently calls [method@NsPangoFc.FontMap.cache_clear] which
 * ensures that list of fonts, etc will be regenerated using the
 * updated configuration.
 *
 * Since: 1.38
 */
void
ns_pango_fc_font_map_config_changed (NsPangoFcFontMap *fcfontmap)
{
  ns_pango_fc_font_map_cache_clear (fcfontmap);
}

static void
forget_family_faces (NsPangoFcFamily *fcfamily)
{
  for (int i = 0; i < fcfamily->n_faces; i++)
    {
      fcfamily->faces[i]->family = NULL;
      g_object_unref (fcfamily->faces[i]);
    }
  g_clear_pointer (&fcfamily->faces, g_free);
  fcfamily->n_faces = -1;
}

static gboolean
add_family_pattern (NsPangoFcFontMap *fcfontmap,
                    FcPattern        *pattern)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFamily *family = NULL;
  gboolean created = FALSE;
  const char *name;
  FcBool variable;
  int spacing;

  if (FcPatternGetString (pattern, FC_FAMILY, 0, (FcChar8 **)(void*)&name) != FcResultMatch ||
      is_alias_family (name))
    return FALSE;

  for (int i = 0; i < priv->n_families && !family; i++)
    if (strcmp (priv->families[i]->family_name, name) == 0)
      family = priv->families[i];

  if (!family)
    {
      if (FcPatternGetInteger (pattern, FC_SPACING, 0, &spacing) != FcResultMatch)
        spacing = FC_PROPORTIONAL;
      family = create_family (fcfontmap, name, spacing, FALSE);
      priv->families = g_renew (NsPangoFcFamily *, priv->families, priv->n_families + 1);
      priv->families[priv->n_families++] = family;
      created = TRUE;
    }

  if (FcPatternGetBool (pattern, FC_VARIABLE, 0, &variable) == FcResultMatch && variable)
    family->variable = TRUE;

  FcPatternReference (pattern);
  FcFontSetAdd (family->patterns, pattern);
  if (family->n_faces >= 0)
    forget_family_faces (family);

  return created;
}

static void
forget_fontsets (NsPangoFcFontMap *fcfontmap)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFontset *fontset;

  while ((fontset = g_queue_pop_head (priv->fontset_cache)))
    fontset->cache_link = NULL;

  g_hash_table_remove_all (priv->fontset_hash);
  g_hash_table_remove_all (priv->patterns_hash);
}

void
ns_pango_fc_font_map_add_fonts (NsPangoFcFontMap *fcfontmap,
                                FcFontSet        *fonts,
                                int               first)
{
  NsPangoFcFontMapPrivate *priv;
  int n_families;
  gboolean added = FALSE;
  gboolean new_family = FALSE;

  g_return_if_fail (NS_PANGO_IS_FC_FONT_MAP (fcfontmap));

  priv = fcfontmap->priv;
  if (G_UNLIKELY (priv->closed) || !fonts)
    return;

  n_families = priv->n_families;

  for (int i = MAX (first, 0); i < fonts->nfont; i++)
    {
      FcPattern *pattern = fonts->fonts[i];

      if (!ns_pango_fc_is_supported_font_format (pattern))
        continue;

      pattern = pattern_set_order (pattern, FcSetApplication);
      if (priv->n_families >= 0 && add_family_pattern (fcfontmap, pattern))
        new_family = TRUE;
      if (priv->fonts)
        FcFontSetAdd (priv->fonts, pattern);
      else
        FcPatternDestroy (pattern);
      added = TRUE;
    }

  if (!added)
    return;

  forget_fontsets (fcfontmap);

  if (new_family)
    {
      qsort (priv->families, priv->n_families, sizeof (NsPangoFcFamily *), compare_font_family_names);
      g_list_model_items_changed (G_LIST_MODEL (fcfontmap), 0, n_families, priv->n_families);
      g_object_notify (G_OBJECT (fcfontmap), "n-items");
    }

  ns_pango_font_map_changed (NS_PANGO_FONT_MAP (fcfontmap));
}

/**
 * ns_pango_fc_font_map_get_config: (skip)
 * @fcfontmap: a `NsPangoFcFontMap`
 *
 * Fetches the `FcConfig` attached to a font map.
 *
 * See also: [method@NsPangoFc.FontMap.set_config].
 *
 * Returns: (nullable): the `FcConfig` object attached to
 *   @fcfontmap, which might be %NULL. The return value is
 *   owned by Pango and should not be freed.
 *
 * Since: 1.38
 */
FcConfig *
ns_pango_fc_font_map_get_config (NsPangoFcFontMap *fcfontmap)
{
  g_return_val_if_fail (NS_PANGO_IS_FC_FONT_MAP (fcfontmap), NULL);

  wait_for_fc_init ();

  return fcfontmap->priv->config;
}

static FcFontSet *
ns_pango_fc_font_map_get_config_fonts (NsPangoFcFontMap *fcfontmap)
{
  if (fcfontmap->priv->fonts == NULL)
    {
      FcFontSet *sets[2];

      wait_for_fc_init ();

      sets[FcSetSystem] = FcConfigGetFonts (fcfontmap->priv->config, FcSetSystem);
      sets[FcSetApplication] = FcConfigGetFonts (fcfontmap->priv->config, FcSetApplication);

      fcfontmap->priv->fonts = filter_by_format (sets, 2);
    }

  return fcfontmap->priv->fonts;
}

static NsPangoFcFontFaceData *
ns_pango_fc_font_map_get_font_face_data (NsPangoFcFontMap *fcfontmap,
				      FcPattern      *font_pattern)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  NsPangoFcFontFaceData key;
  NsPangoFcFontFaceData *data;

  if (FcPatternGetString (font_pattern, FC_FILE, 0, (FcChar8 **)(void*)&key.filename) != FcResultMatch)
    return NULL;

  if (FcPatternGetInteger (font_pattern, FC_INDEX, 0, &key.id) != FcResultMatch)
    return NULL;

  data = g_hash_table_lookup (priv->font_face_data_hash, &key);
  if (G_LIKELY (data))
    return data;

  data = g_slice_new0 (NsPangoFcFontFaceData);
  data->filename = key.filename;
  data->id = key.id;

  data->pattern = font_pattern;
  FcPatternReference (data->pattern);

  g_hash_table_insert (priv->font_face_data_hash, data, data);

  return data;
}

typedef struct {
  NsPangoCoverage parent_instance;

  FcCharSet *covered;
  FcCharSet *not_covered;
} NsPangoFcCoverage;

typedef struct {
  NsPangoCoverageClass parent_class;
} NsPangoFcCoverageClass;

GType ns_pango_fc_coverage_get_type (void) G_GNUC_CONST;

G_DEFINE_TYPE (NsPangoFcCoverage, ns_pango_fc_coverage, NS_TYPE_PANGO_COVERAGE)

static void
ns_pango_fc_coverage_init (NsPangoFcCoverage *coverage)
{
}

static NsPangoCoverageLevel
ns_pango_fc_coverage_real_get (NsPangoCoverage *coverage,
                            int            index)
{
  NsPangoFcCoverage *fc_coverage = (NsPangoFcCoverage*)coverage;
  gunichar ch1, ch2;

  if (FcCharSetHasChar (fc_coverage->covered, index))
    return NS_PANGO_COVERAGE_EXACT;

  if (FcCharSetHasChar (fc_coverage->not_covered, index))
    return NS_PANGO_COVERAGE_NONE;

  if (g_unichar_decompose ((gunichar) index, &ch1, &ch2))
    {
      if ((ns_pango_coverage_get (coverage, ch1) == NS_PANGO_COVERAGE_EXACT) &&
          (ch2 == 0 || ns_pango_coverage_get (coverage, ch2) == NS_PANGO_COVERAGE_EXACT))
        {
          FcCharSetAddChar (fc_coverage->covered, index);
          return NS_PANGO_COVERAGE_EXACT;
        }
    }

  FcCharSetAddChar (fc_coverage->not_covered, index);
  return NS_PANGO_COVERAGE_NONE;
}

static void
ns_pango_fc_coverage_real_set (NsPangoCoverage *coverage,
                            int            index,
                            NsPangoCoverageLevel level)
{
  NsPangoFcCoverage *fc_coverage = (NsPangoFcCoverage*)coverage;

  if (level == NS_PANGO_COVERAGE_NONE)
    {
      FcCharSetDelChar (fc_coverage->covered, index);
      FcCharSetAddChar (fc_coverage->not_covered, index);
    }
  else
    {
      FcCharSetAddChar (fc_coverage->covered, index);
      FcCharSetDelChar (fc_coverage->not_covered, index);
    }
}

static NsPangoCoverage *
ns_pango_fc_coverage_real_copy (NsPangoCoverage *coverage)
{
  NsPangoFcCoverage *fc_coverage = (NsPangoFcCoverage*)coverage;
  NsPangoFcCoverage *copy;

  copy = g_object_new (ns_pango_fc_coverage_get_type (), NULL);
  copy->covered = FcCharSetCopy (fc_coverage->covered);
  copy->not_covered = FcCharSetCopy (fc_coverage->not_covered);

  return (NsPangoCoverage *)copy;
}

static void
ns_pango_fc_coverage_finalize (GObject *object)
{
  NsPangoFcCoverage *fc_coverage = (NsPangoFcCoverage*)object;

  FcCharSetDestroy (fc_coverage->covered);
  FcCharSetDestroy (fc_coverage->not_covered);

  G_OBJECT_CLASS (ns_pango_fc_coverage_parent_class)->finalize (object);
}

static void
ns_pango_fc_coverage_class_init (NsPangoFcCoverageClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  NsPangoCoverageClass *coverage_class = NS_PANGO_COVERAGE_CLASS (class);

  object_class->finalize = ns_pango_fc_coverage_finalize;

  coverage_class->get = ns_pango_fc_coverage_real_get;
  coverage_class->set = ns_pango_fc_coverage_real_set;
  coverage_class->copy = ns_pango_fc_coverage_real_copy;
}

NsPangoCoverage *
_ns_pango_fc_font_map_get_coverage (NsPangoFcFontMap *fcfontmap,
				 NsPangoFcFont    *fcfont)
{
  NsPangoFcFontFaceData *data;
  FcCharSet *charset;

  data = ns_pango_fc_font_map_get_font_face_data (fcfontmap, fcfont->font_pattern);
  if (G_UNLIKELY (!data))
    return NULL;

  if (G_UNLIKELY (data->coverage == NULL))
    {
      /*
       * Pull the coverage out of the pattern, this
       * doesn't require loading the font
       */
      if (FcPatternGetCharSet (fcfont->font_pattern, FC_CHARSET, 0, &charset) != FcResultMatch)
        return ns_pango_coverage_new ();

      data->coverage = _ns_pango_fc_font_map_fc_to_coverage (charset);
    }

  return g_object_ref (data->coverage);
}

/**
 * _ns_pango_fc_font_map_fc_to_coverage:
 * @charset: `FcCharSet` to convert to a `NsPangoCoverage` object.
 *
 * Convert the given `FcCharSet` into a new `NsPangoCoverage` object.
 *
 * The caller is responsible for freeing the newly created object.
 *
 * Since: 1.6
 */
NsPangoCoverage  *
_ns_pango_fc_font_map_fc_to_coverage (FcCharSet *charset)
{
  NsPangoFcCoverage *coverage;

  coverage = g_object_new (ns_pango_fc_coverage_get_type (), NULL);
  coverage->covered = FcCharSetCopy (charset);
  coverage->not_covered = FcCharSetCreate ();

  return (NsPangoCoverage *)coverage;
}

static NsPangoLanguage **
_ns_pango_fc_font_map_fc_to_languages (FcLangSet *langset)
{
  FcStrSet *strset;
  FcStrList *list;
  FcChar8 *s;
  GPtrArray *langs;

  langs = g_ptr_array_new ();

  strset = FcLangSetGetLangs (langset);
  list = FcStrListCreate (strset);

  FcStrListFirst (list);
  while ((s = FcStrListNext (list)))
    {
      NsPangoLanguage *l = ns_pango_language_from_string ((const char *)s);
      g_ptr_array_add (langs, l);
    }

  FcStrListDone (list);
  FcStrSetDestroy (strset);

  g_ptr_array_add (langs, NULL);

  return (NsPangoLanguage **) g_ptr_array_free (langs, FALSE);
}

NsPangoLanguage **
_ns_pango_fc_font_map_get_languages (NsPangoFcFontMap *fcfontmap,
                                  NsPangoFcFont    *fcfont)
{
  NsPangoFcFontFaceData *data;
  FcLangSet *langset;

  data = ns_pango_fc_font_map_get_font_face_data (fcfontmap, fcfont->font_pattern);
  if (G_UNLIKELY (!data))
    return NULL;

  if (G_UNLIKELY (data->languages == NULL))
    {
      /*
       * Pull the languages out of the pattern, this
       * doesn't require loading the font
       */
      if (FcPatternGetLangSet (fcfont->font_pattern, FC_LANG, 0, &langset) != FcResultMatch)
        return NULL;

      data->languages = _ns_pango_fc_font_map_fc_to_languages (langset);
    }

  return data->languages;
}

static void
shutdown_font (gpointer        key,
	       NsPangoFcFont    *fcfont,
	       NsPangoFcFontMap *fcfontmap)
{
  _ns_pango_fc_font_shutdown (fcfont);

  _ns_pango_fc_font_set_font_key (fcfont, NULL);
  ns_pango_fc_font_key_free (key);
}

/**
 * ns_pango_fc_font_map_shutdown:
 * @fcfontmap: a `NsPangoFcFontMap`
 *
 * Clears all cached information for the fontmap and marks
 * all fonts open for the fontmap as dead.
 *
 * See the shutdown() virtual function of `NsPangoFcFont`.
 *
 * This function might be used by a backend when the underlying
 * windowing system for the font map exits. This function is only
 * intended to be called only for backend implementations deriving
 * from `NsPangoFcFontMap`.
 *
 * Since: 1.4
 */
void
ns_pango_fc_font_map_shutdown (NsPangoFcFontMap *fcfontmap)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  int i;

  if (priv->closed)
    return;

  g_hash_table_foreach (priv->font_hash, (GHFunc) shutdown_font, fcfontmap);
  for (i = 0; i < priv->n_families; i++)
    priv->families[i]->fontmap = NULL;

  ns_pango_fc_font_map_fini (fcfontmap);

  while (priv->findfuncs)
    {
      NsPangoFcFindFuncInfo *info;
      info = priv->findfuncs->data;
      if (info->dnotify)
	info->dnotify (info->user_data);

      g_slice_free (NsPangoFcFindFuncInfo, info);
      priv->findfuncs = g_slist_delete_link (priv->findfuncs, priv->findfuncs);
    }

  priv->closed = TRUE;
}

static NsPangoWeight
ns_pango_fc_convert_weight_to_pango (double fc_weight)
{
  return FcWeightToOpenTypeDouble (fc_weight);
}

static NsPangoStyle
ns_pango_fc_convert_slant_to_pango (int fc_style)
{
  switch (fc_style)
    {
    case FC_SLANT_ROMAN:
      return NS_PANGO_STYLE_NORMAL;
    case FC_SLANT_ITALIC:
      return NS_PANGO_STYLE_ITALIC;
    case FC_SLANT_OBLIQUE:
      return NS_PANGO_STYLE_OBLIQUE;
    default:
      return NS_PANGO_STYLE_NORMAL;
    }
}

static NsPangoStretch
ns_pango_fc_convert_width_to_pango (int fc_stretch)
{
  switch (fc_stretch)
    {
    case FC_WIDTH_NORMAL:
      return NS_PANGO_STRETCH_NORMAL;
    case FC_WIDTH_ULTRACONDENSED:
      return NS_PANGO_STRETCH_ULTRA_CONDENSED;
    case FC_WIDTH_EXTRACONDENSED:
      return NS_PANGO_STRETCH_EXTRA_CONDENSED;
    case FC_WIDTH_CONDENSED:
      return NS_PANGO_STRETCH_CONDENSED;
    case FC_WIDTH_SEMICONDENSED:
      return NS_PANGO_STRETCH_SEMI_CONDENSED;
    case FC_WIDTH_SEMIEXPANDED:
      return NS_PANGO_STRETCH_SEMI_EXPANDED;
    case FC_WIDTH_EXPANDED:
      return NS_PANGO_STRETCH_EXPANDED;
    case FC_WIDTH_EXTRAEXPANDED:
      return NS_PANGO_STRETCH_EXTRA_EXPANDED;
    case FC_WIDTH_ULTRAEXPANDED:
      return NS_PANGO_STRETCH_ULTRA_EXPANDED;
    default:
      return NS_PANGO_STRETCH_NORMAL;
    }
}

/**
 * ns_pango_fc_font_description_from_pattern:
 * @pattern: a `FcPattern`
 * @include_size: if %TRUE, the pattern will include the size from
 *   the @pattern; otherwise the resulting pattern will be unsized.
 *   (only %FC_SIZE is examined, not %FC_PIXEL_SIZE)
 *
 * Creates a `NsPangoFontDescription` that matches the specified
 * Fontconfig pattern as closely as possible.
 *
 * Many possible Fontconfig pattern values, such as %FC_RASTERIZER
 * or %FC_DPI, don't make sense in the context of `NsPangoFontDescription`,
 * so will be ignored.
 *
 * Return value: a new `NsPangoFontDescription`. Free with
 *   ns_pango_font_description_free().
 *
 * Since: 1.4
 */
NsPangoFontDescription *
ns_pango_fc_font_description_from_pattern (FcPattern *pattern, gboolean include_size)
{
  return font_description_from_pattern (pattern, include_size, FALSE);
}

NsPangoFontDescription *
font_description_from_pattern (FcPattern *pattern,
                               gboolean   include_size,
                               gboolean   shallow)
{
  NsPangoFontDescription *desc;
  NsPangoStyle style;
  NsPangoWeight weight;
  NsPangoStretch stretch;
  double size;
  NsPangoGravity gravity;
  const char *s;
  int i;
  double d;
  FcResult res;
  GString *str;

  desc = ns_pango_font_description_new ();

  res = FcPatternGetString (pattern, FC_FAMILY, 0, (FcChar8 **) &s);
  g_assert (res == FcResultMatch);

  if (shallow)
    ns_pango_font_description_set_family_static (desc, s);
  else
    ns_pango_font_description_set_family (desc, s);

  if (FcPatternGetInteger (pattern, FC_SLANT, 0, &i) == FcResultMatch)
    style = ns_pango_fc_convert_slant_to_pango (i);
  else
    style = NS_PANGO_STYLE_NORMAL;

  ns_pango_font_description_set_style (desc, style);

  if (FcPatternGetDouble (pattern, FC_WEIGHT, 0, &d) == FcResultMatch)
    weight = ns_pango_fc_convert_weight_to_pango (d);
  else
    weight = NS_PANGO_WEIGHT_NORMAL;

  ns_pango_font_description_set_weight (desc, weight);

  if (FcPatternGetInteger (pattern, FC_WIDTH, 0, &i) == FcResultMatch)
    stretch = ns_pango_fc_convert_width_to_pango (i);
  else
    stretch = NS_PANGO_STRETCH_NORMAL;

  ns_pango_font_description_set_stretch (desc, stretch);

  str = NULL;

  for (int i = 0; i < 32; i++)
    {
      if (FcPatternGetString (pattern, FC_FONT_FEATURES, i, (FcChar8 **)&s) == FcResultMatch)
        {
          if (str == NULL)
            str = g_string_new ("");
          if (str->len > 0)
            g_string_append_c (str, ',');
          g_string_append (str, s);
        }
      else
        break;
    }

  if (str)
    {
      ns_pango_font_description_set_variant (desc, ns_pango_features_get_variant (str->str));
      ns_pango_font_description_set_features (desc, str->str);
      g_string_free (str, TRUE);
    }
  else
    ns_pango_font_description_set_variant (desc, NS_PANGO_VARIANT_NORMAL);

  if (include_size && FcPatternGetDouble (pattern, FC_SIZE, 0, &size) == FcResultMatch)
    {
      FcMatrix *fc_matrix;
      double scale_factor = 1;
      volatile double scaled_size;

      if (FcPatternGetMatrix (pattern, FC_MATRIX, 0, &fc_matrix) == FcResultMatch)
        {
          NsPangoMatrix mat = NS_PANGO_MATRIX_INIT;

          mat.xx = fc_matrix->xx;
          mat.xy = fc_matrix->xy;
          mat.yx = fc_matrix->yx;
          mat.yy = fc_matrix->yy;

          scale_factor = ns_pango_matrix_get_font_scale_factor (&mat);
        }

      /* We need to use a local variable to ensure that the compiler won't
       * implicitly cast it to integer while the result is kept in registers,
       * leading to a wrong approximation in i386 (with 387 FPU)
       */
      scaled_size = scale_factor * size * NS_PANGO_SCALE;
      ns_pango_font_description_set_size (desc, scaled_size);
    }

  /* gravity is a bit different.  we don't want to set it if it was not set on
   * the pattern */
  if (FcPatternGetString (pattern, NS_PANGO_FC_GRAVITY, 0, (FcChar8 **)&s) == FcResultMatch)
    {
      GEnumValue *value = g_enum_get_value_by_nick (get_gravity_class (), (char *)s);
      gravity = value->value;

      ns_pango_font_description_set_gravity (desc, gravity);
    }

  if (FcPatternGetString (pattern, FC_FONT_VARIATIONS, 0, (FcChar8 **)&s) == FcResultMatch)
    {
      if (s && *s)
        {
          if (shallow)
            ns_pango_font_description_set_variations_static (desc, s);
          else
            ns_pango_font_description_set_variations (desc, s);
        }
    }

  return desc;
}

/*
 * NsPangoFcFace
 */

typedef NsPangoFontFaceClass NsPangoFcFaceClass;

G_DEFINE_TYPE (NsPangoFcFace, ns_pango_fc_face, NS_TYPE_PANGO_FONT_FACE)

static NsPangoFontDescription *
make_alias_description (NsPangoFcFamily *fcfamily,
			gboolean        bold,
			gboolean        italic)
{
  NsPangoFontDescription *desc = ns_pango_font_description_new ();

  ns_pango_font_description_set_family (desc, fcfamily->family_name);
  ns_pango_font_description_set_style (desc, italic ? NS_PANGO_STYLE_ITALIC : NS_PANGO_STYLE_NORMAL);
  ns_pango_font_description_set_weight (desc, bold ? NS_PANGO_WEIGHT_BOLD : NS_PANGO_WEIGHT_NORMAL);

  return desc;
}

static NsPangoFontDescription *
ns_pango_fc_face_describe (NsPangoFontFace *face)
{
  NsPangoFcFace *fcface = NS_PANGO_FC_FACE (face);
  NsPangoFcFamily *fcfamily = fcface->family;
  NsPangoFontDescription *desc = NULL;

  if (G_UNLIKELY (!fcfamily))
    return ns_pango_font_description_new ();

  if (fcface->fake)
    {
      if (strcmp (fcface->style, "Regular") == 0)
	return make_alias_description (fcfamily, FALSE, FALSE);
      else if (strcmp (fcface->style, "Bold") == 0)
	return make_alias_description (fcfamily, TRUE, FALSE);
      else if (strcmp (fcface->style, "Italic") == 0)
	return make_alias_description (fcfamily, FALSE, TRUE);
      else			/* Bold Italic */
	return make_alias_description (fcfamily, TRUE, TRUE);
    }

  g_assert (fcface->pattern);
  desc = ns_pango_fc_font_description_from_pattern (fcface->pattern, FALSE);

  return desc;
}

static const char *
ns_pango_fc_face_get_face_name (NsPangoFontFace *face)
{
  NsPangoFcFace *fcface = NS_PANGO_FC_FACE (face);

  return fcface->style;
}

static int
compare_ints (gconstpointer ap,
	      gconstpointer bp)
{
  int a = *(int *)ap;
  int b = *(int *)bp;

  if (a == b)
    return 0;
  else if (a > b)
    return 1;
  else
    return -1;
}

static void
ns_pango_fc_face_list_sizes (NsPangoFontFace  *face,
			  int           **sizes,
			  int            *n_sizes)
{
  NsPangoFcFace *fcface = NS_PANGO_FC_FACE (face);
  FcPattern *pattern;
  FcFontSet *fontset;
  FcObjectSet *objectset;
  FcFontSet *fonts;

  if (sizes)
    *sizes = NULL;
  *n_sizes = 0;
  if (G_UNLIKELY (!fcface->family || !fcface->family->fontmap))
    return;

  pattern = FcPatternCreate ();
  FcPatternAddString (pattern, FC_FAMILY, (FcChar8*)(void*)fcface->family->family_name);
  FcPatternAddString (pattern, FC_STYLE, (FcChar8*)(void*)fcface->style);

  objectset = FcObjectSetCreate ();
  FcObjectSetAdd (objectset, FC_PIXEL_SIZE);

  fonts = ns_pango_fc_font_map_get_config_fonts (fcface->family->fontmap);
  fontset = FcFontSetList (fcface->family->fontmap->priv->config, &fonts, 1, pattern, objectset);

  if (fontset)
    {
      GArray *size_array;
      double size, dpi = -1.0;
      int i, size_i, j;

      size_array = g_array_new (FALSE, FALSE, sizeof (int));

      for (i = 0; i < fontset->nfont; i++)
	{
	  for (j = 0;
	       FcPatternGetDouble (fontset->fonts[i], FC_PIXEL_SIZE, j, &size) == FcResultMatch;
	       j++)
	    {
	      if (dpi < 0)
		dpi = ns_pango_fc_font_map_get_resolution (fcface->family->fontmap, NULL);

	      size_i = (int) (NS_PANGO_SCALE * size * 72.0 / dpi);
	      g_array_append_val (size_array, size_i);
	    }
	}

      g_array_sort (size_array, compare_ints);

      if (size_array->len == 0)
	{
	  *n_sizes = 0;
	  if (sizes)
	    *sizes = NULL;
	  g_array_free (size_array, TRUE);
	}
      else
	{
	  *n_sizes = size_array->len;
	  if (sizes)
	    {
	      *sizes = (int *) size_array->data;
	      g_array_free (size_array, FALSE);
	    }
	  else
	    g_array_free (size_array, TRUE);
	}

      FcFontSetDestroy (fontset);
    }
  else
    {
      *n_sizes = 0;
      if (sizes)
	*sizes = NULL;
    }

  FcPatternDestroy (pattern);
  FcObjectSetDestroy (objectset);
}

static gboolean
ns_pango_fc_face_is_synthesized (NsPangoFontFace *face)
{
  NsPangoFcFace *fcface = NS_PANGO_FC_FACE (face);

  return fcface->fake;
}

static NsPangoFontFamily *
ns_pango_fc_face_get_family (NsPangoFontFace *face)
{
  NsPangoFcFace *fcface = NS_PANGO_FC_FACE (face);

  return NS_PANGO_FONT_FAMILY (fcface->family);
}

static void
ns_pango_fc_face_finalize (GObject *object)
{
  NsPangoFcFace *fcface = NS_PANGO_FC_FACE (object);

  g_free (fcface->style);
  FcPatternDestroy (fcface->pattern);

  G_OBJECT_CLASS (ns_pango_fc_face_parent_class)->finalize (object);
}

static void
ns_pango_fc_face_init (NsPangoFcFace *self)
{
}

static void
ns_pango_fc_face_class_init (NsPangoFcFaceClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ns_pango_fc_face_finalize;

  class->describe = ns_pango_fc_face_describe;
  class->get_face_name = ns_pango_fc_face_get_face_name;
  class->list_sizes = ns_pango_fc_face_list_sizes;
  class->is_synthesized = ns_pango_fc_face_is_synthesized;
  class->get_family = ns_pango_fc_face_get_family;
}


/*
 * NsPangoFcFamily
 */

typedef NsPangoFontFamilyClass NsPangoFcFamilyClass;

static GType
ns_pango_fc_family_get_item_type (GListModel *list)
{
  return NS_TYPE_PANGO_FONT_FACE;
}

static guint
ns_pango_fc_family_get_n_items (GListModel *list)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (list);

  ensure_faces (fcfamily);

  return (guint)fcfamily->n_faces;
}

static gpointer
ns_pango_fc_family_get_item (GListModel *list,
                          guint       position)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (list);

  ensure_faces (fcfamily);

  if (position < fcfamily->n_faces)
    return g_object_ref (fcfamily->faces[position]);

  return NULL;
}

static void
ns_pango_fc_family_list_model_init (GListModelInterface *iface)
{
  iface->get_item_type = ns_pango_fc_family_get_item_type;
  iface->get_n_items = ns_pango_fc_family_get_n_items;
  iface->get_item = ns_pango_fc_family_get_item;
}

G_DEFINE_TYPE_WITH_CODE (NsPangoFcFamily, ns_pango_fc_family, NS_TYPE_PANGO_FONT_FAMILY,
                         G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL, ns_pango_fc_family_list_model_init))

static NsPangoFcFace *
create_face (NsPangoFcFamily *fcfamily,
	     const char    *style,
	     FcPattern     *pattern,
	     gboolean       fake)
{
  NsPangoFcFace *face = g_object_new (NS_PANGO_FC_TYPE_FACE, NULL);
  face->style = g_strdup (style);
  if (pattern)
    FcPatternReference (pattern);
  face->pattern = pattern;
  face->family = fcfamily;
  face->fake = fake;

  return face;
}

static int
compare_face_pattern (FcPattern *p1,
                      FcPattern *p2)
{
  int w1, w2;
  int s1, s2;

  if (FcPatternGetInteger (p1, FC_WEIGHT, 0, &w1) != FcResultMatch)
    w1 = FC_WEIGHT_MEDIUM;

  if (FcPatternGetInteger (p1, FC_SLANT, 0, &s1) != FcResultMatch)
    s1 = FC_SLANT_ROMAN;

  if (FcPatternGetInteger (p2, FC_WEIGHT, 0, &w2) != FcResultMatch)
    w2 = FC_WEIGHT_MEDIUM;

  if (FcPatternGetInteger (p2, FC_SLANT, 0, &s2) != FcResultMatch)
    s2 = FC_SLANT_ROMAN;

  if (s1 != s2)
    return s1 - s2; /* roman < italic < oblique */

  return w1 - w2; /* from light to heavy */
}

static int
compare_face (const void *p1, const void *p2)
{
  const NsPangoFcFace *f1 = *(const void **)p1;
  const NsPangoFcFace *f2 = *(const void **)p2;

  return compare_face_pattern (f1->pattern, f2->pattern);
}

static void
ensure_faces (NsPangoFcFamily *fcfamily)
{
  NsPangoFcFontMap *fcfontmap = fcfamily->fontmap;
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;

  if (fcfamily->n_faces < 0)
    {
      FcFontSet *fontset;
      int i;

      if (is_alias_family (fcfamily->family_name) || priv->closed)
	{
	  fcfamily->n_faces = 4;
	  fcfamily->faces = g_new (NsPangoFcFace *, fcfamily->n_faces);

	  i = 0;
	  fcfamily->faces[i++] = create_face (fcfamily, "Regular", NULL, TRUE);
	  fcfamily->faces[i++] = create_face (fcfamily, "Bold", NULL, TRUE);
	  fcfamily->faces[i++] = create_face (fcfamily, "Italic", NULL, TRUE);
	  fcfamily->faces[i++] = create_face (fcfamily, "Bold Italic", NULL, TRUE);
          fcfamily->faces[0]->regular = 1;
	}
      else
	{
	  enum {
	    REGULAR,
	    ITALIC,
	    BOLD,
	    BOLD_ITALIC
	  };
	  /* Regular, Italic, Bold, Bold Italic */
	  gboolean has_face [4] = { FALSE, FALSE, FALSE, FALSE };
	  NsPangoFcFace **faces;
	  gint num = 0;
          int regular_weight;
          int regular_idx;

	  fontset = fcfamily->patterns;

	  /* at most we have 3 additional artificial faces */
	  faces = g_new (NsPangoFcFace *, fontset->nfont + 3);

          regular_weight = 0;
          regular_idx = -1;

	  for (i = 0; i < fontset->nfont; i++)
	    {
	      const char *style, *font_style = NULL;
	      int weight, slant;

	      if (FcPatternGetInteger(fontset->fonts[i], FC_WEIGHT, 0, &weight) != FcResultMatch)
		weight = FC_WEIGHT_MEDIUM;

	      if (FcPatternGetInteger(fontset->fonts[i], FC_SLANT, 0, &slant) != FcResultMatch)
		slant = FC_SLANT_ROMAN;

              {
                gboolean variable;
                if (FcPatternGetBool(fontset->fonts[i], FC_VARIABLE, 0, &variable) != FcResultMatch)
                  variable = FALSE;
                if (variable) /* skip the variable face */
                  continue;
              }

	      if (FcPatternGetString (fontset->fonts[i], FC_STYLE, 0, (FcChar8 **)(void*)&font_style) != FcResultMatch)
		font_style = NULL;

              if (font_style && strcmp (font_style, "Regular") == 0)
                {
                  regular_weight = FC_WEIGHT_MEDIUM;
                  regular_idx = num;
                }

	      if (weight <= FC_WEIGHT_MEDIUM)
		{
		  if (slant == FC_SLANT_ROMAN)
		    {
		      has_face[REGULAR] = TRUE;
		      style = "Regular";
                      if (weight > regular_weight)
                        {
                          regular_weight = weight;
                          regular_idx = num;
                        }
		    }
		  else
		    {
		      has_face[ITALIC] = TRUE;
		      style = "Italic";
		    }
		}
	      else
		{
		  if (slant == FC_SLANT_ROMAN)
		    {
		      has_face[BOLD] = TRUE;
		      style = "Bold";
		    }
		  else
		    {
		      has_face[BOLD_ITALIC] = TRUE;
		      style = "Bold Italic";
		    }
		}

	      if (!font_style)
		font_style = style;
	      faces[num++] = create_face (fcfamily, font_style, fontset->fonts[i], FALSE);
	    }

	  if (has_face[REGULAR])
	    {
	      if (!has_face[ITALIC])
		faces[num++] = create_face (fcfamily, "Italic", NULL, TRUE);
	      if (!has_face[BOLD])
		faces[num++] = create_face (fcfamily, "Bold", NULL, TRUE);

	    }
	  if ((has_face[REGULAR] || has_face[ITALIC] || has_face[BOLD]) && !has_face[BOLD_ITALIC])
	    faces[num++] = create_face (fcfamily, "Bold Italic", NULL, TRUE);

          if (regular_idx != -1)
            faces[regular_idx]->regular = 1;

	  faces = g_renew (NsPangoFcFace *, faces, num);

          if (num > 0)
            qsort (faces, num, sizeof (NsPangoFcFace *), compare_face);

	  fcfamily->n_faces = num;
	  fcfamily->faces = faces;
	}
    }
}

static void
ns_pango_fc_family_list_faces (NsPangoFontFamily  *family,
			    NsPangoFontFace  ***faces,
			    int              *n_faces)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (family);

  if (faces)
    *faces = NULL;

  if (n_faces)
    *n_faces = 0;

  if (G_UNLIKELY (!fcfamily->fontmap))
    return;

  ensure_faces (fcfamily);

  if (n_faces)
    *n_faces = fcfamily->n_faces;

  if (faces)
    *faces = g_memdup2 (fcfamily->faces, fcfamily->n_faces * sizeof (NsPangoFontFace *));
}

static NsPangoFontFace *
ns_pango_fc_family_get_face (NsPangoFontFamily *family,
                          const char      *name)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (family);
  int i;

  ensure_faces (fcfamily);

  for (i = 0; i < fcfamily->n_faces; i++)
    {
      NsPangoFontFace *face = NS_PANGO_FONT_FACE (fcfamily->faces[i]);

      if ((name != NULL && strcmp (name, ns_pango_font_face_get_face_name (face)) == 0) ||
          (name == NULL && NS_PANGO_FC_FACE (face)->regular))
        return face;
    }

  return NULL;
}

static const char *
ns_pango_fc_family_get_name (NsPangoFontFamily  *family)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (family);

  return fcfamily->family_name;
}

static gboolean
ns_pango_fc_family_is_monospace (NsPangoFontFamily *family)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (family);

  return fcfamily->spacing == FC_MONO ||
	 fcfamily->spacing == FC_DUAL ||
	 fcfamily->spacing == FC_CHARCELL;
}

static gboolean
ns_pango_fc_family_is_variable (NsPangoFontFamily *family)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (family);

  return fcfamily->variable;
}

static void
ns_pango_fc_family_finalize (GObject *object)
{
  NsPangoFcFamily *fcfamily = NS_PANGO_FC_FAMILY (object);

  g_free (fcfamily->family_name);

  for (int i = 0; i < fcfamily->n_faces; i++)
    {
      fcfamily->faces[i]->family = NULL;
      g_object_unref (fcfamily->faces[i]);
    }
  FcFontSetDestroy (fcfamily->patterns);
  g_free (fcfamily->faces);

  G_OBJECT_CLASS (ns_pango_fc_family_parent_class)->finalize (object);
}

static void
ns_pango_fc_family_class_init (NsPangoFcFamilyClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ns_pango_fc_family_finalize;

  class->list_faces = ns_pango_fc_family_list_faces;
  class->get_face = ns_pango_fc_family_get_face;
  class->get_name = ns_pango_fc_family_get_name;
  class->is_monospace = ns_pango_fc_family_is_monospace;
  class->is_variable = ns_pango_fc_family_is_variable;
}

static void
ns_pango_fc_family_init (NsPangoFcFamily *fcfamily)
{
  fcfamily->n_faces = -1;
}

/**
 * ns_pango_fc_font_map_get_hb_face: (skip)
 * @fcfontmap: a `NsPangoFcFontMap`
 * @fcfont: a `NsPangoFcFont`
 *
 * Retrieves the `hb_face_t` for the given `NsPangoFcFont`.
 *
 * Returns: (transfer none) (nullable): the `hb_face_t`
 *   for the given font
 *
 * Since: 1.44
 */
hb_face_t *
ns_pango_fc_font_map_get_hb_face (NsPangoFcFontMap *fcfontmap,
                               NsPangoFcFont    *fcfont)
{
  NsPangoFcFontFaceData *data;

  data = ns_pango_fc_font_map_get_font_face_data (fcfontmap, fcfont->font_pattern);

  if (!data->hb_face)
    {
      hb_blob_t *blob;

      blob = hb_blob_create_from_file (data->filename);
      data->hb_face = hb_face_create (blob, data->id);
      hb_blob_destroy (blob);
    }

  return data->hb_face;
}

static gboolean
insert_face (NsPangoFcFontMap *fcfontmap,
             NsPangoFcFamily  *family,
             FcPattern      *pattern)
{
  const char *name;
  FcResult res;
  NsPangoFcFace *face;

  res = FcPatternGetString (pattern, FC_STYLE, 0, (FcChar8 **)(void*)&name);
  if (res != FcResultMatch)
    return FALSE;

  face = NS_PANGO_FC_FACE (ns_pango_font_family_get_face (NS_PANGO_FONT_FAMILY (family), name));
  if (face)
    {
      int order = 0;

      res = FcPatternGetInteger (face->pattern, FC_SPACING, 0, &order);
      if (res != FcResultMatch)
        order = 0;

      pattern = pattern_set_order (pattern, order + 1);
      FcFontSetAdd (fcfontmap->priv->fonts, pattern);

      for (guint i = 0; i < family->n_faces; i++)
        {
          if (family->faces[i] == face)
            {
              FcPatternReference (pattern);
              family->faces[i] = create_face (family, name, pattern, FALSE);

              g_list_model_items_changed (G_LIST_MODEL (family), i, 1, 1);

              g_object_unref (face);

              break;
            }
        }
    }
  else
    {
      FcPatternReference (pattern);
      FcFontSetAdd (fcfontmap->priv->fonts, pattern);

      FcPatternReference (pattern);
      face = create_face (family, name, pattern, FALSE);

      family->n_faces++;
      family->faces = g_renew (NsPangoFcFace *, family->faces, family->n_faces);
      family->faces[family->n_faces - 1] = face;

      qsort (family->faces, family->n_faces, sizeof (NsPangoFcFace *), compare_face);

      for (guint i = 0; i < family->n_faces; i++)
        {
          if (family->faces[i] == face)
            {
              g_list_model_items_changed (G_LIST_MODEL (family), i, 0, 1);
              break;
            }
        }
    }

  return TRUE;
}

static void
ns_pango_fc_font_map_add_pattern (NsPangoFcFontMap *fcfontmap,
                               FcPattern      *pattern)
{
  NsPangoFcFontMapPrivate *priv = fcfontmap->priv;
  const char *name;
  FcResult res;
  NsPangoFcFamily *family;

  res = FcPatternGetString (pattern, FC_FAMILY, 0, (FcChar8 **)(void*)&name);
  if (res != FcResultMatch)
    return;

  family = (NsPangoFcFamily *) ns_pango_fc_font_map_get_family (NS_PANGO_FONT_MAP (fcfontmap), name);

  if (family)
    {
      insert_face (fcfontmap, family, pattern);
    }
  else
    {
      int spacing = 0;
      gboolean variable = FALSE;

      res = FcPatternGetInteger (pattern, FC_SPACING, 0, &spacing);
      if (res != FcResultMatch)
        spacing = FC_PROPORTIONAL;

      res = FcPatternGetBool (pattern, FC_VARIABLE, 0, &variable);
      if (res != FcResultMatch)
        variable = FALSE;

      family = create_family (fcfontmap, name, spacing, variable);

      if (insert_face (fcfontmap, family, pattern))
        {
          FcPatternReference (pattern);
          FcFontSetAdd (fcfontmap->priv->fonts, pattern);

          /* insert family into fontmap */
          priv->n_families++;
          priv->families = g_renew (NsPangoFcFamily *, priv->families, priv->n_families);
          priv->families[priv->n_families - 1] = family;
          qsort (priv->families, priv->n_families, sizeof (NsPangoFcFamily *), compare_font_family_names);

          /* emit list model signals */
          for (guint i = 0; i < priv->n_families; i++)
            {
              if (priv->families[i] == family)
                {
                  g_list_model_items_changed (G_LIST_MODEL (fcfontmap), i, 0, 1);
                  break;
                }
            }
        }
      else
        {
          g_object_unref (family);
        }
    }
}

static gboolean
ns_pango_fc_font_map_add_font_file (NsPangoFontMap  *fontmap,
                                 const char    *filename,
                                 GError       **error)
{
  NsPangoFcFontMap *fcfontmap = NS_PANGO_FC_FONT_MAP (fontmap);
  FcFontSet *set, *sets[2], *fonts;

  set = FcFontSetCreate ();
  if (!FcFreeTypeQueryAll ((const FcChar8 *) filename, -1, NULL, NULL, set))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "Adding font %s to fontconfig configuration failed",
                   filename);
      return FALSE;
    }

  sets[FcSetSystem] = NULL;
  sets[FcSetApplication] = set;
  fonts = filter_by_format (sets, 2);

  for (int i = 0; i < fonts->nfont; i++)
    ns_pango_fc_font_map_add_pattern (fcfontmap, fonts->fonts[i]);

  /* A font arriving changes what every context over this map resolves, and
   * the contexts only find out through the serial. config_changed and
   * cache_clear bump it; this path did not, so a context kept its cached
   * metrics and itemisation for the fonts it had before.
   */
  if (fonts->nfont > 0)
    ns_pango_font_map_changed (fontmap);

  FcFontSetDestroy (fonts);
  FcFontSetDestroy (set);

  return TRUE;
}
