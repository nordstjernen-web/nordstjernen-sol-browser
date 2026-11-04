/* quickjs-intl-datetime.c: Intl.DateTimeFormat, time zones and Date.prototype.toLocaleString/DateString/TimeString. */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "quickjs-intl.h"
#include "quickjs-temporal.h"

#define MS_PER_DAY 86400000.0
#define RD_EPOCH_1970 719163

enum { F_WEEKDAY, F_ERA, F_YEAR, F_MONTH, F_DAY, F_DAYPERIOD, F_HOUR, F_MINUTE, F_SECOND, F_FSD, F_TZNAME, F_COUNT };

static const char *const field_names[F_COUNT] = {
    "weekday", "era", "year", "month", "day", "dayPeriod", "hour", "minute", "second",
    "fractionalSecondDigits", "timeZoneName",
};
static const char *const text3[] = { "narrow", "short", "long", NULL };
static const char *const num2[] = { "2-digit", "numeric", NULL };
static const char *const month5[] = { "2-digit", "numeric", "narrow", "short", "long", NULL };
static const char *const tzn6[] = { "short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric", NULL };
static const char *const *const field_values[F_COUNT] = {
    text3, text3, num2, month5, num2, text3, num2, num2, num2, NULL, tzn6,
};
static const char *const style_names[] = { "full", "long", "medium", "short", NULL };
static const char *const hc_names[] = { "h11", "h12", "h23", "h24", NULL };
static const char *const matcher_names[] = { "basic", "best fit", NULL };

static const char *const calendar_names[INTL_CAL_COUNT] = {
    "buddhist", "chinese", "coptic", "dangi", "ethioaa", "ethiopic", "gregory", "hebrew", "indian",
    "islamic-civil", "islamic-tbla", "islamic-umalqura", "iso8601", "japanese", "persian", "roc",
};
static const char calendar_codes[INTL_CAL_COUNT] = {
    'b', 'c', 'k', 'd', 'a', 'e', 'g', 'h', 'i', 'l', 'l', 'l', 'g', 'j', 'p', 'r',
};

enum { TZ_UTC, TZ_OFFSET, TZ_NAMED };
enum { REQ_ANY, REQ_DATE, REQ_TIME };
enum { DEF_DATE, DEF_TIME, DEF_ALL };

struct IntlDateTimeFormat {
    char locale[INTL_TAG_MAX];
    char data_locale[INTL_TAG_MAX];
    int data;
    int calendar;
    char numbering[16];
    const char *digits;
    char time_zone[64];
    char tz_primary[64];
    int tz_kind;
    int tz_offset;
    int tzdb;
    bool tz_host;
    bool zoned_defaults;
    int tz_win;
    int hour_cycle;
    int date_style;
    int time_style;
    int8_t fields[F_COUNT];
    char *pattern;
    char *range_skeleton;
    char *date_pattern;
    char *time_pattern;
    char *time_skeleton;
    char decimal[8];
    int8_t raw[F_COUNT];
    int raw_hc;
    struct IntlDateTimeFormat *temporal[6];
    bool temporal_done[6];
    JSValue bound_format;
};

static char *dup_n(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (r) {
        memcpy(r, s, n);
        r[n] = '\0';
    }
    return r;
}

static int calendar_index(const char *name)
{
    for (int i = 0; i < INTL_CAL_COUNT; i++)
        if (!strcmp(calendar_names[i], name))
            return i;
    return -1;
}

static char pattern_code(int cal)
{
    switch (cal) {
    case INTL_CAL_GREGORY:
    case INTL_CAL_ISO8601:
        return 'g';
    case INTL_CAL_CHINESE:
    case INTL_CAL_DANGI:
    case INTL_CAL_BUDDHIST:
    case INTL_CAL_PERSIAN:
    case INTL_CAL_JAPANESE:
    case INTL_CAL_ROC:
    case INTL_CAL_HEBREW:
        return calendar_codes[cal];
    case INTL_CAL_ISLAMIC_CIVIL:
    case INTL_CAL_ISLAMIC_TBLA:
    case INTL_CAL_ISLAMIC_UMALQURA:
        return 'l';
    default:
        return 'x';
    }
}

static const char *dget(const IntlDateTimeFormat *dtf, const char *fmt, ...)
{
    char key[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(key, sizeof(key), fmt, ap);
    va_end(ap);
    return intl_data_get(dtf->data, key);
}

static const char *cal_pattern(const IntlDateTimeFormat *dtf, const char *what)
{
    char pc = pattern_code(dtf->calendar);
    const char *p = dget(dtf, "%c%s", pc, what);
    if (!p && pc != 'g' && pc != 'x' && pc != 'c' && pc != 'd')
        p = dget(dtf, "x%s", what);
    if (!p && pc != 'g')
        p = dget(dtf, "g%s", what);
    return p;
}

static void put_digits(const IntlDateTimeFormat *dtf, DynBuf *b, const char *s)
{
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9' && dtf->digits && strcmp(dtf->numbering, "latn")) {
            const char *d = dtf->digits, *e;
            int idx = *s - '0';
            while (idx-- > 0) {
                d++;
                while ((*d & 0xc0) == 0x80)
                    d++;
            }
            e = d + 1;
            while ((*e & 0xc0) == 0x80)
                e++;
            dbuf_put(b, (const uint8_t *)d, e - d);
        } else {
            dbuf_putc(b, *s);
        }
    }
}

static void add_number_part(const IntlDateTimeFormat *dtf, IntlParts *parts, const char *type, int64_t v,
                            int min_digits)
{
    char buf[32];
    DynBuf b;
    if (v < 0)
        snprintf(buf, sizeof(buf), "-%0*lld", min_digits, (long long)-v);
    else
        snprintf(buf, sizeof(buf), "%0*lld", min_digits, (long long)v);
    dbuf_init(&b);
    put_digits(dtf, &b, buf);
    if (b.error)
        parts->error = true;
    else
        intl_parts_add(parts, type, (const char *)b.buf, b.size, 0);
    dbuf_free(&b);
}

static void add_text_part(IntlParts *parts, const char *type, const char *s)
{
    intl_parts_add(parts, type, s, strlen(s), 0);
}

static const char *pattern_next(const char *p, char *letter, int *count, DynBuf *lit)
{
    *letter = 0;
    *count = 0;
    while (*p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (lit->size > 0)
                return p;
            *letter = c;
            while (*p == c) {
                (*count)++;
                p++;
            }
            return p;
        }
        if (c == '\'') {
            if (p[1] == '\'') {
                dbuf_putc(lit, '\'');
                p += 2;
                continue;
            }
            p++;
            while (*p && !(*p == '\'' && p[1] != '\'')) {
                if (*p == '\'' && p[1] == '\'') {
                    dbuf_putc(lit, '\'');
                    p += 2;
                    continue;
                }
                dbuf_putc(lit, *p++);
            }
            if (*p == '\'')
                p++;
            continue;
        }
        dbuf_putc(lit, c);
        p++;
    }
    return p;
}

enum { K_ERA, K_YEAR, K_MONTH, K_DAY, K_WEEKDAY, K_PERIOD, K_HOUR, K_MINUTE, K_SECOND, K_FRAC, K_ZONE, K_COUNT };

typedef struct Skel {
    char ch[K_COUNT];
    int8_t n[K_COUNT];
} Skel;

static int kind_of(char c)
{
    switch (c) {
    case 'G': return K_ERA;
    case 'y': case 'Y': case 'u': case 'U': case 'r': return K_YEAR;
    case 'M': case 'L': return K_MONTH;
    case 'd': return K_DAY;
    case 'E': case 'c': case 'e': return K_WEEKDAY;
    case 'a': case 'b': case 'B': return K_PERIOD;
    case 'h': case 'H': case 'K': case 'k': case 'j': return K_HOUR;
    case 'm': return K_MINUTE;
    case 's': return K_SECOND;
    case 'S': return K_FRAC;
    case 'z': case 'v': case 'O': case 'V': case 'Z': return K_ZONE;
    default: return -1;
    }
}

static void skel_parse(const char *s, Skel *sk)
{
    memset(sk, 0, sizeof(*sk));
    while (*s) {
        char c = *s;
        int n = 0, k;
        while (*s == c) {
            n++;
            s++;
        }
        k = kind_of(c);
        if (k >= 0 && !sk->ch[k]) {
            sk->ch[k] = c;
            sk->n[k] = (int8_t)n;
        }
    }
}

static void skel_from_pattern(const char *pat, Skel *sk)
{
    DynBuf lit;
    const char *p = pat;
    memset(sk, 0, sizeof(*sk));
    dbuf_init(&lit);
    while (*p) {
        char letter;
        int count, k;
        p = pattern_next(p, &letter, &count, &lit);
        lit.size = 0;
        if (!letter)
            continue;
        k = kind_of(letter);
        if (k >= 0 && !sk->ch[k] && letter != 'a') {
            sk->ch[k] = letter;
            sk->n[k] = (int8_t)count;
        }
    }
    dbuf_free(&lit);
}

static bool is_text_field(int k, int n)
{
    if (k == K_MONTH)
        return n >= 3;
    return false;
}

static bool hour12_letter(char c)
{
    return c == 'h' || c == 'K';
}

static int skel_distance(const Skel *req, const Skel *cand)
{
    int d = 0;
    bool time_only = true;
    for (int k = K_ERA; k <= K_WEEKDAY; k++)
        if (req->ch[k])
            time_only = false;
    for (int k = 0; k < K_COUNT; k++) {
        if (req->ch[k] && !cand->ch[k]) {
            d += 0x1000;
        } else if (!req->ch[k] && cand->ch[k]) {
            d += time_only && k <= K_WEEKDAY ? 0x20000 : 0x10000;
        } else if (req->ch[k]) {
            if (k == K_HOUR && hour12_letter(req->ch[k]) != hour12_letter(cand->ch[k]))
                d += 0x100;
            else if (k == K_PERIOD && ((req->ch[k] == 'B') != (cand->ch[k] == 'B')))
                d += 0x100;
            else if (k == K_ZONE && req->ch[k] != cand->ch[k])
                d += 0x40;
            else if (k == K_YEAR) {
                if (cand->ch[k] == 'U' && req->ch[k] != 'U')
                    d += 0x80;
                else if ((req->n[k] == 2) != (cand->n[k] == 2 && cand->ch[k] == 'y'))
                    d += 1;
            }
            else if (is_text_field(k, req->n[k]) != is_text_field(k, cand->n[k]))
                d += 0x100;
            else
                d += abs(req->n[k] - cand->n[k]);
        }
    }
    return d;
}

typedef struct MatchCtx {
    const IntlDateTimeFormat *dtf;
    const Skel *req;
    int best;
    char best_key[64];
    const char *best_pattern;
    bool interval;
} MatchCtx;

static void match_key(MatchCtx *mc, const char *key, const char *pattern)
{
    Skel cand;
    int d;
    skel_parse(key, &cand);
    d = skel_distance(mc->req, &cand);
    if (d < mc->best) {
        mc->best = d;
        snprintf(mc->best_key, sizeof(mc->best_key), "%s", key);
        mc->best_pattern = pattern;
    }
}

static const char *const af_keys[] = {
    "Bh", "Bhm", "Bhms", "d", "E", "EBhm", "EBhms", "Ed", "Eh", "Ehm", "EHm", "Ehms", "EHms", "Gy", "GyMd",
    "GyMEd", "GyMMM", "GyMMMd", "GyMMMEd", "GyMMMM", "GyMMMMd", "GyMMMMEd", "GyM", "h", "H", "hm", "Hm",
    "hms", "Hms", "hmsv", "Hmsv", "hmv", "Hmv", "hv", "Hv", "M", "Md", "MEd", "MMM", "MMMd", "MMMEd",
    "MMMMd", "MMMMEd", "ms", "y", "yM", "yMd", "yMEd", "yMMM", "yMMMd", "yMMMEd", "yMMMM", "yMMMMd",
    "yMMMMEd", "yyyy", "yyyyM", "yyyyMd", "yyyyMEd", "yyyyMMM", "yyyyMMMd", "yyyyMMMEd", "yyyyMMMM",
    "yyyyMMMMd", "yyyyMMMMEd", "UM", "UMd", "UMMM", "UMMMd", "Ehm", "EEEEd", "MMMMW", "yQQQ", "yQQQQ",
    "GyMMMMEEEEd", "MEEEEd", "MMMMEEEEd", "yMEEEEd", "yMMMEEEEd", "yMMMMEEEEd", "Hmm", "hmm", "mmss",
    "MMdd", "yMM", "yMMdd", "MMMMdd", "Md", "MMd", "yMMMMccccd", "yQ", "yw", "MEd", "UMMMMd",
};

static void match_skeleton(const IntlDateTimeFormat *dtf, const Skel *req, MatchCtx *mc, bool interval)
{
    char pc = pattern_code(dtf->calendar);
    memset(mc, 0, sizeof(*mc));
    mc->dtf = dtf;
    mc->req = req;
    mc->best = 0x7fffffff;
    mc->interval = interval;
    for (size_t i = 0; i < countof(af_keys); i++) {
        const char *p = dget(dtf, "%caf.%s", pc, af_keys[i]);
        if (!p && pc != 'g' && pc != 'x' && pc != 'c' && pc != 'd')
            p = dget(dtf, "xaf.%s", af_keys[i]);
        if (p)
            match_key(mc, af_keys[i], p);
    }
    if (!mc->best_pattern && pc != 'g') {
        for (size_t i = 0; i < countof(af_keys); i++) {
            const char *p = dget(dtf, "gaf.%s", af_keys[i]);
            if (p)
                match_key(mc, af_keys[i], p);
        }
    }
}

