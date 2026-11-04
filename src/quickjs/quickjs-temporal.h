/* quickjs-temporal.h — internal declarations shared by the Temporal implementation files. */
#ifndef QUICKJS_TEMPORAL_H
#define QUICKJS_TEMPORAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

typedef __int128 tp_i128;

#define TP_NS_PER_DAY ((int64_t)86400000000000LL)
#define TP_NS_MAX_INSTANT ((tp_i128)TP_NS_PER_DAY * 100000000)
#define TP_TIME_DURATION_MAX (((tp_i128)9007199254740991LL) * 1000000000 + 999999999)
#define TP_FIELD_CLAMP ((int64_t)1000000000000LL)

enum {
    TP_KIND_INSTANT,
    TP_KIND_ZONED,
    TP_KIND_DATE,
    TP_KIND_DATETIME,
    TP_KIND_TIME,
    TP_KIND_YEAR_MONTH,
    TP_KIND_MONTH_DAY,
    TP_KIND_DURATION,
    TP_KIND_COUNT
};

enum {
    TP_UNIT_YEAR,
    TP_UNIT_MONTH,
    TP_UNIT_WEEK,
    TP_UNIT_DAY,
    TP_UNIT_HOUR,
    TP_UNIT_MINUTE,
    TP_UNIT_SECOND,
    TP_UNIT_MILLISECOND,
    TP_UNIT_MICROSECOND,
    TP_UNIT_NANOSECOND,
    TP_UNIT_COUNT,
    TP_UNIT_AUTO,
    TP_UNIT_UNSET
};

enum { TP_GROUP_DATE, TP_GROUP_TIME, TP_GROUP_DATETIME };

enum {
    TP_ROUND_CEIL,
    TP_ROUND_FLOOR,
    TP_ROUND_EXPAND,
    TP_ROUND_TRUNC,
    TP_ROUND_HALF_CEIL,
    TP_ROUND_HALF_FLOOR,
    TP_ROUND_HALF_EXPAND,
    TP_ROUND_HALF_TRUNC,
    TP_ROUND_HALF_EVEN
};

enum { TP_OVERFLOW_CONSTRAIN, TP_OVERFLOW_REJECT };
enum { TP_DISAMBIG_COMPATIBLE, TP_DISAMBIG_EARLIER, TP_DISAMBIG_LATER, TP_DISAMBIG_REJECT };
enum { TP_OFFSET_PREFER, TP_OFFSET_USE, TP_OFFSET_IGNORE, TP_OFFSET_REJECT };
enum { TP_OFFSET_BEHAVIOUR_OPTION, TP_OFFSET_BEHAVIOUR_EXACT, TP_OFFSET_BEHAVIOUR_WALL };
enum { TP_SHOW_CAL_AUTO, TP_SHOW_CAL_ALWAYS, TP_SHOW_CAL_NEVER, TP_SHOW_CAL_CRITICAL };
enum { TP_SHOW_TZ_AUTO, TP_SHOW_TZ_NEVER, TP_SHOW_TZ_CRITICAL };
enum { TP_SHOW_OFFSET_AUTO, TP_SHOW_OFFSET_NEVER };
enum { TP_FIELDS_DATE, TP_FIELDS_YEAR_MONTH, TP_FIELDS_MONTH_DAY };

enum {
    TP_DF_CALENDAR_ID,
    TP_DF_ERA,
    TP_DF_ERA_YEAR,
    TP_DF_YEAR,
    TP_DF_MONTH,
    TP_DF_MONTH_CODE,
    TP_DF_DAY,
    TP_DF_DAY_OF_WEEK,
    TP_DF_DAY_OF_YEAR,
    TP_DF_WEEK_OF_YEAR,
    TP_DF_YEAR_OF_WEEK,
    TP_DF_DAYS_IN_WEEK,
    TP_DF_DAYS_IN_MONTH,
    TP_DF_DAYS_IN_YEAR,
    TP_DF_MONTHS_IN_YEAR,
    TP_DF_IN_LEAP_YEAR,
    TP_DF_HOUR = 100,
    TP_DF_MINUTE,
    TP_DF_SECOND,
    TP_DF_MILLISECOND,
    TP_DF_MICROSECOND,
    TP_DF_NANOSECOND
};

