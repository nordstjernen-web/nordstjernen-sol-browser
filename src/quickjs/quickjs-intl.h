/* quickjs-intl.h: internal interfaces shared by the ECMA-402 (Intl) implementation files. */

#ifndef QUICKJS_INTL_H
#define QUICKJS_INTL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "quickjs.h"
#include "cutils.h"

JSValue js_intl_create_from_ctor(JSContext *ctx, JSValueConst ctor, JSClassID class_id);
int js_intl_string_list_from_iterable(JSContext *ctx, JSValueConst iterable, JSValue **plist, uint32_t *pcount);
void js_intl_free_value_list(JSContext *ctx, JSValue *list, uint32_t count);
int js_intl_string_utf16(JSContext *ctx, JSValueConst val, uint16_t **pbuf, uint32_t *plen);
JSValue js_intl_sub_string(JSContext *ctx, JSValueConst str, uint32_t start, uint32_t end);
JSValue js_intl_iterator_proto(JSContext *ctx);
int js_intl_this_time_value(JSContext *ctx, double *valp, JSValueConst this_val);
JSValue js_intl_this_number_value(JSContext *ctx, JSValueConst this_val);
JSValue js_intl_this_bigint_value(JSContext *ctx, JSValueConst this_val);
const char *js_intl_runtime_default_locale(JSRuntime *rt);
const char *js_intl_runtime_default_time_zone(JSRuntime *rt);
JSClassID js_intl_class_id(int index);
bool js_intl_in_constructor_call(JSContext *ctx);
int js_intl_ordinary_has_instance(JSContext *ctx, JSValueConst val, JSValueConst ctor);
JSValue js_intl_to_primitive_number(JSContext *ctx, JSValueConst val);
JSValue js_intl_string_case(JSContext *ctx, JSValueConst str, bool to_lower);
void js_intl_set_opaque(JSValueConst obj, void *opaque);
JSValue js_intl_get_fallback_symbol(JSContext *ctx);
void js_intl_set_fallback_symbol(JSContext *ctx, JSValue sym);
JSValue js_intl_get_intrinsic(JSContext *ctx, int index);
void js_intl_set_intrinsic(JSContext *ctx, int index, JSValue val);
double js_intl_local_tza(JSContext *ctx, double t, bool is_utc);

enum {
    INTL_CLASS_LOCALE,
    INTL_CLASS_COLLATOR,
    INTL_CLASS_NUMBER_FORMAT,
    INTL_CLASS_DATE_TIME_FORMAT,
    INTL_CLASS_PLURAL_RULES,
    INTL_CLASS_LIST_FORMAT,
    INTL_CLASS_RELATIVE_TIME_FORMAT,
    INTL_CLASS_DISPLAY_NAMES,
    INTL_CLASS_DURATION_FORMAT,
    INTL_CLASS_SEGMENTER,
    INTL_CLASS_SEGMENTS,
    INTL_CLASS_SEGMENT_ITERATOR,
    INTL_CLASS_COUNT,
};

enum {
    INTL_INTRINSIC_LOCALE,
    INTL_INTRINSIC_COLLATOR,
    INTL_INTRINSIC_NUMBER_FORMAT,
    INTL_INTRINSIC_DATE_TIME_FORMAT,
    INTL_INTRINSIC_PLURAL_RULES,
    INTL_INTRINSIC_LIST_FORMAT,
    INTL_INTRINSIC_RELATIVE_TIME_FORMAT,
    INTL_INTRINSIC_DISPLAY_NAMES,
    INTL_INTRINSIC_DURATION_FORMAT,
    INTL_INTRINSIC_SEGMENTER,
    INTL_INTRINSIC_SEGMENTS_PROTO,
    INTL_INTRINSIC_SEGMENT_ITERATOR_PROTO,
    INTL_INTRINSIC_COUNT,
};

#define INTL_TAG_MAX 256

typedef struct IntlLocaleList {
    char **items;
    int count;
    int size;
} IntlLocaleList;

typedef struct IntlLangId {
    char lang[9];
    char script[5];
    char region[4];
    int nvariants;
    char variants[16][9];
} IntlLangId;

typedef struct IntlKeyword {
    char key[3];
    char value[64];
} IntlKeyword;

typedef struct IntlExtension {
    int nattrs;
    char attrs[8][9];
    int nkeywords;
    IntlKeyword keywords[16];
} IntlExtension;