static void adjust_pattern(const IntlDateTimeFormat *dtf, const char *pat, const Skel *req, DynBuf *out)
{
    DynBuf lit;
    const char *p = pat;
    dbuf_init(&lit);
    while (*p) {
        char letter;
        int count, k;
        p = pattern_next(p, &letter, &count, &lit);
        if (lit.size) {
            dbuf_putc(out, '\'');
            for (size_t i = 0; i < lit.size; i++) {
                if (lit.buf[i] == '\'')
                    dbuf_putc(out, '\'');
                dbuf_putc(out, lit.buf[i]);
            }
            dbuf_putc(out, '\'');
            lit.size = 0;
        }
        if (!letter)
            continue;
        k = kind_of(letter);
        if (k >= 0 && req->ch[k]) {
            switch (k) {
            case K_MONTH:
                if (req->n[k] >= 3 && count >= 3)
                    count = req->n[k];
                else if (req->n[k] < 3 && count < 3)
                    count = req->n[k];
                break;
            case K_WEEKDAY:
                count = req->n[k] < 3 ? 3 : req->n[k];
                if (letter != 'c')
                    letter = 'E';
                break;
            case K_ERA:
            case K_DAY:
                count = req->n[k];
                break;
            case K_YEAR:
                if (letter == 'y')
                    count = req->n[k] == 2 ? 2 : 1;
                break;
            case K_HOUR:
                letter = req->ch[k];
                if (req->n[k] == 2)
                    count = 2;
                break;
            case K_MINUTE:
            case K_SECOND:
                if (req->n[k] == 2)
                    count = 2;
                break;
            case K_ZONE:
                letter = req->ch[k];
                count = req->n[k];
                break;
            case K_PERIOD:
                if (letter == 'B' || req->ch[k] == 'B') {
                    letter = req->ch[k];
                    count = req->n[k];
                }
                break;
            default:
                break;
            }
        }
        for (int i = 0; i < count; i++)
            dbuf_putc(out, letter);
        if (k == K_SECOND && req->ch[K_FRAC]) {
            dbuf_putc(out, '\'');
            dbuf_putstr(out, dtf->decimal);
            dbuf_putc(out, '\'');
            for (int i = 0; i < req->n[K_FRAC]; i++)
                dbuf_putc(out, 'S');
        }
    }
    dbuf_free(&lit);
}

static bool pattern_has_kind(const char *pat, int kind)
{
    Skel sk;
    skel_from_pattern(pat, &sk);
    return sk.ch[kind] != 0;
}

static char *append_missing(const IntlDateTimeFormat *dtf, char *pat, const Skel *req)
{
    static const struct { int k; const char *item; } items[] = {
        { K_ERA, "Era" }, { K_YEAR, "Year" }, { K_MONTH, "Month" }, { K_DAY, "Day" },
        { K_WEEKDAY, "Day-Of-Week" }, { K_HOUR, "Hour" }, { K_MINUTE, "Minute" }, { K_SECOND, "Second" },
        { K_ZONE, "Timezone" },
    };
    for (size_t i = 0; i < countof(items); i++) {
        int k = items[i].k;
        const char *ap;
        DynBuf b;
        if (!req->ch[k] || pattern_has_kind(pat, k))
            continue;
        ap = cal_pattern(dtf, "ai.Timezone");
        if (k == K_ERA || k == K_WEEKDAY || k == K_YEAR || k == K_ZONE) {
            char key[32];
            snprintf(key, sizeof(key), "ai.%s", items[i].item);
            ap = cal_pattern(dtf, key);
        }
        if (!ap || strstr(ap, "{2}"))
            ap = "{0} {1}";
        dbuf_init(&b);
        for (const char *q = ap; *q; q++) {
            if (q[0] == '{' && q[1] == '0' && q[2] == '}') {
                dbuf_putstr(&b, pat);
                q += 2;
            } else if (q[0] == '{' && q[1] == '1' && q[2] == '}') {
                int n = req->n[k];
                char c = req->ch[k];
                if (k == K_WEEKDAY && n < 3)
                    n = 3;
                if (k == K_FRAC)
                    c = 'S';
                for (int j = 0; j < n; j++)
                    dbuf_putc(&b, c);
                q += 2;
            } else {
                if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) {
                    dbuf_putc(&b, '\'');
                    dbuf_putc(&b, *q);
                    dbuf_putc(&b, '\'');
                } else {
                    dbuf_putc(&b, *q);
                }
            }
        }
        dbuf_putc(&b, '\0');
        free(pat);
        pat = (char *)b.buf;
    }
    if (req->ch[K_FRAC] && !pattern_has_kind(pat, K_FRAC)) {
        DynBuf b;
        dbuf_init(&b);
        dbuf_putstr(&b, pat);
        for (int j = 0; j < req->n[K_FRAC]; j++)
            dbuf_putc(&b, 'S');
        dbuf_putc(&b, '\0');
        free(pat);
        pat = (char *)b.buf;
    }
    return pat;
}

static char *best_pattern_part(const IntlDateTimeFormat *dtf, const Skel *req, char *key_out, size_t key_size)
{
    MatchCtx mc;
    DynBuf b;
    char *pat;
    Skel r = *req;
    bool only_period = req->ch[K_PERIOD] && !req->ch[K_HOUR];
    if (only_period) {
        r.ch[K_PERIOD] = 0;
        r.n[K_PERIOD] = 0;
    }
    if (key_out)
        key_out[0] = '\0';
    for (int k = 0; k < K_COUNT; k++)
        if (r.ch[k])
            goto have_fields;
    dbuf_init(&b);
    if (only_period)
        for (int i = 0; i < req->n[K_PERIOD]; i++)
            dbuf_putc(&b, 'B');
    dbuf_putc(&b, '\0');
    return (char *)b.buf;
have_fields:
    {
        Skel match_req = r;
        match_req.ch[K_FRAC] = 0;
        match_req.n[K_FRAC] = 0;
        match_skeleton(dtf, &match_req, &mc, false);
        if (!mc.best_pattern)
            return NULL;
        if (key_out)
            snprintf(key_out, key_size, "%s", mc.best_key);
        dbuf_init(&b);
        adjust_pattern(dtf, mc.best_pattern, &r, &b);
        dbuf_putc(&b, '\0');
        if (b.error) {
            dbuf_free(&b);
            return NULL;
        }
        pat = append_missing(dtf, (char *)b.buf, &r);
    }
    if (only_period && pat) {
        dbuf_init(&b);
        dbuf_putstr(&b, pat);
        dbuf_putc(&b, ' ');
        for (int i = 0; i < req->n[K_PERIOD]; i++)
            dbuf_putc(&b, 'B');
        dbuf_putc(&b, '\0');
        free(pat);
        pat = (char *)b.buf;
    }
    return pat;
}

static int date_style_of(const Skel *req)
{
    if (req->n[K_MONTH] >= 4 && req->ch[K_WEEKDAY])
        return 0;
    if (req->n[K_MONTH] >= 4)
        return 1;
    if (req->n[K_MONTH] == 3)
        return 2;
    return 3;
}

static char *combine(const IntlDateTimeFormat *dtf, const char *date, const char *time, int style)
{
    char key[16];
    const char *conn;
    DynBuf b;
    snprintf(key, sizeof(key), "dta.%s", style_names[style]);
    conn = cal_pattern(dtf, key);
    if (!conn) {
        snprintf(key, sizeof(key), "dt.%s", style_names[style]);
        conn = cal_pattern(dtf, key);
    }
    if (!conn)
        conn = "{1} {0}";
    dbuf_init(&b);
    for (const char *q = conn; *q; q++) {
        if (q[0] == '{' && q[1] == '0' && q[2] == '}') {
            dbuf_putstr(&b, time);
            q += 2;
        } else if (q[0] == '{' && q[1] == '1' && q[2] == '}') {
            dbuf_putstr(&b, date);
            q += 2;
        } else {
            dbuf_putc(&b, *q);
        }
    }
    dbuf_putc(&b, '\0');
    if (b.error) {
        dbuf_free(&b);
        return NULL;
    }
    return (char *)b.buf;
}

static int build_pattern(IntlDateTimeFormat *dtf, const Skel *req)
{
    MatchCtx mc;
    Skel match_req = *req;
    char key[64];
    match_req.ch[K_FRAC] = 0;
    match_req.n[K_FRAC] = 0;
    if (req->ch[K_PERIOD] && !req->ch[K_HOUR]) {
        match_req.ch[K_PERIOD] = 0;
        match_req.n[K_PERIOD] = 0;
    }
    match_skeleton(dtf, &match_req, &mc, false);
    if (mc.best_pattern && mc.best < 0x1000) {
        dtf->pattern = best_pattern_part(dtf, req, key, sizeof(key));
        dtf->range_skeleton = strdup(key);
        return dtf->pattern ? 0 : -1;
    }
    {
        Skel ds = *req, ts = *req;
        bool has_date = false, has_time = false;
        for (int k = 0; k < K_COUNT; k++) {
            if (k <= K_WEEKDAY) {
                ts.ch[k] = 0;
                ts.n[k] = 0;
                has_date |= req->ch[k] != 0;
            } else {
                ds.ch[k] = 0;
                ds.n[k] = 0;
                has_time |= req->ch[k] != 0;
            }
        }
        if (!has_date || !has_time) {
            dtf->pattern = best_pattern_part(dtf, req, key, sizeof(key));
            dtf->range_skeleton = strdup(key);
            return dtf->pattern ? 0 : -1;
        }
        dtf->date_pattern = best_pattern_part(dtf, &ds, key, sizeof(key));
        dtf->range_skeleton = strdup(key);
        dtf->time_pattern = best_pattern_part(dtf, &ts, key, sizeof(key));
        dtf->time_skeleton = strdup(key);
        if (!dtf->date_pattern || !dtf->time_pattern)
            return -1;
        dtf->pattern = combine(dtf, dtf->date_pattern, dtf->time_pattern, date_style_of(req));
        return dtf->pattern ? 0 : -1;
    }
}

static char hour_letter(int hc)
{
    static const char letters[] = "KhHk";
    return letters[hc];
}

static int build_style_pattern(IntlDateTimeFormat *dtf, int default_hc)
{
    const char *date = NULL, *time = NULL;
    char key[16];
    char *time_adj = NULL;
    if (dtf->date_style >= 0) {
        snprintf(key, sizeof(key), "df.%s", style_names[dtf->date_style]);
        date = cal_pattern(dtf, key);
        if (!date)
            date = "y-MM-dd";
    }
    if (dtf->time_style >= 0) {
        snprintf(key, sizeof(key), "tf.%s", style_names[dtf->time_style]);
        time = cal_pattern(dtf, key);
        if (!time)
            time = "HH:mm:ss";
        if (dtf->hour_cycle >= 0 && dtf->hour_cycle != default_hc) {
            Skel sk;
            skel_from_pattern(time, &sk);
            sk.ch[K_HOUR] = hour_letter(dtf->hour_cycle);
            time_adj = best_pattern_part(dtf, &sk, NULL, 0);
            if (time_adj)
                time = time_adj;
        }
    }
    if (date && time) {
        dtf->date_pattern = strdup(date);
        dtf->time_pattern = strdup(time);
        dtf->pattern = combine(dtf, date, time, dtf->date_style);
    } else {
        dtf->pattern = strdup(date ? date : time);
    }
    free(time_adj);
    return dtf->pattern ? 0 : -1;
}

static void fields_from_pattern(IntlDateTimeFormat *dtf)
{
    DynBuf lit;
    const char *p = dtf->pattern;
    for (int i = 0; i < F_COUNT; i++)
        dtf->fields[i] = -1;
    dbuf_init(&lit);
    while (*p) {
        char c;
        int n;
        p = pattern_next(p, &c, &n, &lit);
        lit.size = 0;
        switch (c) {
        case 'G':
            dtf->fields[F_ERA] = n <= 3 ? 1 : n == 4 ? 2 : 0;
            break;
        case 'y':
            dtf->fields[F_YEAR] = n == 2 ? 0 : 1;
            break;
        case 'r':
        case 'U':
            if (dtf->fields[F_YEAR] < 0)
                dtf->fields[F_YEAR] = 1;
            break;
        case 'M':
        case 'L':
            dtf->fields[F_MONTH] = n == 1 ? 1 : n == 2 ? 0 : n == 3 ? 3 : n == 4 ? 4 : 2;
            break;
        case 'd':
            dtf->fields[F_DAY] = n == 2 ? 0 : 1;
            break;
        case 'E':
        case 'c':
            dtf->fields[F_WEEKDAY] = n <= 3 ? 1 : n == 4 ? 2 : n == 5 ? 0 : 1;
            break;
        case 'B':
            dtf->fields[F_DAYPERIOD] = n <= 3 ? 1 : n == 4 ? 2 : 0;
            break;
        case 'h':
        case 'H':
        case 'K':
        case 'k':
            dtf->fields[F_HOUR] = n == 2 ? 0 : 1;
            break;
        case 'm':
            dtf->fields[F_MINUTE] = n == 2 ? 0 : 1;
            break;
        case 's':
            dtf->fields[F_SECOND] = n == 2 ? 0 : 1;
            break;
        case 'S':
            dtf->fields[F_FSD] = n > 3 ? 3 : n;
            break;
        case 'z':
            dtf->fields[F_TZNAME] = n == 4 ? 1 : 0;
            break;
        case 'O':
            dtf->fields[F_TZNAME] = n == 4 ? 3 : 2;
            break;
        case 'v':
            dtf->fields[F_TZNAME] = n == 4 ? 5 : 4;
            break;
        default:
            break;
        }
    }
    dbuf_free(&lit);
}