#define tp_arg(i) ((i) < argc ? argv[i] : JS_UNDEFINED)

#define TP_PRECISION_AUTO (-1)
#define TP_PRECISION_MINUTE (-2)

enum {
    TP_F_ERA,
    TP_F_ERA_YEAR,
    TP_F_YEAR,
    TP_F_MONTH,
    TP_F_MONTH_CODE,
    TP_F_DAY,
    TP_F_HOUR,
    TP_F_MINUTE,
    TP_F_SECOND,
    TP_F_MILLISECOND,
    TP_F_MICROSECOND,
    TP_F_NANOSECOND,
    TP_F_OFFSET,
    TP_F_TIME_ZONE,
    TP_F_COUNT
};

#define TP_FB(f) (1u << (f))
#define TP_FM_DATE (TP_FB(TP_F_YEAR) | TP_FB(TP_F_MONTH) | TP_FB(TP_F_MONTH_CODE) | TP_FB(TP_F_DAY))
#define TP_FM_TIME (TP_FB(TP_F_HOUR) | TP_FB(TP_F_MINUTE) | TP_FB(TP_F_SECOND) | \
                    TP_FB(TP_F_MILLISECOND) | TP_FB(TP_F_MICROSECOND) | TP_FB(TP_F_NANOSECOND))

typedef int TPCalendar;
enum {
    TP_CAL_ISO8601,
    TP_CAL_BUDDHIST,
    TP_CAL_CHINESE,
    TP_CAL_COPTIC,
    TP_CAL_DANGI,
    TP_CAL_ETHIOAA,
    TP_CAL_ETHIOPIC,
    TP_CAL_GREGORY,
    TP_CAL_HEBREW,
    TP_CAL_INDIAN,
    TP_CAL_ISLAMIC_CIVIL,
    TP_CAL_ISLAMIC_TBLA,
    TP_CAL_ISLAMIC_UMALQURA,
    TP_CAL_JAPANESE,
    TP_CAL_PERSIAN,
    TP_CAL_ROC,
    TP_CAL_COUNT
};

typedef struct TPISODate {
    int64_t year;
    int32_t month;
    int32_t day;
} TPISODate;

typedef struct TPTime {
    int32_t hour;
    int32_t minute;
    int32_t second;
    int32_t millisecond;
    int32_t microsecond;
    int32_t nanosecond;
} TPTime;

typedef struct TPDateTime {
    TPISODate date;
    TPTime time;
} TPDateTime;

typedef struct TPDateDuration {
    int64_t years;
    int64_t months;
    int64_t weeks;
    int64_t days;
} TPDateDuration;

typedef struct TPInternalDuration {
    TPDateDuration date;
    tp_i128 time;
} TPInternalDuration;

typedef struct TPDuration {
    double f[TP_UNIT_COUNT];
} TPDuration;

typedef struct TPTimeZone {
    int32_t id;
    int32_t offset_minutes;
} TPTimeZone;

#define TP_TZ_OFFSET_ID (-1)
#define TP_TZ_UTC_ID 0

typedef struct TPFields {
    uint32_t has;
    int64_t v[TP_F_COUNT];
    char era[32];
    bool month_code_leap;
    int64_t offset_ns;
    TPTimeZone tz;
} TPFields;

typedef struct TPCalendarDate {
    bool has_era;
    char era[32];
    int64_t era_year;
    int64_t year;
    int32_t month;
    int32_t month_code;
    bool month_code_leap;
    int32_t day;
    int32_t day_of_week;
    int32_t day_of_year;
    bool has_week_of_year;
    int32_t week_of_year;
    int64_t year_of_week;
    int32_t days_in_week;
    int32_t days_in_month;
    int32_t days_in_year;
    int32_t months_in_year;
    bool in_leap_year;
} TPCalendarDate;