bool intl_is_well_formed_tag(const char *tag);
char *intl_canonicalize_tag(JSContext *ctx, const char *tag);
bool intl_parse_lang_id(const char *s, size_t len, IntlLangId *id);
void intl_format_lang_id(DynBuf *b, const IntlLangId *id);
size_t intl_base_name_len(const char *tag);
const char *intl_find_unicode_ext(const char *tag, size_t *len);
bool intl_unicode_ext_value(const char *tag, const char *key, char *buf, size_t size, bool *has_value);
void intl_unicode_ext_components(const char *ext, size_t len, IntlExtension *out);
char *intl_insert_unicode_ext(JSContext *ctx, const char *locale, const IntlExtension *ext);
char *intl_strip_unicode_ext(JSContext *ctx, const char *tag);
bool intl_add_likely_subtags(IntlLangId *id);
bool intl_remove_likely_subtags(IntlLangId *id);
char *intl_canonicalize_uvalue(JSContext *ctx, const char *key, const char *value);
bool intl_is_type_sequence(const char *s);
bool intl_is_lang_subtag(const char *s, size_t len);
bool intl_is_script_subtag(const char *s, size_t len);
bool intl_is_region_subtag(const char *s, size_t len);
bool intl_is_variant_subtag(const char *s, size_t len);
const void *intl_region_info(const char *region);

void intl_locale_list_free(JSContext *ctx, IntlLocaleList *list);
int intl_canonicalize_locale_list(JSContext *ctx, JSValueConst locales, IntlLocaleList *list);
char *intl_default_locale(JSContext *ctx);

typedef struct IntlResolverOps {
    const char *const *keys;
    int nkeys;
    bool (*key_supported)(const char *locale, const char *key, const char *value);
    const char *(*key_default)(const char *locale, const char *key);
} IntlResolverOps;

typedef struct IntlResolved {
    char locale[INTL_TAG_MAX];
    char data_locale[INTL_TAG_MAX];
    char values[8][64];
} IntlResolved;

extern const char intl_option_null[];
int intl_resolve_locale(JSContext *ctx, int service, const IntlLocaleList *requested, bool lookup,
                        const IntlResolverOps *ops, char *const *options, IntlResolved *res);
const char *intl_lookup_available(int service, const char *tag, char *buf, size_t size);
bool intl_service_available(int service, const char *tag, size_t len);
bool intl_data_has_language(const char *tag, size_t len);
bool intl_collator_has_language(const char *tag, size_t len);
bool intl_locale_available(const char *tag, size_t len);
JSValue intl_supported_locales(JSContext *ctx, int service, JSValueConst locales, JSValueConst options);

JSValue intl_get_options_object(JSContext *ctx, JSValueConst options);
JSValue intl_coerce_options(JSContext *ctx, JSValueConst options);
int intl_get_string_option(JSContext *ctx, JSValueConst options, const char *name,
                           const char *const *values, const char *fallback,
                           char *buf, size_t size, bool *present);
int intl_get_enum_option(JSContext *ctx, JSValueConst options, const char *name,
                         const char *const *values, int fallback);
int intl_get_bool_option(JSContext *ctx, JSValueConst options, const char *name, int fallback);
int intl_get_number_option(JSContext *ctx, JSValueConst options, const char *name,
                           int minimum, int maximum, int fallback, int *out);
int intl_default_number_option(JSContext *ctx, JSValueConst value, int minimum, int maximum,
                               int fallback, int *out);
int intl_get_locale_matcher(JSContext *ctx, JSValueConst options, bool *lookup);
int intl_get_resolution_option(JSContext *ctx, JSValueConst options, const char *name,
                               const char *const *values, char *buf, size_t size, bool *present);

int intl_set_prop_str(JSContext *ctx, JSValueConst obj, const char *name, const char *value);
int intl_set_prop_value(JSContext *ctx, JSValueConst obj, const char *name, JSValue value);
JSValue intl_make_part(JSContext *ctx, const char *type, const char *value, size_t len);
int intl_push_part(JSContext *ctx, JSValueConst parts, uint32_t *idx, const char *type,
                   const char *value, size_t len, const char *extra_name, const char *extra_value);