static int system_time_zone(JSContext *ctx, char *buf, size_t size)
{
    const char *def = js_intl_runtime_default_time_zone(JS_GetRuntime(ctx));
    const char *id, *primary;
    int minutes;
    if (def && *def) {
        if (intl_find_time_zone(def, &id, &primary) >= 0) {
            snprintf(buf, size, "%s", id);
            return 0;
        }
        if (intl_parse_offset_time_zone(def, &minutes) == 0) {
            snprintf(buf, size, "%c%02d:%02d", minutes < 0 ? '-' : '+', abs(minutes) / 60, abs(minutes) % 60);
            return 0;
        }
    }
    {
        const char *tz = getenv("TZ");
        if (tz && *tz) {
            const char *z;
            if (*tz == ':')
                tz++;
            z = strstr(tz, "zoneinfo/");
            if (z)
                tz = z + 9;
            if (intl_find_time_zone(tz, &id, &primary) >= 0) {
                snprintf(buf, size, "%s", id);
                return 0;
            }
        }
    }
#if defined(_WIN32)
    {
        const char *iana = tp_windows_system_zone();
        if (iana && intl_find_time_zone(iana, &id, &primary) >= 0) {
            snprintf(buf, size, "%s", id);
            return 0;
        }
    }
#else
    {
        char link[1024];
        ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
        if (n > 0) {
            const char *z;
            link[n] = '\0';
            z = strstr(link, "zoneinfo/");
            if (z && intl_find_time_zone(z + 9, &id, &primary) >= 0) {
                snprintf(buf, size, "%s", id);
                return 0;
            }
        }
    }
#endif
    {
        double now = (double)time(NULL) * 1000.0;
        int off = -(int)js_intl_local_tza(ctx, now, true);
        if (off == 0) {
            snprintf(buf, size, "UTC");
        } else {
            snprintf(buf, size, "%c%02d:%02d", off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
        }
    }
    return 0;
}

int intl_parse_offset_time_zone(const char *s, int *minutes)
{
    int sign, h, m = 0;
    size_t len = strlen(s);
    if (len < 3 || (s[0] != '+' && s[0] != '-'))
        return -1;
    sign = s[0] == '-' ? -1 : 1;
    if (!(s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9'))
        return -1;
    h = (s[1] - '0') * 10 + (s[2] - '0');
    if (h > 23)
        return -1;
    if (len == 3) {
    } else if (len == 5 && s[3] >= '0' && s[3] <= '9' && s[4] >= '0' && s[4] <= '9') {
        m = (s[3] - '0') * 10 + (s[4] - '0');
    } else if (len == 6 && s[3] == ':' && s[4] >= '0' && s[4] <= '9' && s[5] >= '0' && s[5] <= '9') {
        m = (s[4] - '0') * 10 + (s[5] - '0');
    } else {
        return -1;
    }
    if (m > 59)
        return -1;
    *minutes = sign * (h * 60 + m);
    return 0;
}

#if defined(_WIN32)
typedef DWORD (WINAPI *EnumDynTzFn)(DWORD, PDYNAMIC_TIME_ZONE_INFORMATION);

static int win_zone_index(const char *iana)
{
    static EnumDynTzFn enum_fn;
    static bool loaded;
    const char *wname = intl_iana_to_windows_zone(iana);
    DYNAMIC_TIME_ZONE_INFORMATION d;
    if (!wname)
        return -1;
    if (!loaded) {
        HMODULE h = LoadLibraryA("advapi32.dll");
        if (h)
            enum_fn = (EnumDynTzFn)(void *)GetProcAddress(h, "EnumDynamicTimeZoneInformation");
        loaded = true;
    }
    if (!enum_fn)
        return -1;
    for (DWORD i = 0; enum_fn(i, &d) == ERROR_SUCCESS; i++) {
        char name[128];
        int k;
        for (k = 0; d.TimeZoneKeyName[k] && k < (int)sizeof(name) - 1; k++)
            name[k] = (char)d.TimeZoneKeyName[k];
        name[k] = '\0';
        if (!strcmp(name, wname))
            return (int)i;
    }
    return -1;
}

static int64_t win_rule_minutes(int64_t year, const SYSTEMTIME *st)
{
    int64_t first = intl_fixed_from_gregorian(year, st->wMonth, 1);
    int64_t day;
    int wd = (int)(((first % 7) + 7) % 7);
    int offset = (st->wDayOfWeek - wd + 7) % 7;
    day = first + offset + 7 * (st->wDay - 1);
    if (st->wDay >= 5) {
        int64_t next = st->wMonth == 12 ? intl_fixed_from_gregorian(year + 1, 1, 1)
                                        : intl_fixed_from_gregorian(year, st->wMonth + 1, 1);
        while (day >= next)
            day -= 7;
    }
    return (day - RD_EPOCH_1970) * 1440 + st->wHour * 60 + st->wMinute;
}

static int win_offset(int index, double tv, bool *dst)
{
    static EnumDynTzFn enum_fn;
    static bool loaded;
    DYNAMIC_TIME_ZONE_INFORMATION d;
    TIME_ZONE_INFORMATION tzi;
    int64_t y;
    int m, dd;
    *dst = false;
    if (!loaded) {
        HMODULE h = LoadLibraryA("advapi32.dll");
        if (h)
            enum_fn = (EnumDynTzFn)(void *)GetProcAddress(h, "EnumDynamicTimeZoneInformation");
        loaded = true;
    }
    if (!enum_fn || enum_fn((DWORD)index, &d) != ERROR_SUCCESS)
        return 0;
    intl_gregorian_from_fixed((int64_t)floor(tv / MS_PER_DAY) + RD_EPOCH_1970, &y, &m, &dd);
    if (y < 1601 || y > 30827 || !GetTimeZoneInformationForYear((USHORT)y, &d, &tzi))
        return -(int)d.Bias;
    if (tzi.StandardDate.wMonth == 0 || tzi.DaylightDate.wMonth == 0)
        return -(int)(tzi.Bias + tzi.StandardBias);
    {
        int64_t utc_min = (int64_t)floor(tv / 60000.0);
        int64_t start = win_rule_minutes(y, &tzi.DaylightDate) + tzi.Bias + tzi.StandardBias;
        int64_t end = win_rule_minutes(y, &tzi.StandardDate) + tzi.Bias + tzi.DaylightBias;
        bool in_dst = start < end ? (utc_min >= start && utc_min < end) : (utc_min >= start || utc_min < end);
        *dst = in_dst && tzi.DaylightBias != 0;
        return -(int)(tzi.Bias + (in_dst ? tzi.DaylightBias : tzi.StandardBias));
    }
}
#endif

static int zone_offset_minutes(JSContext *ctx, const IntlDateTimeFormat *dtf, double tv, bool *dst)
{
    *dst = false;
    if (dtf->tz_kind == TZ_UTC)
        return 0;
    if (dtf->tz_kind == TZ_OFFSET)
        return dtf->tz_offset;
    if (dtf->tz_host) {
        int off = -(int)js_intl_local_tza(ctx, tv, true);
        int64_t y;
        int m, d;
        intl_gregorian_from_fixed((int64_t)floor(tv / MS_PER_DAY) + RD_EPOCH_1970, &y, &m, &d);
        {
            double jan = (intl_fixed_from_gregorian(y, 1, 1) - RD_EPOCH_1970) * MS_PER_DAY;
            double jul = (intl_fixed_from_gregorian(y, 7, 1) - RD_EPOCH_1970) * MS_PER_DAY;
            int o1 = -(int)js_intl_local_tza(ctx, jan, true), o2 = -(int)js_intl_local_tza(ctx, jul, true);
            *dst = off > (o1 < o2 ? o1 : o2);
        }
        return off;
    }
    if (dtf->tzdb >= 0) {
        tp_i128 ns = (tp_i128)(int64_t)tv * 1000000;
        int off = (int)(tp_tzdb_offset_ns(dtf->tzdb, ns) / 60000000000LL);
        int64_t y;
        int m, d;
        intl_gregorian_from_fixed((int64_t)floor(tv / MS_PER_DAY) + RD_EPOCH_1970, &y, &m, &d);
        {
            tp_i128 jan = (tp_i128)((intl_fixed_from_gregorian(y, 1, 1) - RD_EPOCH_1970) * 86400LL) * 1000000000;
            tp_i128 jul = (tp_i128)((intl_fixed_from_gregorian(y, 7, 1) - RD_EPOCH_1970) * 86400LL) * 1000000000;
            int o1 = (int)(tp_tzdb_offset_ns(dtf->tzdb, jan) / 60000000000LL);
            int o2 = (int)(tp_tzdb_offset_ns(dtf->tzdb, jul) / 60000000000LL);
            *dst = off > (o1 < o2 ? o1 : o2);
        }
        return off;
    }
#if defined(_WIN32)
    if (dtf->tz_win >= 0)
        return win_offset(dtf->tz_win, tv, dst);
#endif
    return 0;
}

static void format_gmt(const IntlDateTimeFormat *dtf, int offset, bool longform, DynBuf *out)
{
    const char *gmt = dget(dtf, "zg"), *gmt0 = dget(dtf, "zg0"), *hf = dget(dtf, "zh");
    char num[32];
    DynBuf tmp;
    if (offset == 0) {
        dbuf_putstr(out, gmt0 ? gmt0 : "GMT");
        return;
    }
    if (!gmt)
        gmt = "GMT{0}";
    if (!hf)
        hf = "+HH:mm;-HH:mm";
    dbuf_init(&tmp);
    {
        const char *semi = strchr(hf, ';');
        const char *pat = offset >= 0 ? hf : (semi ? semi + 1 : hf);
        size_t plen = offset >= 0 ? (semi ? (size_t)(semi - hf) : strlen(hf)) : strlen(pat);
        int a = abs(offset), h = a / 60, m = a % 60;
        for (size_t i = 0; i < plen; i++) {
            char c = pat[i];
            if (c == 'H') {
                bool two = i + 1 < plen && pat[i + 1] == 'H';
                if (two)
                    i++;
                snprintf(num, sizeof(num), (two && longform) ? "%02d" : "%d", h);
                put_digits(dtf, &tmp, num);
            } else if (c == 'm') {
                bool two = i + 1 < plen && pat[i + 1] == 'm';
                if (two)
                    i++;
                if (longform || m != 0) {
                    snprintf(num, sizeof(num), "%02d", m);
                    put_digits(dtf, &tmp, num);
                } else if (tmp.size > 0 && (tmp.buf[tmp.size - 1] == ':' || tmp.buf[tmp.size - 1] == '.')) {
                    tmp.size--;
                }
            } else if (c == '\'') {
                continue;
            } else {
                dbuf_putc(&tmp, c);
            }
        }
        if (!longform && m == 0 && tmp.size > 0 && (tmp.buf[tmp.size - 1] == ':' || tmp.buf[tmp.size - 1] == '.'))
            tmp.size--;
    }
    dbuf_putc(&tmp, '\0');
    for (const char *q = gmt; *q; q++) {
        if (q[0] == '{' && q[1] == '0' && q[2] == '}') {
            dbuf_putstr(out, (const char *)tmp.buf);
            q += 2;
        } else {
            dbuf_putc(out, *q);
        }
    }
    dbuf_free(&tmp);
}

static void format_zone_name(JSContext *ctx, const IntlDateTimeFormat *dtf, int style, double tv, int offset,
                             bool dst, DynBuf *out)
{
    const char *name = NULL;
    if (style == 2 || style == 3) {
        format_gmt(dtf, offset, style == 3, out);
        return;
    }
    if (dtf->tz_kind == TZ_UTC) {
        name = dget(dtf, "zz.Etc/UTC.%cs", (style == 1 || style == 5) ? 'l' : 's');
        if (name) {
            dbuf_putstr(out, name);
            return;
        }
        format_gmt(dtf, 0, false, out);
        return;
    }
    if (dtf->tz_kind == TZ_NAMED) {
        const char *cldr = intl_time_zone_cldr_id(dtf->tz_primary);
        char lc = (style == 1 || style == 5) ? 'l' : 's';
        char kc = (style >= 4) ? 'g' : (dst ? 'd' : 's');
        if (cldr) {
            const char *mz;
            name = dget(dtf, "zz.%s.%c%c", cldr, lc, kc);
            if (!name) {
                mz = intl_metazone_for(cldr, (int64_t)floor(tv / 60000.0));
                if (mz) {
                    name = dget(dtf, "zm.%s.%c%c", mz, lc, kc);
                    if (!name && kc == 'g')
                        name = dget(dtf, "zm.%s.%cs", mz, lc);
                }
            }
        }
    }
    if (name) {
        dbuf_putstr(out, name);
        return;
    }
    format_gmt(dtf, offset, style == 1 || style == 5, out);
}

typedef struct LocalTime {
    IntlCalendarDate cal;
    int hour, minute, second, ms;
    int offset;
    bool dst;
    double tv;
} LocalTime;

static void to_local_time(JSContext *ctx, const IntlDateTimeFormat *dtf, double tv, LocalTime *lt)
{
    double local;
    int64_t days;
    int64_t ms;
    lt->tv = tv;
    lt->offset = zone_offset_minutes(ctx, dtf, tv, &lt->dst);
    local = tv + lt->offset * 60000.0;
    days = (int64_t)floor(local / MS_PER_DAY);
    ms = (int64_t)(local - (double)days * MS_PER_DAY);
    intl_calendar_compute(dtf->calendar, days + RD_EPOCH_1970, &lt->cal);
    lt->hour = (int)(ms / 3600000);
    lt->minute = (int)(ms / 60000 % 60);
    lt->second = (int)(ms / 1000 % 60);
    lt->ms = (int)(ms % 1000);
}

static const char *day_period_key(const IntlDateTimeFormat *dtf, int hour, int minute, bool flexible)
{
    static const char *const periods[] = {
        "morning1", "morning2", "afternoon1", "afternoon2", "evening1", "evening2", "night1", "night2",
    };
    int t = hour * 60 + minute;
    if (!flexible)
        return hour < 12 ? "am" : "pm";
    if (hour == 12 && minute == 0 && dget(dtf, "dpr.noon") && dget(dtf, "gPfw.noon"))
        return "noon";
    for (size_t i = 0; i < countof(periods); i++) {
        const char *r = dget(dtf, "dpr.%s", periods[i]);
        int fh, fm, bh, bm, from, before;
        if (!r || r[0] == '@')
            continue;
        if (sscanf(r, "%d:%d-%d:%d", &fh, &fm, &bh, &bm) != 4)
            continue;
        from = fh * 60 + fm;
        before = bh * 60 + bm;
        if (from <= before ? (t >= from && t < before) : (t >= from || t < before))
            return periods[i];
    }
    return hour < 12 ? "am" : "pm";
}

static const char *period_name(const IntlDateTimeFormat *dtf, const char *key, int count, bool standalone)
{
    char w = count <= 3 ? 'a' : count == 4 ? 'w' : 'n';
    const char *s = dget(dtf, "gP%c%c.%s", standalone ? 's' : 'f', w, key);
    if (!s)
        s = dget(dtf, "gPf%c.%s", w, key);
    if (!s)
        s = dget(dtf, "gPfa.%s", key);
    return s;
}

static void format_pattern(JSContext *ctx, const IntlDateTimeFormat *dtf, const char *pattern, const LocalTime *lt,
                           IntlParts *parts)
{
    DynBuf lit;
    const char *p = pattern;
    char code = calendar_codes[dtf->calendar];
    dbuf_init(&lit);
    while (*p) {
        char c;
        int n;
        p = pattern_next(p, &c, &n, &lit);
        if (lit.size) {
            intl_parts_add(parts, "literal", (const char *)lit.buf, lit.size, 0);
            lit.size = 0;
        }
        if (!c)
            continue;
        switch (c) {
        case 'G': {
            char w = n <= 3 ? 'a' : n == 4 ? 'w' : 'n';
            const char *s;
            if (lt->cal.era_gregorian)
                s = dget(dtf, "gE%c.%d", w, lt->cal.era);
            else
                s = dget(dtf, "%cE%c.%d", code, w, lt->cal.era);
            if (!s && !lt->cal.era_gregorian)
                s = dget(dtf, "%cEa.%d", code, lt->cal.era);
            if (!s)
                s = dget(dtf, "gE%c.%d", w, lt->cal.era > 1 ? 1 : lt->cal.era);
            add_text_part(parts, "era", s ? s : "");
            break;
        }
        case 'y': {
            int64_t y = lt->cal.era_year;
            if (dtf->calendar == INTL_CAL_CHINESE || dtf->calendar == INTL_CAL_DANGI)
                y = lt->cal.cyclic_year;
            if (n == 2)
                add_number_part(dtf, parts, "year", ((y % 100) + 100) % 100, 2);
            else
                add_number_part(dtf, parts, "year", y, n > 2 ? n : 1);
            break;
        }
        case 'r':
            add_number_part(dtf, parts, "relatedYear", lt->cal.related_year, 1);
            break;
        case 'U': {
            char w = n <= 3 ? 'a' : n == 4 ? 'w' : 'n';
            const char *s = dget(dtf, "%cY%c.%d", code, w, lt->cal.cyclic_year);
            if (!s)
                s = dget(dtf, "%cYa.%d", code, lt->cal.cyclic_year);
            if (s) {
                add_text_part(parts, "yearName", s);
            } else {
                add_number_part(dtf, parts, "yearName", lt->cal.cyclic_year, 1);
            }
            break;
        }
        case 'M':
        case 'L': {
            bool sa = c == 'L';
            bool leap = lt->cal.leap_month;
            if (n <= 2) {
                DynBuf b;
                char num[16];
                const char *mp = leap ? dget(dtf, "%cmpna", code) : NULL;
                snprintf(num, sizeof(num), n == 2 ? "%02d" : "%d", lt->cal.month);
                dbuf_init(&b);
                if (mp) {
                    for (const char *q = mp; *q; q++) {
                        if (q[0] == '{' && q[1] == '0' && q[2] == '}') {
                            put_digits(dtf, &b, num);
                            q += 2;
                        } else {
                            dbuf_putc(&b, *q);
                        }
                    }
                } else {
                    put_digits(dtf, &b, num);
                }
                intl_parts_add(parts, "month", (const char *)b.buf, b.size, 0);
                dbuf_free(&b);
            } else {
                char w = n == 3 ? 'a' : n == 4 ? 'w' : 'n';
                char key[16];
                const char *s;
                if (lt->cal.leap_key)
                    snprintf(key, sizeof(key), "%dL", lt->cal.month_key);
                else
                    snprintf(key, sizeof(key), "%d", lt->cal.month_key);
                s = dget(dtf, "%cM%c%c.%s", code, sa ? 's' : 'f', w, key);
                if (!s)
                    s = dget(dtf, "%cM%c%c.%s", code, sa ? 'f' : 's', w, key);
                if (!s)
                    s = dget(dtf, "%cMf%c.%s", code, 'a', key);
                if (leap) {
                    const char *mp = dget(dtf, "%cmp%c%c", code, sa ? 's' : 'f', w);
                    if (!mp)
                        mp = dget(dtf, "%cmpf%c", code, w);
                    if (mp) {
                        DynBuf b;
                        dbuf_init(&b);
                        for (const char *q = mp; *q; q++) {
                            if (q[0] == '{' && q[1] == '0' && q[2] == '}') {
                                dbuf_putstr(&b, s ? s : "");
                                q += 2;
                            } else {
                                dbuf_putc(&b, *q);
                            }
                        }
                        intl_parts_add(parts, "month", (const char *)b.buf, b.size, 0);
                        dbuf_free(&b);
                        break;
                    }
                }
                if (s)
                    add_text_part(parts, "month", s);
                else
                    add_number_part(dtf, parts, "month", lt->cal.month, 1);
            }
            break;
        }
        case 'd':
            add_number_part(dtf, parts, "day", lt->cal.day, n);
            break;
        case 'E':
        case 'c':
        case 'e': {
            char w = n <= 3 ? 'a' : n == 4 ? 'w' : n == 5 ? 'n' : 's';
            const char *s = dget(dtf, "gD%c%c.%d", c == 'c' ? 's' : 'f', w, lt->cal.weekday);
            if (!s)
                s = dget(dtf, "gDf%c.%d", w, lt->cal.weekday);
            if (!s)
                s = dget(dtf, "gDfa.%d", lt->cal.weekday);
            add_text_part(parts, "weekday", s ? s : "");
            break;
        }
        case 'a':
        case 'b': {
            const char *s = period_name(dtf, lt->hour < 12 ? "am" : "pm", n, false);
            add_text_part(parts, "dayPeriod", s ? s : (lt->hour < 12 ? "AM" : "PM"));
            break;
        }
        case 'B': {
            const char *key = day_period_key(dtf, lt->hour, lt->minute, true);
            const char *s = period_name(dtf, key, n, false);
            if (!s)
                s = period_name(dtf, lt->hour < 12 ? "am" : "pm", n, false);
            add_text_part(parts, "dayPeriod", s ? s : "");
            break;
        }
        case 'h':
            add_number_part(dtf, parts, "hour", lt->hour % 12 == 0 ? 12 : lt->hour % 12, n);
            break;
        case 'H':
            add_number_part(dtf, parts, "hour", lt->hour, n);
            break;
        case 'K':
            add_number_part(dtf, parts, "hour", lt->hour % 12, n);
            break;
        case 'k':
            add_number_part(dtf, parts, "hour", lt->hour == 0 ? 24 : lt->hour, n);
            break;
        case 'm':
            add_number_part(dtf, parts, "minute", lt->minute, n);
            break;
        case 's':
            add_number_part(dtf, parts, "second", lt->second, n);
            break;
        case 'S': {
            int digits = n > 3 ? 3 : n;
            int v = lt->ms;
            for (int i = digits; i < 3; i++)
                v /= 10;
            add_number_part(dtf, parts, "fractionalSecond", v, digits);
            break;
        }
        case 'z':
        case 'v':
        case 'O':
        case 'V':
        case 'Z': {
            int style = c == 'z' ? (n == 4 ? 1 : 0) : c == 'O' ? (n == 4 ? 3 : 2) : c == 'v' ? (n == 4 ? 5 : 4) : 3;
            DynBuf b;
            dbuf_init(&b);
            format_zone_name(ctx, dtf, style, lt->tv, lt->offset, lt->dst, &b);
            intl_parts_add(parts, "timeZoneName", (const char *)b.buf, b.size, 0);
            dbuf_free(&b);
            break;
        }
        default:
            break;
        }
    }
    dbuf_free(&lit);
}

static const char *const dtf_keys[] = { "ca", "hc", "nu" };

static int default_calendar(const char *locale)
{
    IntlLangId id;
    char region[8] = "";
    if (intl_parse_lang_id(locale, intl_base_name_len(locale), &id)) {
        if (!id.region[0])
            intl_add_likely_subtags(&id);
        snprintf(region, sizeof(region), "%s", id.region);
    }
    return intl_region_default_calendar(region[0] ? region : "001");
}

static bool dtf_key_supported(const char *locale, const char *key, const char *value)
{
    if (!strcmp(key, "ca"))
        return calendar_index(value) >= 0;
    if (!strcmp(key, "hc"))
        return !strcmp(value, "h11") || !strcmp(value, "h12") || !strcmp(value, "h23") || !strcmp(value, "h24");
    return intl_numbering_system_digits(value) != NULL;
}

static const char *dtf_key_default(const char *locale, const char *key)
{
    if (!strcmp(key, "ca"))
        return calendar_names[default_calendar(locale)];
    if (!strcmp(key, "hc"))
        return NULL;
    return intl_default_numbering_system(locale);
}

static const IntlResolverOps dtf_resolver = { dtf_keys, 3, dtf_key_supported, dtf_key_default };

static int locale_hour_cycles(const IntlDateTimeFormat *dtf, int *hc_default, int *hc12, int *hc24)
{
    const char *tf = cal_pattern(dtf, "tf.short");
    IntlLangId id;
    char region[8] = "001";
    *hc_default = 2;
    if (tf) {
        for (const char *q = tf; *q; q++) {
            if (*q == '\'') {
                q++;
                while (*q && *q != '\'')
                    q++;
                if (!*q)
                    break;
                continue;
            }
            if (*q == 'h') {
                *hc_default = 1;
                break;
            }
            if (*q == 'H') {
                *hc_default = 2;
                break;
            }
            if (*q == 'K') {
                *hc_default = 0;
                break;
            }
            if (*q == 'k') {
                *hc_default = 3;
                break;
            }
        }
    }
    if (intl_parse_lang_id(dtf->data_locale, intl_base_name_len(dtf->data_locale), &id)) {
        if (!id.region[0])
            intl_add_likely_subtags(&id);
        if (id.region[0])
            snprintf(region, sizeof(region), "%s", id.region);
    }
    intl_region_hour_cycle_prefs(dtf->data_locale, region, hc12, hc24);
    if (*hc_default == 0 || *hc_default == 1)
        *hc12 = *hc_default;
    return 0;
}

static void skel_from_values(const IntlDateTimeFormat *dtf, const int *values, int hc, Skel *preq);

static int dtf_init(JSContext *ctx, IntlDateTimeFormat *dtf, JSValueConst locales, JSValueConst options_in,
                    int required, int defaults, JSValueConst zone_override)
{
    IntlLocaleList requested;
    JSValue options = JS_UNDEFINED, tzv = JS_UNDEFINED;
    IntlResolved res;
    bool lookup, has_ca, has_nu, has_hc, explicit = false;
    char ca[64], nu[64], hcbuf[8];
    char *opts[3];
    int hour12, hc, hc_default, hc12, hc24, r = -1, values[F_COUNT], date_style, time_style;
    Skel req;

    memset(dtf, 0, sizeof(*dtf));
    dtf->bound_format = JS_UNDEFINED;
    dtf->tzdb = -1;
    dtf->tz_win = -1;
    dtf->hour_cycle = -1;
    dtf->date_style = dtf->time_style = -1;
    if (intl_canonicalize_locale_list(ctx, locales, &requested) < 0)
        return -1;
    options = intl_coerce_options(ctx, options_in);
    if (JS_IsException(options))
        goto done;
    if (intl_get_locale_matcher(ctx, options, &lookup) < 0)
        goto done;
    if (intl_get_resolution_option(ctx, options, "calendar", NULL, ca, sizeof(ca), &has_ca) < 0)
        goto done;
    if (intl_get_resolution_option(ctx, options, "numberingSystem", NULL, nu, sizeof(nu), &has_nu) < 0)
        goto done;
    hour12 = intl_get_bool_option(ctx, options, "hour12", -1);
    if (hour12 < -1)
        goto done;
    if (intl_get_string_option(ctx, options, "hourCycle", hc_names, NULL, hcbuf, sizeof(hcbuf), &has_hc) < 0)
        goto done;
    opts[0] = has_ca ? ca : NULL;
    opts[1] = hour12 >= 0 ? (char *)intl_option_null : has_hc ? hcbuf : NULL;
    opts[2] = has_nu ? nu : NULL;
    if (intl_resolve_locale(ctx, INTL_CLASS_DATE_TIME_FORMAT, &requested, lookup, &dtf_resolver, opts, &res) < 0)
        goto done;
    memcpy(dtf->locale, res.locale, sizeof(dtf->locale));
    memcpy(dtf->data_locale, res.data_locale, sizeof(dtf->data_locale));
    dtf->data = intl_data_node(res.data_locale);
    dtf->calendar = calendar_index(res.values[0]);
    if (dtf->calendar < 0)
        dtf->calendar = INTL_CAL_GREGORY;
    snprintf(dtf->numbering, sizeof(dtf->numbering), "%.15s", res.values[2]);
    dtf->digits = intl_numbering_system_digits(dtf->numbering);
    {
        const char *dec = intl_data_getf(dtf->data, "n.%s.decimal", dtf->numbering);
        if (!dec)
            dec = intl_data_get(dtf->data, "n.latn.decimal");
        snprintf(dtf->decimal, sizeof(dtf->decimal), "%s", dec ? dec : ".");
    }
    locale_hour_cycles(dtf, &hc_default, &hc12, &hc24);
    hc = -1;
    for (int i = 0; i < 4; i++)
        if (!strcmp(res.values[1], hc_names[i]))
            hc = i;
    if (hour12 == 1)
        hc = hc12;
    else if (hour12 == 0)
        hc = hc24;
    else if (hc < 0)
        hc = hc_default;

    tzv = JS_GetPropertyStr(ctx, options, "timeZone");
    if (JS_IsException(tzv))
        goto done;
    {
        char tzbuf[128];
        const char *id, *primary;
        int minutes;
        if (!JS_IsUndefined(zone_override)) {
            if (!JS_IsUndefined(tzv)) {
                JS_ThrowTypeError(ctx, "timeZone option is not allowed here");
                goto done;
            }
            dtf->zoned_defaults = true;
            JS_FreeValue(ctx, tzv);
            tzv = JS_DupValue(ctx, zone_override);
        }
        if (JS_IsUndefined(tzv)) {
            system_time_zone(ctx, tzbuf, sizeof(tzbuf));
            dtf->tz_host = true;
        } else {
            size_t len;
            const char *s = JS_ToCStringLen(ctx, &len, tzv);
            if (!s)
                goto done;
            snprintf(tzbuf, sizeof(tzbuf), "%s", strlen(s) == len && len < sizeof(tzbuf) ? s : "\x01");
            JS_FreeCString(ctx, s);
        }
        if (intl_parse_offset_time_zone(tzbuf, &minutes) == 0) {
            dtf->tz_kind = TZ_OFFSET;
            dtf->tz_offset = minutes;
            snprintf(dtf->time_zone, sizeof(dtf->time_zone), "%c%02d:%02d", minutes < 0 ? '-' : '+',
                     abs(minutes) / 60, abs(minutes) % 60);
            dtf->tz_host = false;
        } else if (intl_find_time_zone(tzbuf, &id, &primary) >= 0) {
            snprintf(dtf->time_zone, sizeof(dtf->time_zone), "%s", id);
            snprintf(dtf->tz_primary, sizeof(dtf->tz_primary), "%s", primary);
            if (!strcmp(primary, "UTC")) {
                dtf->tz_kind = TZ_UTC;
                dtf->tz_host = false;
            } else if (!strncmp(primary, "Etc/GMT", 7) && (primary[7] == '+' || primary[7] == '-')) {
                dtf->tz_kind = TZ_OFFSET;
                dtf->tz_offset = (primary[7] == '-' ? 60 : -60) * atoi(primary + 8);
                dtf->tz_host = false;
            } else {
                dtf->tz_kind = TZ_NAMED;
                dtf->tzdb = tp_tzdb_lookup(primary, strlen(primary));
                if (dtf->tzdb < 0)
                    dtf->tzdb = tp_tzdb_lookup(id, strlen(id));
#if defined(_WIN32)
                if (dtf->tzdb < 0 && !dtf->tz_host) {
                    const char *cldr = intl_time_zone_cldr_id(primary);
                    dtf->tz_win = win_zone_index(primary);
                    if (dtf->tz_win < 0 && cldr)
                        dtf->tz_win = win_zone_index(cldr);
                }
#endif
            }
        } else {
            JS_ThrowRangeError(ctx, "invalid time zone: %s", tzbuf);
            goto done;
        }
    }
    for (int f = 0; f < F_COUNT; f++) {
        if (f == F_FSD) {
            if (intl_get_number_option(ctx, options, "fractionalSecondDigits", 1, 3, -1, &values[f]) < 0)
                goto done;
            if (values[f] >= 0)
                explicit = true;
            continue;
        }
        values[f] = intl_get_enum_option(ctx, options, field_names[f], field_values[f], -1);
        if (values[f] < -1)
            goto done;
        if (values[f] >= 0)
            explicit = true;
    }
    if (intl_get_enum_option(ctx, options, "formatMatcher", matcher_names, 1) < -1)
        goto done;
    date_style = intl_get_enum_option(ctx, options, "dateStyle", style_names, -1);
    if (date_style < -1)
        goto done;
    time_style = intl_get_enum_option(ctx, options, "timeStyle", style_names, -1);
    if (time_style < -1)
        goto done;
    dtf->date_style = date_style;
    dtf->time_style = time_style;
    dtf->raw_hc = hc;
    if (date_style >= 0 || time_style >= 0) {
        if (explicit) {
            JS_ThrowTypeError(ctx, "dateStyle and timeStyle cannot be combined with explicit components");
            goto done;
        }
        if (required == REQ_DATE && time_style >= 0) {
            JS_ThrowTypeError(ctx, "timeStyle is not allowed here");
            goto done;
        }
        if (required == REQ_TIME && date_style >= 0) {
            JS_ThrowTypeError(ctx, "dateStyle is not allowed here");
            goto done;
        }
        dtf->hour_cycle = time_style >= 0 ? hc : -1;
        if (build_style_pattern(dtf, hc_default) < 0) {
            JS_ThrowOutOfMemory(ctx);
            goto done;
        }
        fields_from_pattern(dtf);
        if (dtf->fields[F_HOUR] < 0)
            dtf->hour_cycle = -1;
        r = 0;
        goto done;
    }
    for (int f = 0; f < F_COUNT; f++)
        dtf->raw[f] = (int8_t)values[f];
    dtf->raw_hc = hc;
    {
        bool need_defaults = true;
        if (required == REQ_DATE || required == REQ_ANY) {
            if (values[F_WEEKDAY] >= 0 || values[F_YEAR] >= 0 || values[F_MONTH] >= 0 || values[F_DAY] >= 0)
                need_defaults = false;
        }
        if (required == REQ_TIME || required == REQ_ANY) {
            if (values[F_DAYPERIOD] >= 0 || values[F_HOUR] >= 0 || values[F_MINUTE] >= 0 ||
                values[F_SECOND] >= 0 || values[F_FSD] >= 0)
                need_defaults = false;
        }
        if (need_defaults) {
            if (defaults == DEF_DATE || defaults == DEF_ALL)
                values[F_YEAR] = values[F_MONTH] = values[F_DAY] = 1;
            if (defaults == DEF_TIME || defaults == DEF_ALL)
                values[F_HOUR] = values[F_MINUTE] = values[F_SECOND] = 1;
        }
    }
    skel_from_values(dtf, values, hc, &req);
    if (build_pattern(dtf, &req) < 0) {
        JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    fields_from_pattern(dtf);
    if (values[F_FSD] >= 0)
        dtf->fields[F_FSD] = (int8_t)values[F_FSD];
    dtf->hour_cycle = dtf->fields[F_HOUR] >= 0 ? hc : -1;
    r = 0;
done:
    intl_locale_list_free(ctx, &requested);
    JS_FreeValue(ctx, options);
    JS_FreeValue(ctx, tzv);
    return r;
}

static void skel_from_values(const IntlDateTimeFormat *dtf, const int *values, int hc, Skel *preq)
{
    Skel req;
    memset(&req, 0, sizeof(req));
    if (values[F_ERA] >= 0 && dtf->calendar != INTL_CAL_CHINESE && dtf->calendar != INTL_CAL_DANGI) {
        req.ch[K_ERA] = 'G';
        req.n[K_ERA] = values[F_ERA] == 0 ? 5 : values[F_ERA] == 1 ? 1 : 4;
    }
    if (values[F_YEAR] >= 0) {
        req.ch[K_YEAR] = 'y';
        req.n[K_YEAR] = values[F_YEAR] == 0 ? 2 : 1;
    }
    if (values[F_MONTH] >= 0) {
        static const int8_t mn[] = { 2, 1, 5, 3, 4 };
        req.ch[K_MONTH] = 'M';
        req.n[K_MONTH] = mn[values[F_MONTH]];
    }
    if (values[F_DAY] >= 0) {
        req.ch[K_DAY] = 'd';
        req.n[K_DAY] = values[F_DAY] == 0 ? 2 : 1;
    }
    if (values[F_WEEKDAY] >= 0) {
        req.ch[K_WEEKDAY] = 'E';
        req.n[K_WEEKDAY] = values[F_WEEKDAY] == 0 ? 5 : values[F_WEEKDAY] == 1 ? 3 : 4;
    }
    if (values[F_DAYPERIOD] >= 0) {
        req.ch[K_PERIOD] = 'B';
        req.n[K_PERIOD] = values[F_DAYPERIOD] == 0 ? 5 : values[F_DAYPERIOD] == 1 ? 1 : 4;
    }
    if (values[F_HOUR] >= 0) {
        req.ch[K_HOUR] = hour_letter(hc);
        req.n[K_HOUR] = values[F_HOUR] == 0 ? 2 : 1;
    }
    if (values[F_MINUTE] >= 0) {
        req.ch[K_MINUTE] = 'm';
        req.n[K_MINUTE] = values[F_MINUTE] == 0 ? 2 : 1;
    }
    if (values[F_SECOND] >= 0) {
        req.ch[K_SECOND] = 's';
        req.n[K_SECOND] = values[F_SECOND] == 0 ? 2 : 1;
    }
    if (values[F_FSD] >= 0) {
        req.ch[K_FRAC] = 'S';
        req.n[K_FRAC] = (int8_t)values[F_FSD];
    }
    if (values[F_TZNAME] >= 0) {
        static const char zc[] = { 'z', 'z', 'O', 'O', 'v', 'v' };
        req.ch[K_ZONE] = zc[values[F_TZNAME]];
        req.n[K_ZONE] = (values[F_TZNAME] & 1) ? 4 : 1;
    }
    *preq = req;
}

static void dtf_free_fields(IntlDateTimeFormat *dtf)
{
    free(dtf->pattern);
    free(dtf->range_skeleton);
    free(dtf->date_pattern);
    free(dtf->time_pattern);
    free(dtf->time_skeleton);
    for (int i = 0; i < 6; i++) {
        if (dtf->temporal[i]) {
            free(dtf->temporal[i]->pattern);
            free(dtf->temporal[i]->range_skeleton);
            free(dtf->temporal[i]->date_pattern);
            free(dtf->temporal[i]->time_pattern);
            free(dtf->temporal[i]->time_skeleton);
            free(dtf->temporal[i]);
        }
    }
}

static int to_time_value(JSContext *ctx, JSValueConst v, double *out)
{
    if (JS_IsUndefined(v)) {
        *out = (double)(js__gettimeofday_us() / 1000);
        return 0;
    }
    return JS_ToFloat64(ctx, out, v);
}

static int time_clip(double *t)
{
    if (!isfinite(*t) || fabs(*t) > 8.64e15)
        return -1;
    *t = trunc(*t) + 0.0;
    return 0;
}

int intl_dtf_partition(JSContext *ctx, IntlDateTimeFormat *dtf, double tv, IntlParts *parts)
{
    LocalTime lt;
    if (time_clip(&tv) < 0) {
        JS_ThrowRangeError(ctx, "Invalid time value");
        return -1;
    }
    to_local_time(ctx, dtf, tv, &lt);
    format_pattern(ctx, dtf, dtf->pattern, &lt, parts);
    if (parts->error) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

enum { R_DEFAULT, R_ERA, R_YEAR, R_MONTH, R_DAY, R_AMPM, R_DAYPERIOD, R_HOUR, R_MINUTE, R_SECOND, R_FSD, R_COUNT };

static const char range_letters[R_COUNT] = { 0, 'G', 'y', 'M', 'd', 'a', 'B', 'h', 'm', 's', 'S' };

static const char *interval_pattern(const IntlDateTimeFormat *dtf, const char *skeleton, int field, int hc)
{
    char letter = range_letters[field];
    char pc = pattern_code(dtf->calendar);
    const char *p;
    if (!skeleton || !skeleton[0] || !letter)
        return NULL;
    if (field == R_HOUR && (hc == 2 || hc == 3))
        letter = 'H';
    p = dget(dtf, "%cif.%s.%c", pc, skeleton, letter);
    if (!p && pc != 'g')
        p = dget(dtf, "gif.%s.%c", skeleton, letter);
    if (!p && field == R_HOUR)
        p = dget(dtf, "%cif.%s.%c", pc, skeleton, letter == 'H' ? 'h' : 'H');
    return p;
}

static void range_field_values(JSContext *ctx, const IntlDateTimeFormat *dtf, int field, const LocalTime *a,
                               const LocalTime *b, int64_t *v1, int64_t *v2)
{
    switch (field) {
    case R_ERA:
        *v1 = a->cal.era * 2 + a->cal.era_gregorian;
        *v2 = b->cal.era * 2 + b->cal.era_gregorian;
        break;
    case R_YEAR:
        *v1 = a->cal.year;
        *v2 = b->cal.year;
        if (dtf->calendar == INTL_CAL_CHINESE || dtf->calendar == INTL_CAL_DANGI) {
            *v1 = a->cal.related_year;
            *v2 = b->cal.related_year;
        }
        break;
    case R_MONTH:
        *v1 = a->cal.month * 2 + a->cal.leap_month;
        *v2 = b->cal.month * 2 + b->cal.leap_month;
        break;
    case R_DAY:
        *v1 = a->cal.day;
        *v2 = b->cal.day;
        break;
    case R_AMPM:
        *v1 = a->hour < 12;
        *v2 = b->hour < 12;
        break;
    case R_DAYPERIOD: {
        const char *k1 = day_period_key(dtf, a->hour, a->minute, true);
        const char *k2 = day_period_key(dtf, b->hour, b->minute, true);
        *v1 = 0;
        *v2 = strcmp(k1, k2) != 0;
        break;
    }
    case R_HOUR:
        *v1 = a->hour;
        *v2 = b->hour;
        break;
    case R_MINUTE:
        *v1 = a->minute;
        *v2 = b->minute;
        break;
    case R_SECOND:
        *v1 = a->second;
        *v2 = b->second;
        break;
    default: {
        int digits = dtf->fields[F_FSD] > 0 ? dtf->fields[F_FSD] : 3;
        int d1 = a->ms, d2 = b->ms;
        for (int i = digits; i < 3; i++) {
            d1 /= 10;
            d2 /= 10;
        }
        *v1 = d1;
        *v2 = d2;
        break;
    }
    }
}

static void add_parts_with_source(IntlParts *out, IntlParts *src, const char *source)
{
    for (int i = 0; i < src->count; i++) {
        intl_parts_add(out, src->items[i].type, src->items[i].value, strlen(src->items[i].value), 0);
        if (out->count > 0)
            out->items[out->count - 1].source = source;
    }
    intl_parts_free(src);
}

static void format_with_source(JSContext *ctx, const IntlDateTimeFormat *dtf, const char *pattern,
                               const LocalTime *lt, IntlParts *out, const char *source)
{
    IntlParts tmp;
    intl_parts_init(&tmp);
    format_pattern(ctx, dtf, pattern, lt, &tmp);
    if (tmp.error)
        out->error = true;
    add_parts_with_source(out, &tmp, source);
}

static void adjust_interval(const IntlDateTimeFormat *dtf, const char *ipat, DynBuf *out)
{
    Skel req;
    skel_from_pattern(dtf->pattern, &req);
    if (dtf->fields[F_FSD] > 0) {
        req.ch[K_FRAC] = 'S';
        req.n[K_FRAC] = dtf->fields[F_FSD];
    }
    adjust_pattern(dtf, ipat, &req, out);
}

static void emit_interval(JSContext *ctx, const IntlDateTimeFormat *dtf, const char *ipat, const LocalTime *a,
                          const LocalTime *b, IntlParts *out)
{
    DynBuf adj, lit;
    char *pat;
    const char *p, *split = NULL;
    char seen[64];
    int nseen = 0;
    dbuf_init(&adj);
    adjust_interval(dtf, ipat, &adj);
    dbuf_putc(&adj, '\0');
    if (adj.error) {
        out->error = true;
        dbuf_free(&adj);
        return;
    }
    pat = (char *)adj.buf;
    dbuf_init(&lit);
    p = pat;
    while (*p) {
        char c;
        int n;
        const char *start = p;
        p = pattern_next(p, &c, &n, &lit);
        lit.size = 0;
        if (!c)
            continue;
        {
            int k = kind_of(c);
            bool rep = false;
            for (int i = 0; i < nseen; i++)
                if (kind_of(seen[i]) == k)
                    rep = true;
            if (rep) {
                split = start;
                break;
            }
            if (nseen < (int)sizeof(seen))
                seen[nseen++] = c;
        }
    }
    if (!split) {
        format_with_source(ctx, dtf, pat, a, out, "shared");
        dbuf_free(&lit);
        dbuf_free(&adj);
        return;
    }
    {
        char *first = dup_n(pat, (size_t)(split - pat));
        const char *second = split;
        const char *last_field_end = first, *first_field_start = NULL;
        const char *q = first;
        bool second_kinds[K_COUNT];
        memset(second_kinds, 0, sizeof(second_kinds));
        for (const char *r2 = second; *r2;) {
            char c;
            int n;
            r2 = pattern_next(r2, &c, &n, &lit);
            lit.size = 0;
            if (c && kind_of(c) >= 0)
                second_kinds[kind_of(c)] = true;
        }
        while (*q) {
            char c;
            int n;
            const char *start = q;
            q = pattern_next(q, &c, &n, &lit);
            lit.size = 0;
            if (c) {
                if (!first_field_start && kind_of(c) >= 0 && second_kinds[kind_of(c)])
                    first_field_start = start;
                last_field_end = q;
            }
        }
        {
            char *lead = dup_n(first, first_field_start ? (size_t)(first_field_start - first) : 0);
            char *startp = dup_n(first_field_start ? first_field_start : first,
                                   (size_t)(last_field_end - (first_field_start ? first_field_start : first)));
            char *sep = strdup(last_field_end);
            const char *end_of_rep = second;
            const char *r = second;
            while (*r) {
                char c;
                int n;
                r = pattern_next(r, &c, &n, &lit);
                lit.size = 0;
                if (c) {
                    bool rep = false;
                    for (int i = 0; i < nseen; i++)
                        if (kind_of(seen[i]) == kind_of(c))
                            rep = true;
                    if (rep)
                        end_of_rep = r;
                }
            }
            {
                char *endp = dup_n(second, (size_t)(end_of_rep - second));
                if (lead && *lead)
                    format_with_source(ctx, dtf, lead, a, out, "shared");
                format_with_source(ctx, dtf, startp, a, out, "startRange");
                if (sep && *sep)
                    format_with_source(ctx, dtf, sep, a, out, "shared");
                format_with_source(ctx, dtf, endp, b, out, "endRange");
                if (*end_of_rep)
                    format_with_source(ctx, dtf, end_of_rep, a, out, "shared");
                free(endp);
            }
            free(lead);
            free(startp);
            free(sep);
        }
        free(first);
    }
    dbuf_free(&lit);
    dbuf_free(&adj);
}

static void emit_fallback(JSContext *ctx, const IntlDateTimeFormat *dtf, const char *pattern, const LocalTime *a,
                          const LocalTime *b, IntlParts *out)
{
    const char *fb = cal_pattern(dtf, "if");
    if (!fb)
        fb = "{0} \xe2\x80\x93 {1}";
    {
        const char *z0 = strstr(fb, "{0}"), *z1 = strstr(fb, "{1}");
        const LocalTime *first = a, *second = b;
        const char *s0 = "startRange", *s1 = "endRange";
        if (!z0 || !z1) {
            format_with_source(ctx, dtf, pattern, a, out, "startRange");
            return;
        }
        if (z1 < z0) {
            const char *t = z0;
            z0 = z1;
            z1 = t;
            first = b;
            second = a;
            s0 = "endRange";
            s1 = "startRange";
        }
        if (z0 > fb) {
            char *lit = dup_n(fb, (size_t)(z0 - fb));
            intl_parts_add(out, "literal", lit, strlen(lit), 0);
            out->items[out->count - 1].source = "shared";
            free(lit);
        }
        format_with_source(ctx, dtf, pattern, first, out, s0);
        if (z1 > z0 + 3) {
            intl_parts_add(out, "literal", z0 + 3, (size_t)(z1 - z0 - 3), 0);
            out->items[out->count - 1].source = "shared";
        }
        format_with_source(ctx, dtf, pattern, second, out, s1);
        if (z1[3]) {
            intl_parts_add(out, "literal", z1 + 3, strlen(z1 + 3), 0);
            out->items[out->count - 1].source = "shared";
        }
    }
}

static void pattern_letters(const char *pat, bool *seen)
{
    DynBuf lit;
    const char *p = pat;
    memset(seen, 0, 128);
    dbuf_init(&lit);
    while (*p) {
        char c;
        int n;
        p = pattern_next(p, &c, &n, &lit);
        lit.size = 0;
        if (c > 0)
            seen[(int)c] = true;
    }
    dbuf_free(&lit);
}

static void key_for_pattern(const IntlDateTimeFormat *dtf, const char *pat, char *key, size_t size)
{
    Skel sk;
    MatchCtx mc;
    skel_from_pattern(pat, &sk);
    sk.ch[K_FRAC] = 0;
    sk.n[K_FRAC] = 0;
    match_skeleton(dtf, &sk, &mc, true);
    snprintf(key, size, "%s", mc.best_key);
}

static int dtf_partition_range_raw(JSContext *ctx, IntlDateTimeFormat *dtf, double x, double y, IntlParts *out,
                                   bool clip)
{
    LocalTime a, b;
    int selected = -1;
    bool equal = true, check_more = true;
    bool has_range[R_COUNT];
    bool seen[128];
    bool combined;
    if (clip && (time_clip(&x) < 0 || time_clip(&y) < 0)) {
        JS_ThrowRangeError(ctx, "Invalid time value");
        return -1;
    }
    to_local_time(ctx, dtf, x, &a);
    to_local_time(ctx, dtf, y, &b);
    pattern_letters(dtf->pattern, seen);
    memset(has_range, 0, sizeof(has_range));
    has_range[R_ERA] = seen['G'];
    has_range[R_YEAR] = seen['y'] || seen['r'] || seen['U'];
    has_range[R_MONTH] = seen['M'] || seen['L'];
    has_range[R_DAY] = seen['d'] || seen['E'] || seen['c'];
    has_range[R_AMPM] = seen['a'] || seen['b'];
    has_range[R_DAYPERIOD] = seen['B'];
    has_range[R_HOUR] = seen['h'] || seen['H'] || seen['K'] || seen['k'];
    has_range[R_MINUTE] = seen['m'];
    has_range[R_SECOND] = seen['s'];
    has_range[R_FSD] = seen['S'];
    for (int f = R_ERA; f < R_COUNT; f++) {
        int64_t v1, v2;
        if (selected >= 0 && !has_range[f])
            check_more = false;
        if (!(equal && check_more))
            continue;
        selected = has_range[f] ? f : -1;
        range_field_values(ctx, dtf, f, &a, &b, &v1, &v2);
        if (v1 != v2)
            equal = false;
    }
    if (equal) {
        format_with_source(ctx, dtf, dtf->pattern, &a, out, "shared");
        goto end;
    }
    if (selected < 0) {
        emit_fallback(ctx, dtf, dtf->pattern, &a, &b, out);
        goto end;
    }
    combined = dtf->date_pattern && dtf->time_pattern;
    if (!combined) {
        char key[64];
        const char *ip;
        if (dtf->date_style >= 0 || dtf->time_style >= 0 || !dtf->range_skeleton)
            key_for_pattern(dtf, dtf->pattern, key, sizeof(key));
        else
            snprintf(key, sizeof(key), "%s", dtf->range_skeleton);
        ip = interval_pattern(dtf, key, selected, dtf->hour_cycle);
        if (ip)
            emit_interval(ctx, dtf, ip, &a, &b, out);
        else
            emit_fallback(ctx, dtf, dtf->pattern, &a, &b, out);
        goto end;
    }
    if (selected <= R_DAY) {
        emit_fallback(ctx, dtf, dtf->pattern, &a, &b, out);
        goto end;
    }
    {
        char key[64];
        const char *ip;
        if (dtf->time_skeleton && dtf->date_style < 0 && dtf->time_style < 0)
            snprintf(key, sizeof(key), "%s", dtf->time_skeleton);
        else
            key_for_pattern(dtf, dtf->time_pattern, key, sizeof(key));
        ip = interval_pattern(dtf, key, selected, dtf->hour_cycle);
        {
            int style = dtf->date_style >= 0 ? dtf->date_style : 3;
            char ckey[16];
            const char *conn;
            snprintf(ckey, sizeof(ckey), "dta.%s", style_names[style]);
            conn = cal_pattern(dtf, ckey);
            if (!conn) {
                snprintf(ckey, sizeof(ckey), "dt.%s", style_names[style]);
                conn = cal_pattern(dtf, ckey);
            }
            if (!conn)
                conn = "{1} {0}";
            for (const char *q = conn; *q;) {
                if (q[0] == '{' && q[1] == '0' && q[2] == '}') {
                    if (ip)
                        emit_interval(ctx, dtf, ip, &a, &b, out);
                    else
                        emit_fallback(ctx, dtf, dtf->time_pattern, &a, &b, out);
                    q += 3;
                } else if (q[0] == '{' && q[1] == '1' && q[2] == '}') {
                    format_with_source(ctx, dtf, dtf->date_pattern, &a, out, "shared");
                    q += 3;
                } else {
                    const char *e = strchr(q, '{');
                    size_t len = e ? (size_t)(e - q) : strlen(q);
                    DynBuf tmp;
                    dbuf_init(&tmp);
                    dbuf_put(&tmp, (const uint8_t *)q, len);
                    dbuf_putc(&tmp, '\0');
                    format_with_source(ctx, dtf, (const char *)tmp.buf, &a, out, "shared");
                    dbuf_free(&tmp);
                    q += len;
                }
            }
        }
    }
end:
    if (out->error) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    return 0;
}

enum { TK_DATE, TK_YEAR_MONTH, TK_MONTH_DAY, TK_TIME, TK_DATETIME, TK_INSTANT };

static IntlDateTimeFormat *temporal_format(IntlDateTimeFormat *dtf, int tk)
{
    IntlDateTimeFormat *t;
    if (dtf->temporal_done[tk])
        return dtf->temporal[tk];
    dtf->temporal_done[tk] = true;
    t = malloc(sizeof(*t));
    if (!t)
        return NULL;
    *t = *dtf;
    t->pattern = t->range_skeleton = t->date_pattern = t->time_pattern = t->time_skeleton = NULL;
    memset(t->temporal, 0, sizeof(t->temporal));
    memset(t->temporal_done, 0, sizeof(t->temporal_done));
    t->bound_format = JS_UNDEFINED;
    if (dtf->date_style >= 0 || dtf->time_style >= 0) {
        static const uint16_t allowed[6] = {
            (1 << F_WEEKDAY) | (1 << F_ERA) | (1 << F_YEAR) | (1 << F_MONTH) | (1 << F_DAY),
            (1 << F_ERA) | (1 << F_YEAR) | (1 << F_MONTH),
            (1 << F_MONTH) | (1 << F_DAY),
            (1 << F_DAYPERIOD) | (1 << F_HOUR) | (1 << F_MINUTE) | (1 << F_SECOND) | (1 << F_FSD),
            0x3ff & ~(1 << F_TZNAME),
            0x7ff,
        };
        bool conflict = false;
        int values[F_COUNT];
        Skel req;
        if ((tk <= TK_MONTH_DAY && dtf->date_style < 0) || (tk == TK_TIME && dtf->time_style < 0)) {
            free(t);
            return NULL;
        }
        for (int f = 0; f < F_COUNT; f++) {
            values[f] = -1;
            if (dtf->fields[f] >= 0) {
                if (allowed[tk] & (1 << f))
                    values[f] = dtf->fields[f];
                else
                    conflict = true;
            }
        }
        if (!conflict || tk == TK_INSTANT) {
            t->pattern = strdup(dtf->pattern);
            t->date_pattern = dtf->date_pattern ? strdup(dtf->date_pattern) : NULL;
            t->time_pattern = dtf->time_pattern ? strdup(dtf->time_pattern) : NULL;
        } else {
            t->date_style = t->time_style = -1;
            skel_from_values(dtf, values, dtf->raw_hc, &req);
            build_pattern(t, &req);
        }
        if (!t->pattern) {
            free(t);
            return NULL;
        }
        fields_from_pattern(t);
        if (tk != TK_INSTANT && (t->date_style >= 0 || t->time_style >= 0) && conflict) {
            t->date_style = t->time_style = -1;
        }
        t->hour_cycle = t->fields[F_HOUR] >= 0 ? dtf->raw_hc : -1;
        dtf->temporal[tk] = t;
        return t;
    }
    {
        static const uint16_t required[6] = {
            (1 << F_WEEKDAY) | (1 << F_YEAR) | (1 << F_MONTH) | (1 << F_DAY),
            (1 << F_YEAR) | (1 << F_MONTH),
            (1 << F_MONTH) | (1 << F_DAY),
            (1 << F_DAYPERIOD) | (1 << F_HOUR) | (1 << F_MINUTE) | (1 << F_SECOND) | (1 << F_FSD),
            0x3ff & ~(1 << F_ERA),
            0x3ff & ~(1 << F_ERA),
        };
        static const uint16_t defaults[6] = {
            (1 << F_YEAR) | (1 << F_MONTH) | (1 << F_DAY),
            (1 << F_YEAR) | (1 << F_MONTH),
            (1 << F_MONTH) | (1 << F_DAY),
            (1 << F_HOUR) | (1 << F_MINUTE) | (1 << F_SECOND),
            (1 << F_YEAR) | (1 << F_MONTH) | (1 << F_DAY) | (1 << F_HOUR) | (1 << F_MINUTE) | (1 << F_SECOND),
            (1 << F_YEAR) | (1 << F_MONTH) | (1 << F_DAY) | (1 << F_HOUR) | (1 << F_MINUTE) | (1 << F_SECOND),
        };
        int values[F_COUNT];
        bool any_present = false, need_defaults = true;
        Skel req;
        for (int f = 0; f < F_COUNT; f++)
            values[f] = tk == TK_INSTANT ? dtf->raw[f] : -1;
        if (tk != TK_INSTANT) {
            if (tk == TK_DATE || tk == TK_YEAR_MONTH || tk == TK_DATETIME)
                values[F_ERA] = dtf->raw[F_ERA];
        }
        for (int f = 0; f < F_COUNT; f++)
            if (f != F_ERA && f != F_TZNAME && dtf->raw[f] >= 0)
                any_present = true;
        for (int f = 0; f < F_COUNT; f++) {
            if ((required[tk] & (1 << f)) && dtf->raw[f] >= 0) {
                values[f] = dtf->raw[f];
                need_defaults = false;
            }
        }
        if (need_defaults) {
            if (any_present && tk != TK_INSTANT) {
                free(t);
                return NULL;
            }
            for (int f = 0; f < F_COUNT; f++)
                if (defaults[tk] & (1 << f))
                    values[f] = 1;
            if (tk == TK_INSTANT && dtf->zoned_defaults && values[F_TZNAME] < 0)
                values[F_TZNAME] = 0;
        }
        skel_from_values(dtf, values, dtf->raw_hc, &req);
        if (build_pattern(t, &req) < 0 || !t->pattern) {
            free(t);
            return NULL;
        }
        fields_from_pattern(t);
        if (values[F_FSD] >= 0)
            t->fields[F_FSD] = (int8_t)values[F_FSD];
        t->hour_cycle = t->fields[F_HOUR] >= 0 ? dtf->raw_hc : -1;
        dtf->temporal[tk] = t;
        return t;
    }
}

static int temporal_kind_of(JSValueConst v, TPObject **pobj)
{
    static const int kinds[] = {
        TP_KIND_DATE, TP_KIND_YEAR_MONTH, TP_KIND_MONTH_DAY, TP_KIND_TIME, TP_KIND_DATETIME, TP_KIND_INSTANT,
        TP_KIND_ZONED,
    };
    if (!JS_IsObject(v))
        return -1;
    for (int i = 0; i < 7; i++) {
        TPObject *o = JS_GetOpaque(v, js_temporal_class_id(kinds[i]));
        if (o) {
            *pobj = o;
            return i;
        }
    }
    return -1;
}

static int to_formattable(JSContext *ctx, JSValueConst v, double *num, int *tk, TPObject **obj)
{
    *tk = temporal_kind_of(v, obj);
    if (*tk >= 0)
        return 0;
    return JS_ToFloat64(ctx, num, v);
}

static int handle_temporal(JSContext *ctx, IntlDateTimeFormat *dtf, int tk, TPObject *o, IntlDateTimeFormat **pview,
                           double *ms)
{
    IntlDateTimeFormat *t;
    const char *cal;
    if (tk == 6) {
        JS_ThrowTypeError(ctx, "Temporal.ZonedDateTime cannot be formatted by Intl.DateTimeFormat");
        return -1;
    }
    cal = tp_calendar_id(o->cal);
    if (tk == TK_DATE || tk == TK_DATETIME) {
        if (strcmp(cal, "iso8601") && strcmp(cal, calendar_names[dtf->calendar])) {
            JS_ThrowRangeError(ctx, "calendar %s does not match the formatter's calendar", cal);
            return -1;
        }
    } else if (tk == TK_YEAR_MONTH || tk == TK_MONTH_DAY) {
        if (strcmp(cal, calendar_names[dtf->calendar])) {
            JS_ThrowRangeError(ctx, "calendar %s does not match the formatter's calendar", cal);
            return -1;
        }
    }
    t = temporal_format(dtf, tk);
    if (!t) {
        JS_ThrowTypeError(ctx, "the formatter has no fields to format this Temporal value");
        return -1;
    }
    if (tk == TK_INSTANT) {
        tp_i128 q = tp_floor_div128(o->ns, 1000000);
        *ms = (double)(int64_t)q;
    } else {
        TPDateTime dt = o->dt;
        int64_t days = intl_fixed_from_gregorian(dt.date.year, dt.date.month, dt.date.day) - RD_EPOCH_1970;
        if (tk == TK_DATE || tk == TK_YEAR_MONTH || tk == TK_MONTH_DAY) {
            dt.time.hour = 12;
            dt.time.minute = dt.time.second = dt.time.millisecond = 0;
        }
        if (tk == TK_TIME)
            days = 0;
        *ms = (double)days * MS_PER_DAY + dt.time.hour * 3600000.0 + dt.time.minute * 60000.0 +
              dt.time.second * 1000.0 + dt.time.millisecond;
        t->tz_kind = TZ_OFFSET;
        t->tz_offset = 0;
        t->tz_host = false;
    }
    *pview = t;
    return 0;
}

static int dtf_partition_value(JSContext *ctx, IntlDateTimeFormat *dtf, double x, int tk, TPObject *o, IntlParts *parts)
{
    IntlDateTimeFormat *view = dtf;
    LocalTime lt;
    if (tk >= 0) {
        if (handle_temporal(ctx, dtf, tk, o, &view, &x) < 0)
            return -1;
        to_local_time(ctx, view, x, &lt);
        format_pattern(ctx, view, view->pattern, &lt, parts);
        if (parts->error) {
            JS_ThrowOutOfMemory(ctx);
            return -1;
        }
        return 0;
    }
    return intl_dtf_partition(ctx, dtf, x, parts);
}

static int dtf_partition_range(JSContext *ctx, IntlDateTimeFormat *dtf, double x, int tkx, TPObject *ox,
                               double y, int tky, TPObject *oy, IntlParts *out)
{
    IntlDateTimeFormat *view = dtf;
    if (tkx >= 0 || tky >= 0) {
        if (tkx != tky) {
            JS_ThrowTypeError(ctx, "formatRange arguments must be of the same type");
            return -1;
        }
        if (handle_temporal(ctx, dtf, tkx, ox, &view, &x) < 0 || handle_temporal(ctx, dtf, tky, oy, &view, &y) < 0)
            return -1;
    }
    return dtf_partition_range_raw(ctx, view, x, y, out, tkx < 0);
}

static void dtf_finalizer(JSRuntime *rt, JSValueConst val)
{
    IntlDateTimeFormat *dtf = JS_GetOpaque(val, js_intl_class_id(INTL_CLASS_DATE_TIME_FORMAT));
    if (dtf) {
        JS_FreeValueRT(rt, dtf->bound_format);
        dtf_free_fields(dtf);
        free(dtf);
    }
}

static void dtf_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    IntlDateTimeFormat *dtf = JS_GetOpaque(val, js_intl_class_id(INTL_CLASS_DATE_TIME_FORMAT));
    if (dtf)
        JS_MarkValue(rt, dtf->bound_format, mark_func);
}

static IntlDateTimeFormat *dtf_get(JSValueConst obj)
{
    return JS_GetOpaque(obj, js_intl_class_id(INTL_CLASS_DATE_TIME_FORMAT));
}

static JSValue js_intl_dtf_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    bool construct = js_intl_in_constructor_call(ctx);
    JSValue new_target = construct ? this_val : JS_UNDEFINED;
    JSValue obj, ctor = JS_UNDEFINED;
    IntlDateTimeFormat *dtf;
    if (!construct) {
        ctor = js_intl_get_intrinsic(ctx, INTL_INTRINSIC_DATE_TIME_FORMAT);
        new_target = ctor;
    }
    obj = js_intl_create_from_ctor(ctx, new_target, js_intl_class_id(INTL_CLASS_DATE_TIME_FORMAT));
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, ctor);
        return obj;
    }
    dtf = malloc(sizeof(*dtf));
    if (!dtf) {
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, obj);
        return JS_ThrowOutOfMemory(ctx);
    }
    if (dtf_init(ctx, dtf, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, REQ_ANY,
                 DEF_DATE, JS_UNDEFINED) < 0) {
        dtf_free_fields(dtf);
        free(dtf);
        JS_FreeValue(ctx, ctor);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    js_intl_set_opaque(obj, dtf);
    if (!construct) {
        int inst = js_intl_ordinary_has_instance(ctx, this_val, ctor);
        if (inst < 0) {
            JS_FreeValue(ctx, ctor);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
        if (inst) {
            JSValue sym = js_intl_get_fallback_symbol(ctx);
            JSAtom atom = JS_ValueToAtom(ctx, sym);
            int ok;
            JS_FreeValue(ctx, sym);
            ok = JS_DefinePropertyValue(ctx, this_val, atom, obj, JS_PROP_THROW);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, ctor);
            if (ok < 0)
                return JS_EXCEPTION;
            return JS_DupValue(ctx, this_val);
        }
    }
    JS_FreeValue(ctx, ctor);
    return obj;
}

static IntlDateTimeFormat *unwrap_dtf(JSContext *ctx, JSValueConst this_val, const char *method, bool legacy,
                                      JSValue *holder)
{
    IntlDateTimeFormat *dtf = dtf_get(this_val);
    *holder = JS_UNDEFINED;
    if (dtf)
        return dtf;
    if (legacy && JS_IsObject(this_val)) {
        JSValue ctor = js_intl_get_intrinsic(ctx, INTL_INTRINSIC_DATE_TIME_FORMAT);
        int inst = js_intl_ordinary_has_instance(ctx, this_val, ctor);
        JS_FreeValue(ctx, ctor);
        if (inst < 0)
            return NULL;
        if (inst) {
            JSValue sym = js_intl_get_fallback_symbol(ctx);
            JSAtom atom = JS_ValueToAtom(ctx, sym);
            JSValue v = JS_GetProperty(ctx, this_val, atom);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, sym);
            if (JS_IsException(v))
                return NULL;
            dtf = dtf_get(v);
            if (dtf) {
                *holder = v;
                return dtf;
            }
            JS_FreeValue(ctx, v);
        }
    }
    JS_ThrowTypeError(ctx, "Intl.DateTimeFormat.prototype.%s called on incompatible receiver", method);
    return NULL;
}