typedef struct TPObject {
    int kind;
    TPCalendar cal;
    tp_i128 ns __attribute__((packed, aligned(8)));
    TPTimeZone tz;
    TPDateTime dt;
    TPDuration dur;
} TPObject;

enum {
    TP_GOAL_DATETIME_ZONED,
    TP_GOAL_DATETIME,
    TP_GOAL_INSTANT,
    TP_GOAL_TIME,
    TP_GOAL_MONTH_DAY,
    TP_GOAL_YEAR_MONTH,
    TP_GOAL_END
};

typedef struct TPParsed {
    bool has_year;
    int64_t year;
    int32_t month;
    int32_t day;
    bool has_time;
    TPTime time;
    bool z;
    bool has_offset;
    int64_t offset_ns;
    bool offset_sub_minute;
    size_t tz_start;
    size_t tz_len;
    size_t cal_start;
    size_t cal_len;
} TPParsed;

typedef struct TPTimeZoneParse {
    bool is_offset;
    int32_t offset_minutes;
    size_t name_start;
    size_t name_len;
} TPTimeZoneParse;

typedef struct TPPrecision {
    int precision;
    int unit;
    int64_t increment;
} TPPrecision;

typedef struct TPDiffSettings {
    int smallest_unit;
    int largest_unit;
    int rounding_mode;
    int64_t rounding_increment;
} TPDiffSettings;

typedef struct TPRelativeTo {
    int kind;
    TPISODate date;
    TPCalendar cal;
    tp_i128 ns;
    TPTimeZone tz;
} TPRelativeTo;

enum { TP_RELATIVE_NONE, TP_RELATIVE_PLAIN, TP_RELATIVE_ZONED };

typedef struct TPBuf {
    char s[1024];
    int len;
} TPBuf;

extern JSClassID tp_class_id[TP_KIND_COUNT];
extern const char *const tp_unit_singular[TP_UNIT_COUNT];
extern const int64_t tp_unit_ns[TP_UNIT_COUNT];

JSClassID js_temporal_class_id(int kind);
void js_temporal_set_opaque(JSValueConst obj, void *opaque);
JSValue js_temporal_create_from_ctor(JSContext *ctx, JSValueConst ctor, JSClassID class_id);
int js_temporal_this_time_value(JSContext *ctx, double *pres, JSValueConst this_val);
JSValue js_temporal_date_prototype(JSContext *ctx);
JSValue js_temporal_to_primitive(JSContext *ctx, JSValueConst val, bool hint_string);
JSValue js_temporal_to_bigint(JSContext *ctx, JSValueConst val);
JSValue js_temporal_new_bigint128(JSContext *ctx, tp_i128 v);
int js_temporal_bigint_to_i128(JSContext *ctx, tp_i128 *pres, JSValueConst val);

int64_t tp_floor_div64(int64_t a, int64_t b);
int64_t tp_floor_mod64(int64_t a, int64_t b);
tp_i128 tp_floor_div128(tp_i128 a, tp_i128 b);
tp_i128 tp_floor_mod128(tp_i128 a, tp_i128 b);
tp_i128 tp_abs128(tp_i128 a);
int tp_sign128(tp_i128 a);
int tp_cmp128(tp_i128 a, tp_i128 b);
int tp_i128_to_str(char *buf, tp_i128 v);
double tp_i128_to_double(tp_i128 v);
double tp_i128_fdiv(tp_i128 num, tp_i128 den);
bool tp_double_to_i128(double d, tp_i128 *out);
int64_t tp_clamp_double(double d);

void tp_buf_init(TPBuf *b);
void tp_buf_putc(TPBuf *b, char c);
void tp_buf_puts(TPBuf *b, const char *s);
void tp_buf_putn(TPBuf *b, const char *s, size_t n);
void tp_buf_pad(TPBuf *b, int64_t v, int width);
JSValue tp_buf_to_string(JSContext *ctx, TPBuf *b);