JSValue intl_new_string_array(JSContext *ctx, const char *const *items, int count);
JSValue intl_throw_range(JSContext *ctx, const char *fmt, ...);
JSValue intl_throw_type(JSContext *ctx, const char *fmt, ...);
void intl_ascii_lower(char *s);
void intl_ascii_upper(char *s);
bool intl_ascii_eq_nocase(const char *a, const char *b);
char *intl_strdup(JSContext *ctx, const char *s);
char *intl_strndup(JSContext *ctx, const char *s, size_t len);
JSValue intl_this_object(JSContext *ctx, JSValueConst this_val, int cls, const char *method);
void *intl_get_object(JSContext *ctx, JSValueConst this_val, int cls, const char *method);

bool intl_is_well_formed_currency(const char *s);
bool intl_is_well_formed_unit(const char *s);
bool intl_is_sanctioned_unit(const char *s, size_t len);
int intl_currency_digits(const char *code);
const char *intl_numbering_system_digits(const char *name);
bool intl_is_numbering_system(const char *name);
bool intl_is_calendar(const char *name);
int intl_find_time_zone(const char *name, const char **identifier, const char **primary);
bool tp_tzdb_is_link(const char *name);
int intl_parse_offset_time_zone(const char *s, int *minutes);
const char *intl_script_direction(const char *script);

JSValue intl_locale_get_tag(JSContext *ctx, JSValueConst obj, bool *is_locale);

int intl_data_root(void);
int intl_data_node(const char *tag);
const char *intl_data_get(int node, const char *key);
const char *intl_data_getf(int node, const char *fmt, ...);
const char *intl_data_node_name(int node);

enum { INTL_DEC_FINITE, INTL_DEC_NAN, INTL_DEC_POS_INF, INTL_DEC_NEG_INF };

typedef struct IntlDecimal {
    int kind;
    bool negative;
    char *digits;
    int nd;
    int exp;
} IntlDecimal;

enum {
    INTL_ROUND_CEIL,
    INTL_ROUND_FLOOR,
    INTL_ROUND_EXPAND,
    INTL_ROUND_TRUNC,
    INTL_ROUND_HALF_CEIL,
    INTL_ROUND_HALF_FLOOR,
    INTL_ROUND_HALF_EXPAND,
    INTL_ROUND_HALF_TRUNC,
    INTL_ROUND_HALF_EVEN,
};

enum {
    INTL_ROUNDING_FRACTION,
    INTL_ROUNDING_SIGNIFICANT,
    INTL_ROUNDING_MORE,
    INTL_ROUNDING_LESS,
};

typedef struct IntlDigitOptions {
    int min_int;
    int min_frac;
    int max_frac;
    int min_sig;
    int max_sig;
    int rounding_type;
    int rounding_increment;
    int rounding_mode;
    int rounding_priority;
    bool trailing_zero_strip;
} IntlDigitOptions;

typedef struct IntlRawResult {
    char *str;
    IntlDecimal rounded;
    int int_digits;
    int rounding_magnitude;
} IntlRawResult;

void intl_decimal_free(IntlDecimal *d);
int intl_decimal_copy(IntlDecimal *dst, const IntlDecimal *src);
int intl_decimal_magnitude(const IntlDecimal *d);
int intl_decimal_from_string(const char *s, size_t len, IntlDecimal *d);
int intl_decimal_from_double(double x, IntlDecimal *d);
int intl_to_intl_mv(JSContext *ctx, JSValueConst value, IntlDecimal *d);
void intl_raw_result_free(IntlRawResult *r);
int intl_format_numeric_to_string(const IntlDigitOptions *o, const IntlDecimal *x, IntlRawResult *out);
extern const char *const intl_plural_categories[6];
int intl_plural_select(int data, bool ordinal, const char *formatted, int exponent);
int intl_plural_category_list(int data, bool ordinal, int *cats);
int intl_plural_select_range(int data, int start, int end);
int intl_set_digit_options(JSContext *ctx, IntlDigitOptions *o, JSValueConst options,
                           int mnfd_default, int mxfd_default, bool compact);
int intl_resolved_digit_options(JSContext *ctx, JSValueConst obj, const IntlDigitOptions *o,
                                bool before_categories);

typedef struct IntlPart {
    const char *type;
    char *value;
    int level;
    const char *source;
} IntlPart;

typedef struct IntlParts {
    IntlPart *items;
    int count;
    int size;
    bool error;
} IntlParts;