static JSValue dtf_format_value(JSContext *ctx, IntlDateTimeFormat *dtf, JSValueConst date, bool to_parts)
{
    double x = 0;
    IntlParts parts;
    JSValue r;
    int tk = -1;
    TPObject *o = NULL;
    if (JS_IsUndefined(date)) {
        if (to_time_value(ctx, date, &x) < 0)
            return JS_EXCEPTION;
    } else if (to_formattable(ctx, date, &x, &tk, &o) < 0) {
        return JS_EXCEPTION;
    }
    intl_parts_init(&parts);
    if (dtf_partition_value(ctx, dtf, x, tk, o, &parts) < 0) {
        intl_parts_free(&parts);
        return JS_EXCEPTION;
    }
    r = to_parts ? intl_parts_to_array(ctx, &parts, NULL, NULL, false) : intl_parts_to_string(ctx, &parts);
    intl_parts_free(&parts);
    return r;
}

static JSValue dtf_bound_format(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic,
                                JSValueConst *func_data)
{
    IntlDateTimeFormat *dtf = dtf_get(func_data[0]);
    if (!dtf)
        return JS_ThrowTypeError(ctx, "invalid DateTimeFormat");
    return dtf_format_value(ctx, dtf, argc > 0 ? argv[0] : JS_UNDEFINED, false);
}