TPObject *tp_this(JSContext *ctx, JSValueConst this_val, int kind);
TPObject *tp_get(JSValueConst v, int kind);
JSValue tp_alloc(JSContext *ctx, JSValueConst new_target, int kind, TPObject **pobj);

int tp_to_integer_with_truncation(JSContext *ctx, JSValueConst v, double *out);
int tp_to_positive_integer_with_truncation(JSContext *ctx, JSValueConst v, double *out);
int tp_to_integer_if_integral(JSContext *ctx, JSValueConst v, double *out);
int tp_require_string(JSContext *ctx, JSValueConst v);
const char *tp_to_cstring_ascii(JSContext *ctx, JSValueConst v, size_t *plen);

int tp_get_options_object(JSContext *ctx, JSValueConst options, JSValue *out);
int tp_get_string_option(JSContext *ctx, JSValueConst options, const char *prop,
                         const char *const *values, int count, int fallback, int *out);
int tp_get_overflow_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_disambiguation_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_offset_option(JSContext *ctx, JSValueConst options, int fallback, int *out);
int tp_get_show_calendar_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_show_time_zone_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_show_offset_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_direction_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_rounding_mode_option(JSContext *ctx, JSValueConst options, int fallback, int *out);
int tp_get_rounding_increment_option(JSContext *ctx, JSValueConst options, int64_t *out);
int tp_get_fractional_second_digits_option(JSContext *ctx, JSValueConst options, int *out);
int tp_get_unit_option(JSContext *ctx, JSValueConst options, const char *key, bool required, int *out);
int tp_validate_unit(JSContext *ctx, int unit, int group, int extra1, int extra2);
int tp_validate_rounding_increment(JSContext *ctx, int64_t increment, int64_t dividend, bool inclusive);
int tp_maximum_rounding_increment(int unit);
TPPrecision tp_seconds_precision(int smallest_unit, int digits);
int tp_negate_rounding_mode(int mode);
int tp_larger_unit(int a, int b);
bool tp_is_calendar_unit(int unit);
int tp_unit_category(int unit);
int tp_round_options_from_arg(JSContext *ctx, JSValueConst arg, const char *key, JSValue *out);

tp_i128 tp_round_to_increment(tp_i128 x, tp_i128 increment, int mode);
tp_i128 tp_round_to_increment_as_if_positive(tp_i128 x, tp_i128 increment, int mode);
int tp_unsigned_rounding_mode(int mode, bool negative);
tp_i128 tp_apply_unsigned_rounding(tp_i128 r1, tp_i128 r2, int cmp, bool even, int umode);

bool tp_is_leap_year(int64_t year);
int tp_days_in_month(int64_t year, int month);
int tp_days_in_year(int64_t year);
int64_t tp_epoch_days(int64_t year, int64_t month0, int64_t day);
TPISODate tp_epoch_days_to_date(int64_t days);
int64_t tp_iso_date_epoch_days(TPISODate d);
TPISODate tp_balance_year_month(int64_t year, int64_t month);
TPISODate tp_add_days(TPISODate d, int64_t days);
bool tp_is_valid_iso_date(int64_t year, int64_t month, int64_t day);
bool tp_is_valid_time(int64_t h, int64_t mi, int64_t s, int64_t ms, int64_t us, int64_t ns);
int tp_regulate_iso_date(JSContext *ctx, int64_t year, int64_t month, int64_t day, int overflow, TPISODate *out);
int tp_regulate_time(JSContext *ctx, const int64_t v[6], int overflow, TPTime *out);
int tp_compare_iso_date(TPISODate a, TPISODate b);
int tp_compare_time(const TPTime *a, const TPTime *b);
int tp_compare_datetime(const TPDateTime *a, const TPDateTime *b);
bool tp_iso_date_within_limits(TPISODate d);
bool tp_iso_datetime_within_limits(const TPDateTime *dt);
bool tp_iso_year_month_within_limits(TPISODate d);
bool tp_epoch_ns_valid(tp_i128 ns);
int tp_check_iso_days_range(JSContext *ctx, TPISODate d);
tp_i128 tp_time_to_ns(const TPTime *t);
tp_i128 tp_utc_epoch_ns(const TPDateTime *dt);
void tp_epoch_ns_to_datetime(tp_i128 ns, TPDateTime *out);
int64_t tp_balance_time_ns(tp_i128 ns, TPTime *out);
TPDateTime tp_balance_datetime(TPISODate d, tp_i128 time_ns);
int64_t tp_add_time(const TPTime *t, tp_i128 duration_ns, TPTime *out);
tp_i128 tp_difference_time(const TPTime *a, const TPTime *b);
int64_t tp_round_time(const TPTime *t, int64_t increment, int unit, int mode, TPTime *out);
TPDateTime tp_round_iso_datetime(const TPDateTime *dt, int64_t increment, int unit, int mode);
int tp_iso_day_of_week(TPISODate d);
int tp_iso_day_of_year(TPISODate d);
void tp_iso_week_of_year(TPISODate d, int *week, int64_t *year);
TPTime tp_midnight(void);