void intl_parts_init(IntlParts *p);
void intl_parts_free(IntlParts *p);
int intl_parts_add(IntlParts *p, const char *type, const char *s, size_t len, int level);
int intl_parts_append(IntlParts *dst, const IntlParts *src, const char *source);
JSValue intl_parts_to_array(JSContext *ctx, const IntlParts *p, const char *extra_name,
                            const char *extra_value, bool use_source);
JSValue intl_parts_to_string(JSContext *ctx, const IntlParts *p);
char *intl_parts_join(const IntlParts *p);

typedef struct IntlNumberFormat IntlNumberFormat;
extern const IntlResolverOps intl_nu_resolver;
int intl_nf_format_double(JSContext *ctx, JSValueConst nf_obj, double x, IntlParts *parts);
int intl_pr_select_double(JSContext *ctx, JSValueConst pr_obj, double x);
int intl_nf_format_decimal(JSContext *ctx, JSValueConst nf_obj, const char *s, IntlParts *parts);
int intl_lf_partition_strings(JSContext *ctx, JSValueConst lf_obj, const char *const *strs, uint32_t n,
                              IntlParts *parts);
JSValue intl_construct_intrinsic(JSContext *ctx, int intrinsic, JSValueConst locale, JSValueConst options);
typedef struct IntlPluralRules IntlPluralRules;
typedef struct IntlDateTimeFormat IntlDateTimeFormat;

int intl_define_service(JSContext *ctx, JSValueConst intl, const char *name, int cls, int intrinsic,
                        JSCFunction *ctor_fn, int length, JSCFunctionEnum cproto,
                        const JSCFunctionListEntry *proto_funcs, int nfuncs,
                        JSClassFinalizer *finalizer, JSClassGCMark *mark);
enum {
    INTL_CAL_BUDDHIST,
    INTL_CAL_CHINESE,
    INTL_CAL_COPTIC,
    INTL_CAL_DANGI,
    INTL_CAL_ETHIOAA,
    INTL_CAL_ETHIOPIC,
    INTL_CAL_GREGORY,
    INTL_CAL_HEBREW,
    INTL_CAL_INDIAN,
    INTL_CAL_ISLAMIC_CIVIL,
    INTL_CAL_ISLAMIC_TBLA,
    INTL_CAL_ISLAMIC_UMALQURA,
    INTL_CAL_ISO8601,
    INTL_CAL_JAPANESE,
    INTL_CAL_PERSIAN,
    INTL_CAL_ROC,
    INTL_CAL_COUNT,
};

typedef struct IntlCalendarDate {
    int64_t year;
    int64_t era_year;
    int64_t related_year;
    int64_t iso_year;
    int era;
    bool era_gregorian;
    int month;
    int month_key;
    int ordinal_month;
    bool leap_month;
    bool leap_key;
    int day;
    int weekday;
    int cyclic_year;
} IntlCalendarDate;

int intl_calendar_compute(int cal, int64_t date, IntlCalendarDate *out);
int64_t intl_fixed_from_gregorian(int64_t y, int m, int d);
void intl_gregorian_from_fixed(int64_t date, int64_t *py, int *pm, int *pd);
const char *intl_metazone_for(const char *zone, int64_t epoch_minutes);
const char *intl_time_zone_cldr_id(const char *name);
const char *intl_iana_to_windows_zone(const char *iana);
int intl_region_default_calendar(const char *region);
void intl_region_hour_cycle_prefs(const char *locale, const char *region, int *hc12, int *hc24);
const char *intl_default_numbering_system(const char *locale);
int js_intl_init_locale(JSContext *ctx, JSValueConst intl);
int js_intl_init_collator(JSContext *ctx, JSValueConst intl);
int js_intl_init_number_format(JSContext *ctx, JSValueConst intl);
int js_intl_init_plural_rules(JSContext *ctx, JSValueConst intl);
int js_intl_init_date_time_format(JSContext *ctx, JSValueConst intl);
int js_intl_init_list_format(JSContext *ctx, JSValueConst intl);
int js_intl_init_relative_time_format(JSContext *ctx, JSValueConst intl);
int js_intl_init_display_names(JSContext *ctx, JSValueConst intl);
int js_intl_init_duration_format(JSContext *ctx, JSValueConst intl);
int js_intl_init_segmenter(JSContext *ctx, JSValueConst intl);

#endif