static JSValue js_intl_dtf_get_format(JSContext *ctx, JSValueConst this_val)
{
    JSValue holder;
    IntlDateTimeFormat *dtf = unwrap_dtf(ctx, this_val, "format", true, &holder);
    if (!dtf)
        return JS_EXCEPTION;
    if (JS_IsUndefined(dtf->bound_format)) {
        JSValueConst target = JS_IsUndefined(holder) ? this_val : holder;
        JSValue f = JS_NewCFunctionData2(ctx, dtf_bound_format, "", 1, 0, 1, &target);
        if (JS_IsException(f)) {
            JS_FreeValue(ctx, holder);
            return f;
        }
        dtf->bound_format = f;
    }
    JS_FreeValue(ctx, holder);
    return JS_DupValue(ctx, dtf->bound_format);
}

static JSValue js_intl_dtf_formatToParts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    IntlDateTimeFormat *dtf = dtf_get(this_val);
    if (!dtf)
        return JS_ThrowTypeError(ctx, "Intl.DateTimeFormat.prototype.formatToParts called on incompatible receiver");
    return dtf_format_value(ctx, dtf, argc > 0 ? argv[0] : JS_UNDEFINED, true);
}

static JSValue js_intl_dtf_formatRange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                       int to_parts)
{
    IntlDateTimeFormat *dtf = dtf_get(this_val);
    JSValueConst start = argc > 0 ? argv[0] : JS_UNDEFINED, end = argc > 1 ? argv[1] : JS_UNDEFINED;
    double x = 0, y = 0;
    IntlParts parts;
    JSValue r;
    int tkx, tky;
    TPObject *ox = NULL, *oy = NULL;
    if (!dtf)
        return JS_ThrowTypeError(ctx, "Intl.DateTimeFormat.prototype.formatRange called on incompatible receiver");
    if (JS_IsUndefined(start) || JS_IsUndefined(end))
        return JS_ThrowTypeError(ctx, "formatRange requires start and end dates");
    if (to_formattable(ctx, start, &x, &tkx, &ox) < 0 || to_formattable(ctx, end, &y, &tky, &oy) < 0)
        return JS_EXCEPTION;
    intl_parts_init(&parts);
    if (dtf_partition_range(ctx, dtf, x, tkx, ox, y, tky, oy, &parts) < 0) {
        intl_parts_free(&parts);
        return JS_EXCEPTION;
    }
    r = to_parts ? intl_parts_to_array(ctx, &parts, NULL, NULL, true) : intl_parts_to_string(ctx, &parts);
    intl_parts_free(&parts);
    return r;
}