bool tp_ncal_supports_era(TPCalendar cal);
void tp_ncal_iso_to_date(TPCalendar cal, TPISODate d, TPCalendarDate *out);
uint32_t tp_ncal_field_keys_to_ignore(TPCalendar cal, uint32_t keys);
int tp_ncal_resolve_fields(JSContext *ctx, TPCalendar cal, TPFields *f, int type);
int tp_ncal_date_to_iso(JSContext *ctx, TPCalendar cal, TPFields *f, int overflow, TPISODate *out);
int tp_ncal_month_day_to_iso(JSContext *ctx, TPCalendar cal, TPFields *f, int overflow, TPISODate *out);
void tp_ncal_date_until(TPCalendar cal, TPISODate one, TPISODate two, int largest_unit, TPDateDuration *out);
int tp_ncal_date_add(JSContext *ctx, TPCalendar cal, TPISODate d, const TPDateDuration *dur, int overflow,
                     TPISODate *out);
int tp_canonicalize_calendar(JSContext *ctx, const char *s, size_t len, TPCalendar *out);
const char *tp_calendar_id(TPCalendar cal);
int tp_to_calendar_identifier(JSContext *ctx, JSValueConst v, TPCalendar *out);
int tp_get_calendar_with_iso_default(JSContext *ctx, JSValueConst item, TPCalendar *out);
bool tp_calendar_equals(TPCalendar a, TPCalendar b);
void tp_calendar_iso_to_date(TPCalendar cal, TPISODate d, TPCalendarDate *out);
int tp_calendar_date_add(JSContext *ctx, TPCalendar cal, TPISODate d, const TPDateDuration *dur,
                         int overflow, TPISODate *out);
void tp_calendar_date_until(TPCalendar cal, TPISODate one, TPISODate two, int largest_unit,
                            TPDateDuration *out);
int tp_prepare_calendar_fields(JSContext *ctx, TPCalendar cal, JSValueConst obj, uint32_t cal_fields,
                               uint32_t noncal_fields, uint32_t required, bool partial, TPFields *out);
void tp_calendar_merge_fields(TPCalendar cal, const TPFields *fields, const TPFields *additional,
                              TPFields *out);
void tp_iso_date_to_fields(TPCalendar cal, TPISODate d, int type, TPFields *out);
int tp_calendar_date_from_fields(JSContext *ctx, TPCalendar cal, TPFields *fields, int overflow,
                                 TPISODate *out);
int tp_calendar_year_month_from_fields(JSContext *ctx, TPCalendar cal, TPFields *fields, int overflow,
                                       TPISODate *out);
int tp_calendar_month_day_from_fields(JSContext *ctx, TPCalendar cal, TPFields *fields, int overflow,
                                      TPISODate *out);
