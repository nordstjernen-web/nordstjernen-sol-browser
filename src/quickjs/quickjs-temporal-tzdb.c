/* quickjs-temporal-tzdb.c — IANA time zones read from the system tz database (tzdata.zi and TZif files). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cutils.h"
#include "quickjs-temporal.h"
#include "quickjs-intl.h"
#if !defined(_WIN32)
#include <unistd.h>
#else
#include <windows.h>
#include "quickjs-temporal-winzones.h"
#endif

typedef struct TZRule {
    int kind;
    int month, week, weekday, day;
    int32_t time;
} TZRule;

typedef struct TZPosix {
    int32_t std_offset;
    int32_t dst_offset;
    bool has_dst;
    TZRule start, end;
} TZPosix;

typedef struct TZData {
    int count;
    int64_t *times;
    int32_t *offsets;
    int32_t initial;
    bool has_posix;
    int64_t posix_from;
    TZPosix posix;
} TZData;

typedef struct TZEntry {
    char *id;
    int primary;
    int canonical;
    char country[2];
    TZData *data;
    bool failed;
} TZEntry;

enum { TZ_RULE_JULIAN, TZ_RULE_ZERO_BASED, TZ_RULE_MONTH };

static TZEntry *tzdb_entries;
static int tzdb_count;
static int tzdb_capacity;
static char tzdb_dir[1024];
#if JS_HAVE_THREADS
static js_once_t tzdb_once = JS_ONCE_INIT;
static js_mutex_t tzdb_mutex;
#else
static bool tzdb_initialized;
#endif

static void tzdb_lock(void)
{
#if JS_HAVE_THREADS
    js_mutex_lock(&tzdb_mutex);
#endif
}

static void tzdb_unlock(void)
{
#if JS_HAVE_THREADS
    js_mutex_unlock(&tzdb_mutex);
#endif
}

static bool tzdb_ieq(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        char x = a[i], y = b[i];
        if (y == '\0')
            return false;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return false;
    }
    return b[alen] == '\0';
}

static int tzdb_find(const char *s, size_t len)
{
    int i;
    for (i = 0; i < tzdb_count; i++) {
        if (tzdb_ieq(s, len, tzdb_entries[i].id))
            return i;
    }
    return -1;
}

static int tzdb_add(const char *id, size_t len, int primary)
{
    TZEntry *e;
    if (tzdb_count == tzdb_capacity) {
        int cap = tzdb_capacity ? tzdb_capacity * 2 : 256;
        TZEntry *n = realloc(tzdb_entries, sizeof(TZEntry) * (size_t)cap);
        if (!n)
            return -1;
        tzdb_entries = n;
        tzdb_capacity = cap;
    }
    e = &tzdb_entries[tzdb_count];
    e->id = malloc(len + 1);
    if (!e->id)
        return -1;
    memcpy(e->id, id, len);
    e->id[len] = '\0';
    e->primary = primary < 0 ? tzdb_count : primary;
    e->canonical = e->primary;
    e->country[0] = e->country[1] = '\0';
    e->data = NULL;
    e->failed = false;
    return tzdb_count++;
}

static bool tzdb_join(char *out, size_t size, const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    if (a + b + 2 > size)
        return false;
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return true;
}

static bool tzdb_dir_usable(const char *dir)
{
    char path[1100];
    FILE *f;
    if (strlen(dir) >= sizeof(tzdb_dir) || !tzdb_join(path, sizeof(path), dir, "tzdata.zi"))
        return false;
    f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    memcpy(tzdb_dir, dir, strlen(dir) + 1);
    return true;
}

static bool tzdb_find_dir(void)
{
    static const char *const candidates[] = {
        "/usr/share/zoneinfo", "/usr/lib/zoneinfo", "/usr/share/lib/zoneinfo", "/etc/zoneinfo"
    };
    const char *env = getenv("TZDIR");
    size_t i;

    if (env && *env && tzdb_dir_usable(env))
        return true;
#if defined(_WIN32)
    {
        char exe[MAX_PATH], dir[1100];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
        if (n > 0 && n < sizeof(exe)) {
            char *slash = strrchr(exe, '\\');
            if (slash)
                *slash = '\0';
            snprintf(dir, sizeof(dir), "%s/share/zoneinfo", exe);
            if (tzdb_dir_usable(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/../share/zoneinfo", exe);
            if (tzdb_dir_usable(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/zoneinfo", exe);
            if (tzdb_dir_usable(dir))
                return true;
        }
    }
#endif
#if defined(TP_DEFAULT_TZDIR)
    if (tzdb_dir_usable(TP_DEFAULT_TZDIR))
        return true;
#endif
    for (i = 0; i < countof(candidates); i++) {
        if (tzdb_dir_usable(candidates[i]))
            return true;
    }
    return false;
}

static int tzdb_utc_alias_primary(const char *name, size_t len)
{
    if (tzdb_ieq(name, len, "Etc/UTC") || tzdb_ieq(name, len, "Etc/GMT") || tzdb_ieq(name, len, "GMT"))
        return TP_TZ_UTC_ID;
    return -1;
}

static char *tzdb_read_file(const char *path, size_t *plen)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long size;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size < 64 * 1024 * 1024 &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)size + 1);
        if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[size] = '\0';
            *plen = (size_t)size;
        }
    }
    fclose(f);
    return buf;
}

static size_t tzdb_token(const char *p, const char *end, const char **tok)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    *tok = p;
    while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return (size_t)(p - *tok);
}

static void tzdb_load_names(void)
{
    char path[1100];
    char *buf, *line, *end;
    size_t len;
    int pass;

    if (!tzdb_join(path, sizeof(path), tzdb_dir, "tzdata.zi"))
        return;
    buf = tzdb_read_file(path, &len);
    if (!buf)
        return;
    end = buf + len;
    for (pass = 0; pass < 2; pass++) {
        for (line = buf; line < end;) {
            char *next = memchr(line, '\n', (size_t)(end - line));
            const char *a, *b;
            size_t alen, blen;
            if (!next)
                next = end;
            if (pass == 0 && line[0] == 'Z' && line[1] == ' ') {
                alen = tzdb_token(line + 2, next, &a);
                if (alen && !tzdb_ieq(a, alen, "Factory") && tzdb_find(a, alen) < 0)
                    tzdb_add(a, alen, tzdb_utc_alias_primary(a, alen));
            } else if (pass == 1 && line[0] == 'L' && line[1] == ' ') {
                alen = tzdb_token(line + 2, next, &a);
                blen = tzdb_token(a + alen, next, &b);
                if (alen && blen && tzdb_find(b, blen) < 0) {
                    int target = tzdb_find(a, alen);
                    if (target >= 0)
                        tzdb_add(b, blen, tzdb_entries[target].primary);
                }
            }
            line = next + 1;
        }
    }
    free(buf);
}

static void tzdb_split_country_links(void)
{
    char path[1100];
    char *buf, *line, *end;
    size_t len;
    int i;

    if (!tzdb_join(path, sizeof(path), tzdb_dir, "zone.tab"))
        return;
    buf = tzdb_read_file(path, &len);
    if (!buf)
        return;
    end = buf + len;
    for (line = buf; line < end;) {
        char *next = memchr(line, '\n', (size_t)(end - line));
        const char *cc, *coord, *name;
        size_t cclen, coordlen, namelen;
        if (!next)
            next = end;
        if (line[0] != '#') {
            cclen = tzdb_token(line, next, &cc);
            coordlen = tzdb_token(cc + cclen, next, &coord);
            namelen = tzdb_token(coord + coordlen, next, &name);
            if (cclen == 2 && coordlen && namelen) {
                int id = tzdb_find(name, namelen);
                if (id >= 0)
                    memcpy(tzdb_entries[id].country, cc, 2);
            }
        }
        line = next + 1;
    }
    free(buf);
    for (i = 0; i < tzdb_count; i++) {
        TZEntry *e = &tzdb_entries[i];
        if (e->primary != i && e->primary != TP_TZ_UTC_ID && e->country[0] &&
            memcmp(e->country, tzdb_entries[e->primary].country, 2) != 0)
            e->canonical = i;
    }
    for (i = 0; i < tzdb_count; i++) {
        TZEntry *e = &tzdb_entries[i];
        const char *identifier, *cldr_primary;
        int target;
        if (e->canonical == i || e->primary == TP_TZ_UTC_ID ||
            intl_find_time_zone(e->id, &identifier, &cldr_primary) < 0)
            continue;
        target = tzdb_find(cldr_primary, strlen(cldr_primary));
        if (target >= 0 && target != i && tzdb_entries[target].canonical == target &&
            tzdb_entries[target].primary == e->primary)
            e->canonical = target;
    }
}

static void tzdb_init(void)
{
#if JS_HAVE_THREADS
    js_mutex_init(&tzdb_mutex);
#endif
    tzdb_add("UTC", 3, -1);
    if (tzdb_find_dir()) {
        tzdb_load_names();
        tzdb_split_country_links();
    }
}

static void tzdb_ensure(void)
{
#if JS_HAVE_THREADS
    js_once(&tzdb_once, tzdb_init);
#else
    if (!tzdb_initialized) {
        tzdb_initialized = true;
        tzdb_init();
    }
#endif
}

static uint32_t tzdb_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int64_t tzdb_be64(const unsigned char *p)
{
    return (int64_t)(((uint64_t)tzdb_be32(p) << 32) | tzdb_be32(p + 4));
}

static const char *tzdb_posix_name(const char *p)
{
    if (*p == '<') {
        const char *q = strchr(p, '>');
        return q ? q + 1 : NULL;
    }
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
        p++;
    return p;
}

static const char *tzdb_posix_time(const char *p, int32_t *out)
{
    int sign = 1, h = 0, m = 0, s = 0;
    if (*p == '+' || *p == '-') {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    if (*p < '0' || *p > '9')
        return NULL;
    while (*p >= '0' && *p <= '9')
        h = h * 10 + (*p++ - '0');
    if (*p == ':') {
        p++;
        while (*p >= '0' && *p <= '9')
            m = m * 10 + (*p++ - '0');
        if (*p == ':') {
            p++;
            while (*p >= '0' && *p <= '9')
                s = s * 10 + (*p++ - '0');
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return p;
}

static const char *tzdb_posix_rule(const char *p, TZRule *r)
{
    memset(r, 0, sizeof(*r));
    r->time = 7200;
    if (*p == 'M') {
        r->kind = TZ_RULE_MONTH;
        r->month = (int)strtol(p + 1, (char **)&p, 10);
        if (*p++ != '.')
            return NULL;
        r->week = (int)strtol(p, (char **)&p, 10);
        if (*p++ != '.')
            return NULL;
        r->weekday = (int)strtol(p, (char **)&p, 10);
    } else if (*p == 'J') {
        r->kind = TZ_RULE_JULIAN;
        r->day = (int)strtol(p + 1, (char **)&p, 10);
    } else if (*p >= '0' && *p <= '9') {
        r->kind = TZ_RULE_ZERO_BASED;
        r->day = (int)strtol(p, (char **)&p, 10);
    } else {
        return NULL;
    }
    if (*p == '/')
        p = tzdb_posix_time(p + 1, &r->time);
    return p;
}

static bool tzdb_parse_posix(const char *p, TZPosix *out)
{
    int32_t off;
    memset(out, 0, sizeof(*out));
    p = tzdb_posix_name(p);
    if (!p || !(p = tzdb_posix_time(p, &off)))
        return false;
    out->std_offset = -off;
    if (*p == '\0' || *p == '\n')
        return true;
    p = tzdb_posix_name(p);
    if (!p)
        return false;
    out->has_dst = true;
    out->dst_offset = out->std_offset + 3600;
    if (*p && *p != ',') {
        if (!(p = tzdb_posix_time(p, &off)))
            return false;
        out->dst_offset = -off;
    }
    if (*p != ',') {
        out->has_dst = false;
        return true;
    }
    if (!(p = tzdb_posix_rule(p + 1, &out->start)) || *p != ',')
        return false;
    return tzdb_posix_rule(p + 1, &out->end) != NULL;
}

static int64_t tzdb_rule_day(const TZRule *r, int64_t year)
{
    int64_t first;
    if (r->kind == TZ_RULE_JULIAN)
        return tp_epoch_days(year, 0, 1) + r->day - 1 + (tp_is_leap_year(year) && r->day >= 60 ? 1 : 0);
    if (r->kind == TZ_RULE_ZERO_BASED)
        return tp_epoch_days(year, 0, 1) + r->day;
    first = tp_epoch_days(year, r->month - 1, 1);
    {
        int dow = (int)tp_floor_mod64(first + 4, 7);
        int64_t day = first + (r->weekday - dow + 7) % 7 + 7 * (int64_t)(r->week - 1);
        int64_t limit = first + tp_days_in_month(year, r->month);
        while (day >= limit)
            day -= 7;
        return day;
    }
}

typedef struct TZTransition {
    int64_t time;
    int32_t offset;
} TZTransition;

static int tzdb_rule_year(const TZPosix *p, int64_t year, TZTransition out[2])
{
    out[0].time = tzdb_rule_day(&p->start, year) * 86400 + p->start.time - p->std_offset;
    out[0].offset = p->dst_offset;
    out[1].time = tzdb_rule_day(&p->end, year) * 86400 + p->end.time - p->dst_offset;
    out[1].offset = p->std_offset;
    if (out[1].time < out[0].time) {
        TZTransition t = out[0];
        out[0] = out[1];
        out[1] = t;
    }
    return 2;
}

static int64_t tzdb_year_of(int64_t seconds)
{
    return tp_epoch_days_to_date(tp_floor_div64(seconds, 86400)).year;
}

static int32_t tzdb_posix_offset(const TZPosix *p, int64_t t)
{
    TZTransition tr[6];
    int64_t year;
    int i, n = 0, best = -1;

    if (!p->has_dst)
        return p->std_offset;
    year = tzdb_year_of(t + p->std_offset);
    for (i = -1; i <= 1; i++)
        n += tzdb_rule_year(p, year + i, tr + n);
    for (i = 0; i < n; i++) {
        if (tr[i].time <= t && (best < 0 || tr[i].time > tr[best].time))
            best = i;
    }
    return best >= 0 ? tr[best].offset : p->std_offset;
}

static int tzdb_upper_bound(const TZData *d, int64_t t)
{
    int lo = 0, hi = d->count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (d->times[mid] <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int32_t tzdb_offset_at(const TZData *d, int64_t t)
{
    int i;
    if (d->has_posix && t > d->posix_from)
        return tzdb_posix_offset(&d->posix, t);
    i = tzdb_upper_bound(d, t);
    return i == 0 ? d->initial : d->offsets[i - 1];
}

static void tzdb_push(TZData *d, int64_t t, int32_t offset, int *cap)
{
    int32_t previous = d->count ? d->offsets[d->count - 1] : d->initial;
    if (offset == previous)
        return;
    if (d->count == *cap) {
        int ncap = *cap ? *cap * 2 : 64;
        int64_t *nt = realloc(d->times, sizeof(int64_t) * (size_t)ncap);
        int32_t *no;
        if (!nt)
            return;
        d->times = nt;
        no = realloc(d->offsets, sizeof(int32_t) * (size_t)ncap);
        if (!no)
            return;
        d->offsets = no;
        *cap = ncap;
    }
    d->times[d->count] = t;
    d->offsets[d->count] = offset;
    d->count++;
}

static TZData *tzdb_parse_tzif(const unsigned char *buf, size_t len)
{
    const unsigned char *p = buf, *end = buf + len;
    uint32_t isut, isstd, leap, timecnt, typecnt, charcnt;
    int time_size = 4, cap = 0;
    uint32_t i;
    TZData *d;
    const unsigned char *times, *idx, *types;

    if (len < 44 || memcmp(buf, "TZif", 4) != 0)
        return NULL;
    isut = tzdb_be32(p + 20);
    isstd = tzdb_be32(p + 24);
    leap = tzdb_be32(p + 28);
    timecnt = tzdb_be32(p + 32);
    typecnt = tzdb_be32(p + 36);
    charcnt = tzdb_be32(p + 40);
    if (buf[4] >= '2') {
        size_t v1 = (size_t)timecnt * 5 + (size_t)typecnt * 6 + charcnt + (size_t)leap * 8 + isstd + isut;
        p = buf + 44 + v1;
        if (p + 44 > end || memcmp(p, "TZif", 4) != 0)
            return NULL;
        isut = tzdb_be32(p + 20);
        isstd = tzdb_be32(p + 24);
        leap = tzdb_be32(p + 28);
        timecnt = tzdb_be32(p + 32);
        typecnt = tzdb_be32(p + 36);
        charcnt = tzdb_be32(p + 40);
        time_size = 8;
    }
    times = p + 44;
    idx = times + (size_t)timecnt * time_size;
    types = idx + timecnt;
    if (typecnt == 0 || types + (size_t)typecnt * 6 > end)
        return NULL;
    d = calloc(1, sizeof(TZData));
    if (!d)
        return NULL;
    d->initial = (int32_t)tzdb_be32(types);
    d->posix_from = INT64_MIN;
    for (i = 0; i < timecnt; i++) {
        int64_t t = time_size == 8 ? tzdb_be64(times + (size_t)i * 8) : (int32_t)tzdb_be32(times + (size_t)i * 4);
        uint8_t type = idx[i];
        if (type < typecnt)
            tzdb_push(d, t, (int32_t)tzdb_be32(types + (size_t)type * 6), &cap);
        d->posix_from = t;
    }
    if (time_size == 8) {
        const unsigned char *footer = types + (size_t)typecnt * 6 + charcnt + (size_t)leap * 12 + isstd + isut;
        if (footer < end && *footer == '\n') {
            char tz[256];
            size_t n = 0;
            footer++;
            while (footer < end && *footer != '\n' && n < sizeof(tz) - 1)
                tz[n++] = (char)*footer++;
            tz[n] = '\0';
            if (n && tzdb_parse_posix(tz, &d->posix)) {
                d->has_posix = true;
                if (timecnt == 0)
                    d->initial = d->posix.std_offset;
            }
        }
    }
    return d;
}

static TZData *tzdb_load(int primary)
{
    TZEntry *e = &tzdb_entries[primary];
    char path[1400];
    unsigned char *buf;
    size_t len;

    if (e->data || e->failed)
        return e->data;
    buf = tzdb_join(path, sizeof(path), tzdb_dir, e->id) ? (unsigned char *)tzdb_read_file(path, &len) : NULL;
    if (buf) {
        e->data = tzdb_parse_tzif(buf, len);
        free(buf);
    }
    if (!e->data)
        e->failed = true;
    return e->data;
}

int tp_tzdb_lookup(const char *s, size_t len)
{
    int id, primary;
    bool ok;

    tzdb_ensure();
    id = tzdb_find(s, len);
    if (id < 0)
        return -1;
    primary = tzdb_entries[id].primary;
    if (primary == TP_TZ_UTC_ID)
        return id;
    tzdb_lock();
    ok = tzdb_load(primary) != NULL;
    tzdb_unlock();
    return ok ? id : -1;
}

const char *tp_tzdb_identifier(int id)
{
    tzdb_ensure();
    return tzdb_entries[id].id;
}

int tp_tzdb_primary(int id)
{
    tzdb_ensure();
    return tzdb_entries[id].canonical;
}

bool tp_tzdb_is_link(const char *name)
{
    int id;
    tzdb_ensure();
    id = tzdb_find(name, strlen(name));
    return id >= 0 && tzdb_entries[id].canonical != id;
}

static const TZData *tzdb_data(int id)
{
    int primary;
    tzdb_ensure();
    primary = tzdb_entries[id].primary;
    return primary == TP_TZ_UTC_ID ? NULL : tzdb_entries[primary].data;
}

static int64_t tzdb_floor_seconds(tp_i128 ns)
{
    return (int64_t)tp_floor_div128(ns, 1000000000);
}

int64_t tp_tzdb_offset_ns(int id, tp_i128 ns)
{
    const TZData *d = tzdb_data(id);
    if (!d)
        return 0;
    return (int64_t)tzdb_offset_at(d, tzdb_floor_seconds(ns)) * 1000000000LL;
}

static int tzdb_transitions_between(const TZData *d, int64_t lo, int64_t hi, TZTransition *out, int max)
{
    int n = 0, i;
    int64_t last = d->posix_from;

    for (i = tzdb_upper_bound(d, lo); i < d->count && d->times[i] <= hi && n < max; i++) {
        out[n].time = d->times[i];
        out[n].offset = d->offsets[i];
        n++;
    }
    if (d->has_posix && d->posix.has_dst && hi > last) {
        int64_t year, y_end = tzdb_year_of(hi) + 1;
        for (year = tzdb_year_of(lo > last ? lo : last) - 1; year <= y_end && n < max; year++) {
            TZTransition tr[2];
            int k;
            tzdb_rule_year(&d->posix, year, tr);
            for (k = 0; k < 2 && n < max; k++) {
                if (tr[k].time > lo && tr[k].time <= hi && tr[k].time > last)
                    out[n++] = tr[k];
            }
        }
    }
    return n;
}

int tp_tzdb_possible(int id, const TPDateTime *dt, tp_i128 out[2])
{
    const TZData *d = tzdb_data(id);
    tp_i128 wall = tp_utc_epoch_ns(dt), found[8];
    int64_t ws = tzdb_floor_seconds(wall);
    int32_t offsets[16];
    TZTransition tr[14];
    int noffsets = 0, ntr, i, j, nfound = 0;

    if (!d) {
        out[0] = wall;
        return 1;
    }
    offsets[noffsets++] = tzdb_offset_at(d, ws - 2 * 86400 - 1);
    ntr = tzdb_transitions_between(d, ws - 2 * 86400 - 1, ws + 2 * 86400 + 1, tr, 14);
    for (i = 0; i < ntr; i++) {
        bool seen = false;
        for (j = 0; j < noffsets; j++)
            seen |= offsets[j] == tr[i].offset;
        if (!seen && noffsets < 16)
            offsets[noffsets++] = tr[i].offset;
    }
    for (i = 0; i < noffsets && nfound < 8; i++) {
        tp_i128 candidate = wall - (tp_i128)offsets[i] * 1000000000;
        if (tzdb_offset_at(d, tzdb_floor_seconds(candidate)) == offsets[i])
            found[nfound++] = candidate;
    }
    for (i = 1; i < nfound; i++) {
        tp_i128 v = found[i];
        for (j = i; j > 0 && found[j - 1] > v; j--)
            found[j] = found[j - 1];
        found[j] = v;
    }
    if (nfound == 0)
        return 0;
    out[0] = found[0];
    if (nfound == 1 || found[nfound - 1] == found[0])
        return 1;
    out[1] = found[nfound - 1];
    return 2;
}

bool tp_tzdb_next_transition(int id, tp_i128 ns, tp_i128 *out)
{
    const TZData *d = tzdb_data(id);
    int64_t s;
    int i;

    if (!d)
        return false;
    s = tzdb_floor_seconds(ns);
    i = tzdb_upper_bound(d, s);
    if (i < d->count) {
        *out = (tp_i128)d->times[i] * 1000000000;
    } else {
        int64_t last = d->posix_from;
        int64_t year, start = tzdb_year_of(s > last ? s : last);
        bool found = false;
        if (!d->has_posix || !d->posix.has_dst)
            return false;
        for (year = start - 1; year <= start + 2 && !found; year++) {
            TZTransition tr[2];
            int k;
            tzdb_rule_year(&d->posix, year, tr);
            for (k = 0; k < 2 && !found; k++) {
                if (tr[k].time > s && tr[k].time > last &&
                    tzdb_offset_at(d, tr[k].time - 1) != tr[k].offset) {
                    *out = (tp_i128)tr[k].time * 1000000000;
                    found = true;
                }
            }
        }
        if (!found)
            return false;
    }
    return *out <= TP_NS_MAX_INSTANT;
}

bool tp_tzdb_previous_transition(int id, tp_i128 ns, tp_i128 *out)
{
    const TZData *d = tzdb_data(id);
    int64_t c, last;
    int i;

    if (!d)
        return false;
    c = -tzdb_floor_seconds(-ns);
    last = d->posix_from;
    if (d->has_posix && d->posix.has_dst && c > last + 1) {
        int64_t year, stop = last != INT64_MIN ? tzdb_year_of(last) - 1 : tzdb_year_of(c) - 3;
        for (year = tzdb_year_of(c) + 1; year >= stop; year--) {
            TZTransition tr[2];
            int k;
            tzdb_rule_year(&d->posix, year, tr);
            for (k = 1; k >= 0; k--) {
                if (tr[k].time < c && tr[k].time > last && tzdb_offset_at(d, tr[k].time - 1) != tr[k].offset) {
                    *out = (tp_i128)tr[k].time * 1000000000;
                    return *out >= -TP_NS_MAX_INSTANT;
                }
            }
        }
    }
    i = tzdb_upper_bound(d, c - 1);
    if (i == 0)
        return false;
    *out = (tp_i128)d->times[i - 1] * 1000000000;
    return *out >= -TP_NS_MAX_INSTANT;
}

static int tzdb_id_from_path(const char *path)
{
    const char *p = strstr(path, "zoneinfo/");
    if (p)
        path = p + 9;
    if (*path == '\0')
        return -1;
    return tp_tzdb_lookup(path, strlen(path));
}

#if defined(_WIN32)
static bool tzdb_user_region(char region[3])
{
    GEOID geo = GetUserGeoID(GEOCLASS_NATION);
    char iso[8];

    if (geo == GEOID_NOT_AVAILABLE)
        return false;
    if (GetGeoInfoA(geo, GEO_ISO2, iso, sizeof(iso), 0) < 3)
        return false;
    region[0] = iso[0];
    region[1] = iso[1];
    region[2] = ' ';
    return true;
}

const char *tp_windows_system_zone(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION info;
    char name[128];
    char region[3];
    size_t i;

    if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID)
        return NULL;
    for (i = 0; i + 1 < sizeof(name) && info.TimeZoneKeyName[i]; i++) {
        if (info.TimeZoneKeyName[i] > 0x7f)
            return NULL;
        name[i] = (char)info.TimeZoneKeyName[i];
    }
    name[i] = ' ';
    if (tzdb_user_region(region)) {
        for (i = 0; i < countof(tp_windows_zone_regions); i++) {
            if (strcmp(tp_windows_zone_regions[i][0], name) == 0 &&
                strcmp(tp_windows_zone_regions[i][1], region) == 0)
                return tp_windows_zone_regions[i][2];
        }
    }
    for (i = 0; i < countof(tp_windows_zones); i++) {
        if (strcmp(tp_windows_zones[i][0], name) == 0)
            return tp_windows_zones[i][1];
    }
    return NULL;
}

static int tzdb_windows_system(void)
{
    const char *iana = tp_windows_system_zone();

    return iana ? tp_tzdb_lookup(iana, strlen(iana)) : -1;
}
#endif

int tp_tzdb_system(void)
{
    const char *tz = getenv("TZ");
    int id;

    if (tz && *tz) {
        if (*tz == ':')
            tz++;
        id = tzdb_id_from_path(tz);
        if (id >= 0)
            return id;
    }
#if defined(_WIN32)
    id = tzdb_windows_system();
    if (id >= 0)
        return id;
#else
    {
        char buf[1024];
        ssize_t n = readlink("/etc/localtime", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            id = tzdb_id_from_path(buf);
            if (id >= 0)
                return id;
        }
    }
#endif
    return TP_TZ_UTC_ID;
}