static JSValue js_intl_dtf_resolvedOptions(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue holder, obj;
    IntlDateTimeFormat *dtf = unwrap_dtf(ctx, this_val, "resolvedOptions", true, &holder);
    if (!dtf)
        return JS_EXCEPTION;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        goto fail;
    if (intl_set_prop_str(ctx, obj, "locale", dtf->locale) < 0 ||
        intl_set_prop_str(ctx, obj, "calendar", calendar_names[dtf->calendar]) < 0 ||
        intl_set_prop_str(ctx, obj, "numberingSystem", dtf->numbering) < 0 ||
        intl_set_prop_str(ctx, obj, "timeZone", dtf->time_zone) < 0)
        goto fail;
    if (dtf->hour_cycle >= 0) {
        if (intl_set_prop_str(ctx, obj, "hourCycle", hc_names[dtf->hour_cycle]) < 0 ||
            intl_set_prop_value(ctx, obj, "hour12", JS_NewBool(ctx, dtf->hour_cycle <= 1)) < 0)
            goto fail;
    }
    if (dtf->date_style < 0 && dtf->time_style < 0) {
        for (int f = 0; f < F_COUNT; f++) {
            int v = dtf->fields[f];
            if (v < 0)
                continue;
            if (f == F_FSD) {
                if (intl_set_prop_value(ctx, obj, field_names[f], JS_NewInt32(ctx, v)) < 0)
                    goto fail;
            } else if (intl_set_prop_str(ctx, obj, field_names[f], field_values[f][v]) < 0) {
                goto fail;
            }
        }
    }
    if (dtf->date_style >= 0 && intl_set_prop_str(ctx, obj, "dateStyle", style_names[dtf->date_style]) < 0)
        goto fail;
    if (dtf->time_style >= 0 && intl_set_prop_str(ctx, obj, "timeStyle", style_names[dtf->time_style]) < 0)
        goto fail;
    JS_FreeValue(ctx, holder);
    return obj;
fail:
    JS_FreeValue(ctx, holder);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static const JSCFunctionListEntry js_intl_dtf_proto_funcs[] = {
    JS_CGETSET_DEF("format", js_intl_dtf_get_format, NULL),
    JS_CFUNC_DEF("formatToParts", 1, js_intl_dtf_formatToParts),
    JS_CFUNC_MAGIC_DEF("formatRange", 2, js_intl_dtf_formatRange, 0),
    JS_CFUNC_MAGIC_DEF("formatRangeToParts", 2, js_intl_dtf_formatRange, 1),
    JS_CFUNC_DEF("resolvedOptions", 0, js_intl_dtf_resolvedOptions),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Intl.DateTimeFormat", JS_PROP_CONFIGURABLE),
};

static JSValue js_date_toLocaleString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                      int magic)
{
    static const int req[] = { REQ_ANY, REQ_DATE, REQ_TIME };
    static const int def[] = { DEF_ALL, DEF_DATE, DEF_TIME };
    IntlDateTimeFormat dtf;
    double x;
    IntlParts parts;
    JSValue r;
    if (js_intl_this_time_value(ctx, &x, this_val) < 0)
        return JS_EXCEPTION;
    if (isnan(x))
        return JS_NewString(ctx, "Invalid Date");
    if (dtf_init(ctx, &dtf, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, req[magic],
                 def[magic], JS_UNDEFINED) < 0) {
        dtf_free_fields(&dtf);
        return JS_EXCEPTION;
    }
    intl_parts_init(&parts);
    if (intl_dtf_partition(ctx, &dtf, x, &parts) < 0)
        r = JS_EXCEPTION;
    else
        r = intl_parts_to_string(ctx, &parts);
    intl_parts_free(&parts);
    dtf_free_fields(&dtf);
    return r;
}