void tp_fields_set_time(TPFields *f, const TPTime *t);
int tp_interpret_datetime_fields(JSContext *ctx, TPCalendar cal, TPFields *fields, int overflow,
                                 TPDateTime *out);
JSValue tp_calendar_month_code_string(JSContext *ctx, int month, bool leap);
JSValue tp_date_field_value(JSContext *ctx, TPCalendar cal, TPISODate d, int field);
JSValue tp_time_field_value(JSContext *ctx, const TPTime *t, int field);
int tp_reject_temporal_like_object(JSContext *ctx, JSValueConst v);

int tp_parse_iso_datetime(JSContext *ctx, const char *s, size_t len, const int *goals, TPParsed *out);
int tp_parse_duration(JSContext *ctx, const char *s, size_t len, TPDuration *out);
bool tp_parse_utc_offset(const char *s, size_t len, bool sub_minute, int64_t *ns, bool *has_seconds);
bool tp_parse_time_zone_identifier(const char *s, size_t len, TPTimeZoneParse *out);
bool tp_parse_annotation_value(const char *s, size_t len);
int tp_parse_month_code(JSContext *ctx, JSValueConst v, int *num, bool *leap);
int tp_parse_offset_string_value(JSContext *ctx, JSValueConst v, int64_t *ns);

int tp_time_zone_from_identifier(JSContext *ctx, const char *s, size_t len, TPTimeZone *out);
int tp_to_time_zone_identifier(JSContext *ctx, JSValueConst v, TPTimeZone *out);
JSValue tp_time_zone_to_string(JSContext *ctx, TPTimeZone tz);
void tp_time_zone_put(TPBuf *b, TPTimeZone tz);
bool tp_time_zone_equals(TPTimeZone a, TPTimeZone b);
bool tp_time_zone_is_offset_or_utc(TPTimeZone tz);
int64_t tp_offset_ns_for(TPTimeZone tz, tp_i128 ns);
void tp_iso_datetime_for(TPTimeZone tz, tp_i128 ns, TPDateTime *out);
int tp_possible_epoch_ns(JSContext *ctx, TPTimeZone tz, const TPDateTime *dt, tp_i128 out[2], int *count);
int tp_epoch_ns_for(JSContext *ctx, TPTimeZone tz, const TPDateTime *dt, int disambiguation, tp_i128 *out);
int tp_start_of_day(JSContext *ctx, TPTimeZone tz, TPISODate d, tp_i128 *out);
int tp_interpret_iso_datetime_offset(JSContext *ctx, TPISODate d, const TPTime *time, int behaviour,
                                     int64_t offset_ns, TPTimeZone tz, int disambiguation,
                                     int offset_option, bool match_minutes, tp_i128 *out);
bool tp_next_transition(TPTimeZone tz, tp_i128 ns, tp_i128 *out);
bool tp_previous_transition(TPTimeZone tz, tp_i128 ns, tp_i128 *out);
void tp_format_offset_ns(TPBuf *b, int64_t offset_ns);
void tp_format_offset_rounded(TPBuf *b, int64_t offset_ns);
void tp_format_offset_minutes(TPBuf *b, int64_t minutes);
TPTimeZone tp_system_time_zone(void);
tp_i128 tp_system_epoch_ns(void);

int tp_tzdb_lookup(const char *s, size_t len);
const char *tp_tzdb_identifier(int id);
int tp_tzdb_primary(int id);
int64_t tp_tzdb_offset_ns(int id, tp_i128 ns);
int tp_tzdb_possible(int id, const TPDateTime *dt, tp_i128 out[2]);
bool tp_tzdb_next_transition(int id, tp_i128 ns, tp_i128 *out);
bool tp_tzdb_previous_transition(int id, tp_i128 ns, tp_i128 *out);
int tp_tzdb_system(void);
#if defined(_WIN32)
const char *tp_windows_system_zone(void);
#endif

void tp_put_iso_year(TPBuf *b, int64_t year);
void tp_put_iso_date(TPBuf *b, TPISODate d);
void tp_put_time(TPBuf *b, int h, int m, int s, int64_t subsec, int precision);
void tp_put_fraction(TPBuf *b, int64_t subsec, int precision);
void tp_put_calendar_annotation(TPBuf *b, TPCalendar cal, int show);
void tp_put_iso_datetime(TPBuf *b, const TPDateTime *dt, TPCalendar cal, int precision, int show_cal);

bool tp_is_valid_duration(const TPDuration *d);
int tp_validate_duration(JSContext *ctx, const TPDuration *d);
JSValue tp_new_duration(JSContext *ctx, const TPDuration *d, JSValueConst new_target);
int tp_to_temporal_duration(JSContext *ctx, JSValueConst v, TPDuration *out);
void tp_negate_duration(TPDuration *d);
int tp_duration_sign(const TPDuration *d);
int tp_date_duration_sign(const TPDateDuration *d);
int tp_internal_duration_sign(const TPInternalDuration *d);
int tp_default_largest_unit(const TPDuration *d);
tp_i128 tp_time_duration_from_components(const TPDuration *d);
void tp_to_internal_duration(const TPDuration *d, TPInternalDuration *out);
int tp_to_internal_duration_24h(JSContext *ctx, const TPDuration *d, TPInternalDuration *out);
int tp_to_date_duration_without_time(JSContext *ctx, const TPDuration *d, TPDateDuration *out);
int tp_duration_from_internal(JSContext *ctx, const TPInternalDuration *d, int largest_unit, TPDuration *out);
int tp_check_time_duration(JSContext *ctx, tp_i128 t);
int tp_time_duration_add_days(JSContext *ctx, tp_i128 t, int64_t days, tp_i128 *out);
int tp_round_time_duration(JSContext *ctx, tp_i128 t, int64_t increment, int unit, int mode, tp_i128 *out);
double tp_total_time_duration(tp_i128 t, int unit);
int tp_adjust_date_duration(JSContext *ctx, const TPDateDuration *d, int64_t days, const int64_t *weeks,
                            const int64_t *months, TPDateDuration *out);
int tp_add_instant(JSContext *ctx, tp_i128 ns, tp_i128 t, tp_i128 *out);
int tp_add_zoned_datetime(JSContext *ctx, tp_i128 ns, TPTimeZone tz, TPCalendar cal,
                          const TPInternalDuration *d, int overflow, tp_i128 *out);
int tp_difference_iso_datetime(JSContext *ctx, const TPDateTime *a, const TPDateTime *b, TPCalendar cal,
                               int largest_unit, TPInternalDuration *out);
int tp_difference_zoned_datetime(JSContext *ctx, tp_i128 ns1, tp_i128 ns2, TPTimeZone tz, TPCalendar cal,
                                 int largest_unit, TPInternalDuration *out);
int tp_round_relative_duration(JSContext *ctx, const TPInternalDuration *d, tp_i128 origin_ns, tp_i128 dest_ns,
                               const TPDateTime *dt, const TPTimeZone *tz, TPCalendar cal, int largest_unit,
                               int64_t increment, int smallest_unit, int mode, TPInternalDuration *out);
int tp_difference_plain_datetime_with_rounding(JSContext *ctx, const TPDateTime *a, const TPDateTime *b,
                                               TPCalendar cal, int largest_unit, int64_t increment,
                                               int smallest_unit, int mode, TPInternalDuration *out);
int tp_difference_plain_datetime_with_total(JSContext *ctx, const TPDateTime *a, const TPDateTime *b,
                                            TPCalendar cal, int unit, double *out);
int tp_difference_zoned_datetime_with_rounding(JSContext *ctx, tp_i128 ns1, tp_i128 ns2, TPTimeZone tz,
                                               TPCalendar cal, int largest_unit, int64_t increment,
                                               int smallest_unit, int mode, TPInternalDuration *out);