static int install_date_method(JSContext *ctx, const char *name, int magic)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Date");
    JSValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
    JSValue f = JS_NewCFunctionMagic(ctx, js_date_toLocaleString, name, 0, JS_CFUNC_generic_magic, magic);
    int r = JS_DefinePropertyValueStr(ctx, proto, name, f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    return r;
}

static const int temporal_kinds[7] = {
    TP_KIND_DATE, TP_KIND_YEAR_MONTH, TP_KIND_MONTH_DAY, TP_KIND_TIME, TP_KIND_DATETIME, TP_KIND_INSTANT, TP_KIND_ZONED,
};

static JSValue js_temporal_toLocaleString(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                          int magic)
{
    static const int req[7] = { REQ_DATE, REQ_DATE, REQ_DATE, REQ_TIME, REQ_ANY, REQ_ANY, REQ_ANY };
    static const int def[7] = { DEF_DATE, DEF_DATE, DEF_DATE, DEF_TIME, DEF_ALL, DEF_ALL, DEF_ALL };
    TPObject *o = JS_GetOpaque(this_val, js_temporal_class_id(temporal_kinds[magic]));
    IntlDateTimeFormat dtf;
    JSValue zone = JS_UNDEFINED, r = JS_EXCEPTION;
    IntlParts parts;
    TPObject instant;
    int tk = magic;
    if (!o)
        return JS_ThrowTypeError(ctx, "not a Temporal object of the expected type");
    if (magic == 6) {
        zone = tp_time_zone_to_string(ctx, o->tz);
        if (JS_IsException(zone))
            return zone;
    }
    if (dtf_init(ctx, &dtf, argc > 0 ? argv[0] : JS_UNDEFINED, argc > 1 ? argv[1] : JS_UNDEFINED, req[magic],
                 def[magic], zone) < 0) {
        JS_FreeValue(ctx, zone);
        dtf_free_fields(&dtf);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, zone);
    if (magic == 6) {
        const char *cal = tp_calendar_id(o->cal);
        if (strcmp(cal, "iso8601") && strcmp(cal, calendar_names[dtf.calendar])) {
            JS_ThrowRangeError(ctx, "calendar %s does not match the formatter's calendar", cal);
            dtf_free_fields(&dtf);
            return JS_EXCEPTION;
        }
        instant = *o;
        o = &instant;
        tk = TK_INSTANT;
    }
    intl_parts_init(&parts);
    if (dtf_partition_value(ctx, &dtf, 0, tk, o, &parts) == 0)
        r = intl_parts_to_string(ctx, &parts);
    intl_parts_free(&parts);
    dtf_free_fields(&dtf);
    return r;
}

static int install_temporal_methods(JSContext *ctx)
{
    for (int i = 0; i < 7; i++) {
        JSValue proto = JS_GetClassProto(ctx, js_temporal_class_id(temporal_kinds[i]));
        int r = 0;
        if (JS_IsObject(proto)) {
            JSValue f = JS_NewCFunctionMagic(ctx, js_temporal_toLocaleString, "toLocaleString", 0,
                                             JS_CFUNC_generic_magic, i);
            r = JS_DefinePropertyValueStr(ctx, proto, "toLocaleString", f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        }
        JS_FreeValue(ctx, proto);
        if (r < 0)
            return -1;
    }
    return 0;
}

int js_intl_init_date_time_format(JSContext *ctx, JSValueConst intl)
{
    if (intl_define_service(ctx, intl, "DateTimeFormat", INTL_CLASS_DATE_TIME_FORMAT,
                            INTL_INTRINSIC_DATE_TIME_FORMAT, js_intl_dtf_ctor, 0, JS_CFUNC_generic,
                            js_intl_dtf_proto_funcs, countof(js_intl_dtf_proto_funcs), dtf_finalizer, dtf_mark) < 0)
        return -1;
    if (install_date_method(ctx, "toLocaleString", 0) < 0 || install_date_method(ctx, "toLocaleDateString", 1) < 0 ||
        install_date_method(ctx, "toLocaleTimeString", 2) < 0 || install_temporal_methods(ctx) < 0)
        return -1;
    return 0;
}