int tp_difference_zoned_datetime_with_total(JSContext *ctx, tp_i128 ns1, tp_i128 ns2, TPTimeZone tz,
                                            TPCalendar cal, int unit, double *out);
int tp_difference_instant(JSContext *ctx, tp_i128 ns1, tp_i128 ns2, int64_t increment, int smallest_unit,
                          int mode, TPInternalDuration *out);
int tp_get_difference_settings(JSContext *ctx, bool since, JSValueConst options, int group,
                               uint32_t disallowed, int fallback_smallest, int smallest_largest_default,
                               TPDiffSettings *out);
int tp_get_relative_to_option(JSContext *ctx, JSValueConst options, TPRelativeTo *out);
JSValue tp_duration_result(JSContext *ctx, const TPInternalDuration *d, int largest_unit, bool negate);
void tp_put_duration(TPBuf *b, const TPDuration *d, int precision);

JSValue tp_new_instant(JSContext *ctx, tp_i128 ns, JSValueConst new_target);
JSValue tp_new_zoned(JSContext *ctx, tp_i128 ns, TPTimeZone tz, TPCalendar cal, JSValueConst new_target);
JSValue tp_new_plain_date(JSContext *ctx, TPISODate d, TPCalendar cal, JSValueConst new_target);
JSValue tp_new_plain_datetime(JSContext *ctx, const TPDateTime *dt, TPCalendar cal, JSValueConst new_target);
JSValue tp_new_plain_time(JSContext *ctx, const TPTime *t, JSValueConst new_target);
JSValue tp_new_year_month(JSContext *ctx, TPISODate d, TPCalendar cal, JSValueConst new_target);
JSValue tp_new_month_day(JSContext *ctx, TPISODate d, TPCalendar cal, JSValueConst new_target);

int tp_to_temporal_instant(JSContext *ctx, JSValueConst v, tp_i128 *out);
int tp_to_temporal_date(JSContext *ctx, JSValueConst v, JSValueConst options, TPISODate *d, TPCalendar *cal);
int tp_to_temporal_datetime(JSContext *ctx, JSValueConst v, JSValueConst options, TPDateTime *dt,
                            TPCalendar *cal);
int tp_to_temporal_time(JSContext *ctx, JSValueConst v, JSValueConst options, TPTime *out);
int tp_to_time_record_or_midnight(JSContext *ctx, JSValueConst v, TPTime *out);
int tp_to_temporal_year_month(JSContext *ctx, JSValueConst v, JSValueConst options, TPISODate *d,
                              TPCalendar *cal);
int tp_to_temporal_month_day(JSContext *ctx, JSValueConst v, JSValueConst options, TPISODate *d,
                             TPCalendar *cal);
int tp_to_temporal_zoned(JSContext *ctx, JSValueConst v, JSValueConst options, tp_i128 *ns, TPTimeZone *tz,
                         TPCalendar *cal);
int tp_to_temporal_time_record(JSContext *ctx, JSValueConst v, bool partial, int64_t out[6], bool present[6]);

int tp_init_duration(JSContext *ctx, JSValue ns);
int tp_init_instant(JSContext *ctx, JSValue ns);
int tp_init_zoned(JSContext *ctx, JSValue ns);
int tp_init_plain_date(JSContext *ctx, JSValue ns);
int tp_init_plain_datetime(JSContext *ctx, JSValue ns);
int tp_init_plain_time(JSContext *ctx, JSValue ns);
int tp_init_year_month(JSContext *ctx, JSValue ns);
int tp_init_month_day(JSContext *ctx, JSValue ns);
int tp_init_now(JSContext *ctx, JSValue ns);
int tp_define_class(JSContext *ctx, JSValue ns, int kind, const char *name, JSCFunction *ctor, int length,
                    const JSCFunctionListEntry *proto_funcs, int proto_count,
                    const JSCFunctionListEntry *static_funcs, int static_count);
JSValue tp_value_of_throws(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

#endif
