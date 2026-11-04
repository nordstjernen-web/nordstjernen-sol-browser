/* quickjs-intl-locale.c: Unicode BCP 47 locale identifiers (UTS #35 canonicalization, likely subtags) and Intl.Locale. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "quickjs-intl.h"
#include "quickjs-intl-locale-data.h"


static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static bool is_alnum(int c)
{
    return is_alpha(c) || is_digit(c);
}

static int to_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int to_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static bool all_alpha(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (!is_alpha(s[i]))
            return false;
    return true;
}

static bool all_alnum(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (!is_alnum(s[i]))
            return false;
    return true;
}

static bool all_digit(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (!is_digit(s[i]))
            return false;
    return true;
}

bool intl_is_lang_subtag(const char *s, size_t len)
{
    return ((len >= 2 && len <= 3) || (len >= 5 && len <= 8)) && all_alpha(s, len);
}

bool intl_is_script_subtag(const char *s, size_t len)
{
    return len == 4 && all_alpha(s, len);
}

bool intl_is_region_subtag(const char *s, size_t len)
{
    return (len == 2 && all_alpha(s, len)) || (len == 3 && all_digit(s, len));
}

bool intl_is_variant_subtag(const char *s, size_t len)
{
    if (len >= 5 && len <= 8)
        return all_alnum(s, len);
    return len == 4 && is_digit(s[0]) && all_alnum(s, len);
}

static bool is_attribute(const char *s, size_t len)
{
    return len >= 3 && len <= 8 && all_alnum(s, len);
}

static bool is_ukey(const char *s, size_t len)
{
    return len == 2 && is_alnum(s[0]) && is_alpha(s[1]);
}

static bool is_tkey(const char *s, size_t len)
{
    return len == 2 && is_alpha(s[0]) && is_digit(s[1]);
}

bool intl_is_type_sequence(const char *s)
{
    size_t start = 0, i = 0;
    for (;;) {
        if (s[i] == '-' || s[i] == '\0') {
            if (!is_attribute(s + start, i - start))
                return false;
            if (s[i] == '\0')
                return true;
            start = i + 1;
        }
        i++;
    }
}

void intl_ascii_lower(char *s)
{
    for (; *s; s++)
        *s = to_lower(*s);
}

void intl_ascii_upper(char *s)
{
    for (; *s; s++)
        *s = to_upper(*s);
}

bool intl_ascii_eq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

typedef struct Subtag {
    const char *s;
    size_t len;
} Subtag;

typedef struct Tokens {
    char *buf;
    Subtag *tok;
    int n;
} Tokens;

static void tokens_free(Tokens *t)
{
    free(t->buf);
    free(t->tok);
}

static bool tokenize(const char *tag, Tokens *t)
{
    size_t len = strlen(tag), i, start;
    int n = 1;
    t->buf = NULL;
    t->tok = NULL;
    t->n = 0;
    for (i = 0; i < len; i++)
        if (tag[i] == '-')
            n++;
    t->buf = malloc(len + 1);
    t->tok = malloc(sizeof(Subtag) * n);
    if (!t->buf || !t->tok) {
        tokens_free(t);
        return false;
    }
    for (i = 0; i < len; i++)
        t->buf[i] = to_lower(tag[i]);
    t->buf[len] = '\0';
    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || t->buf[i] == '-') {
            t->tok[t->n].s = t->buf + start;
            t->tok[t->n].len = i - start;
            t->n++;
            start = i + 1;
        }
    }
    return true;
}

static bool subtag_eq(const Subtag *a, const Subtag *b)
{
    return a->len == b->len && !memcmp(a->s, b->s, a->len);
}

static int parse_lang_tokens(const Tokens *t, int i, bool *ok)
{
    int vstart;
    *ok = false;
    if (i >= t->n || !intl_is_lang_subtag(t->tok[i].s, t->tok[i].len))
        return i;
    i++;
    if (i < t->n && intl_is_script_subtag(t->tok[i].s, t->tok[i].len))
        i++;
    if (i < t->n && intl_is_region_subtag(t->tok[i].s, t->tok[i].len))
        i++;
    vstart = i;
    while (i < t->n && intl_is_variant_subtag(t->tok[i].s, t->tok[i].len)) {
        for (int j = vstart; j < i; j++)
            if (subtag_eq(&t->tok[j], &t->tok[i]))
                return i;
        i++;
    }
    *ok = true;
    return i;
}

static int parse_extension(const Tokens *t, int i, bool *ok)
{
    char singleton = t->tok[i].s[0];
    int count = 0;
    *ok = false;
    i++;
    if (singleton == 'u') {
        while (i < t->n && is_attribute(t->tok[i].s, t->tok[i].len)) {
            i++;
            count++;
        }
        while (i < t->n && is_ukey(t->tok[i].s, t->tok[i].len)) {
            i++;
            count++;
            while (i < t->n && is_attribute(t->tok[i].s, t->tok[i].len))
                i++;
        }
    } else if (singleton == 't') {
        if (i < t->n && intl_is_lang_subtag(t->tok[i].s, t->tok[i].len)) {
            bool lok;
            i = parse_lang_tokens(t, i, &lok);
            if (!lok)
                return i;
            count++;
        }
        while (i < t->n && is_tkey(t->tok[i].s, t->tok[i].len)) {
            int vals = 0;
            i++;
            while (i < t->n && is_attribute(t->tok[i].s, t->tok[i].len)) {
                i++;
                vals++;
            }
            if (vals == 0)
                return i;
            count++;
        }
    } else {
        while (i < t->n && t->tok[i].len >= 2 && t->tok[i].len <= 8 &&
               all_alnum(t->tok[i].s, t->tok[i].len)) {
            i++;
            count++;
        }
    }
    if (count == 0)
        return i;
    if (i < t->n && t->tok[i].len != 1)
        return i;
    *ok = true;
    return i;
}

static bool tokens_well_formed(const Tokens *t)
{
    bool ok;
    char seen[36];
    int i, nseen = 0;
    for (i = 0; i < t->n; i++)
        if (t->tok[i].len == 0 || t->tok[i].len > 8 || !all_alnum(t->tok[i].s, t->tok[i].len))
            return false;
    i = parse_lang_tokens(t, 0, &ok);
    if (!ok)
        return false;
    while (i < t->n) {
        char c;
        if (t->tok[i].len != 1)
            return false;
        c = t->tok[i].s[0];
        if (c == 'x') {
            i++;
            if (i >= t->n)
                return false;
            return true;
        }
        for (int j = 0; j < nseen; j++)
            if (seen[j] == c)
                return false;
        seen[nseen++] = c;
        i = parse_extension(t, i, &ok);
        if (!ok)
            return false;
    }
    return true;
}

bool intl_is_well_formed_tag(const char *tag)
{
    Tokens t;
    bool r;
    for (const char *p = tag; *p; p++)
        if (!is_alnum(*p) && *p != '-')
            return false;
    if (!tokenize(tag, &t))
        return false;
    r = tokens_well_formed(&t);
    tokens_free(&t);
    return r;
}

typedef struct LangId {
    char lang[9];
    char script[5];
    char region[4];
    int nvariants;
    int vcap;
    char (*variants)[9];
} LangId;

static void langid_init(LangId *id)
{
    memset(id, 0, sizeof(*id));
}

static void langid_free(LangId *id)
{
    free(id->variants);
    id->variants = NULL;
    id->nvariants = id->vcap = 0;
}

static bool langid_add_variant(LangId *id, const char *v, size_t len)
{
    if (id->nvariants == id->vcap) {
        int cap = id->vcap ? id->vcap * 2 : 4;
        char (*nv)[9] = realloc(id->variants, sizeof(*nv) * cap);
        if (!nv)
            return false;
        id->variants = nv;
        id->vcap = cap;
    }
    memcpy(id->variants[id->nvariants], v, len);
    id->variants[id->nvariants][len] = '\0';
    id->nvariants++;
    return true;
}

static int cmp_variant(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void langid_sort_variants(LangId *id)
{
    if (id->nvariants > 1)
        qsort(id->variants, id->nvariants, sizeof(id->variants[0]), cmp_variant);
}

static bool langid_has_variant(const LangId *id, const char *v, size_t len)
{
    for (int i = 0; i < id->nvariants; i++)
        if (strlen(id->variants[i]) == len && !memcmp(id->variants[i], v, len))
            return true;
    return false;
}

static void langid_remove_variant(LangId *id, const char *v, size_t len)
{
    for (int i = 0; i < id->nvariants; i++) {
        if (strlen(id->variants[i]) == len && !memcmp(id->variants[i], v, len)) {
            memmove(id->variants[i], id->variants[i + 1],
                    sizeof(id->variants[0]) * (id->nvariants - i - 1));
            id->nvariants--;
            return;
        }
    }
}

static void set_field(char *dst, size_t size, const char *s, size_t len, int kind)
{
    size_t i;
    if (len >= size)
        len = size - 1;
    for (i = 0; i < len; i++) {
        int c = s[i];
        if (kind == 1)
            c = i == 0 ? to_upper(c) : to_lower(c);
        else if (kind == 2)
            c = to_upper(c);
        else
            c = to_lower(c);
        dst[i] = c;
    }
    dst[len] = '\0';
}

static int langid_from_tokens(const Tokens *t, int i, LangId *id)
{
    langid_init(id);
    set_field(id->lang, sizeof(id->lang), t->tok[i].s, t->tok[i].len, 0);
    if (!strcmp(id->lang, "und"))
        id->lang[0] = '\0';
    i++;
    if (i < t->n && intl_is_script_subtag(t->tok[i].s, t->tok[i].len)) {
        set_field(id->script, sizeof(id->script), t->tok[i].s, t->tok[i].len, 1);
        i++;
    }
    if (i < t->n && intl_is_region_subtag(t->tok[i].s, t->tok[i].len)) {
        set_field(id->region, sizeof(id->region), t->tok[i].s, t->tok[i].len, 2);
        i++;
    }
    while (i < t->n && intl_is_variant_subtag(t->tok[i].s, t->tok[i].len)) {
        if (!langid_add_variant(id, t->tok[i].s, t->tok[i].len))
            return -1;
        i++;
    }
    langid_sort_variants(id);
    return i;
}

static void langid_format(DynBuf *b, const LangId *id, bool lower)
{
    size_t start = b->size;
    dbuf_putstr(b, id->lang[0] ? id->lang : "und");
    if (id->script[0]) {
        dbuf_putc(b, '-');
        dbuf_putstr(b, id->script);
    }
    if (id->region[0]) {
        dbuf_putc(b, '-');
        dbuf_putstr(b, id->region);
    }
    for (int i = 0; i < id->nvariants; i++) {
        dbuf_putc(b, '-');
        dbuf_putstr(b, id->variants[i]);
    }
    if (lower && !b->error)
        for (size_t i = start; i < b->size; i++)
            b->buf[i] = to_lower(b->buf[i]);
}

static uint32_t likely_lang_code(const char *lang)
{
    uint32_t v = 0;
    size_t len;
    if (!lang[0])
        lang = "und";
    len = strlen(lang);
    if (len > 3)
        return 0;
    for (int i = 0; i < 3; i++)
        v = v * 27 + (i < (int)len ? (uint32_t)(lang[i] - 'a' + 1) : 0);
    return v;
}

static void likely_lang_decode(uint32_t v, char *out)
{
    char tmp[3];
    int n = 0;
    for (int i = 2; i >= 0; i--) {
        tmp[i] = (char)(v % 27);
        v /= 27;
    }
    for (int i = 0; i < 3; i++)
        if (tmp[i])
            out[n++] = 'a' + tmp[i] - 1;
    out[n] = '\0';
}

static int likely_script_index(const char *script)
{
    int lo = 0, hi = countof(intl_likely_scripts) - 1;
    if (!script[0])
        return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = memcmp(intl_likely_scripts[mid], script, 4);
        if (c == 0)
            return mid + 1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int likely_region_index(const char *region)
{
    int lo = 0, hi = countof(intl_likely_regions) - 1;
    if (!region[0])
        return 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(intl_likely_regions[mid], region);
        if (c == 0)
            return mid + 1;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static bool likely_lookup(const char *lang, const char *script, const char *region,
                          char *olang, char *oscript, char *oregion)
{
    uint32_t lc = likely_lang_code(lang), key, val;
    int si = likely_script_index(script), ri = likely_region_index(region);
    int lo = 0, hi = countof(intl_likely_table) - 1;
    if (lc == 0 || si < 0 || ri < 0)
        return false;
    key = (lc << 17) | ((uint32_t)si << 9) | (uint32_t)ri;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (intl_likely_table[mid][0] == key) {
            val = intl_likely_table[mid][1];
            likely_lang_decode(val >> 17, olang);
            if ((val >> 9) & 0xff)
                memcpy(oscript, intl_likely_scripts[((val >> 9) & 0xff) - 1], 4), oscript[4] = '\0';
            else
                oscript[0] = '\0';
            if (val & 0x1ff)
                strcpy(oregion, intl_likely_regions[(val & 0x1ff) - 1]);
            else
                oregion[0] = '\0';
            return true;
        }
        if (intl_likely_table[mid][0] < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

static bool add_likely(char *lang, char *script, char *region)
{
    char ml[9], ms[5], mr[4];
    const char *l = lang;
    if (!strcmp(script, "Zzzz"))
        script[0] = '\0';
    if (!strcmp(region, "ZZ"))
        region[0] = '\0';
    if (lang[0] && strcmp(lang, "und") && script[0] && region[0])
        return true;
    if (!strcmp(lang, "und"))
        l = "";
    if ((script[0] && region[0] && likely_lookup(l, script, region, ml, ms, mr)) ||
        (script[0] && likely_lookup(l, script, "", ml, ms, mr)) ||
        (region[0] && likely_lookup(l, "", region, ml, ms, mr)) ||
        likely_lookup(l, "", "", ml, ms, mr)) {
        if (!l[0])
            strcpy(lang, ml);
        if (!script[0])
            strcpy(script, ms);
        if (!region[0])
            strcpy(region, mr);
        return true;
    }
    return false;
}

static bool likely_region_for(const char *lang, const char *script, char *region)
{
    char l[9], s[5];
    snprintf(l, sizeof(l), "%s", lang);
    snprintf(s, sizeof(s), "%s", script);
    region[0] = '\0';
    return add_likely(l, s, region) && region[0];
}

static const char *pool_str(uint16_t off)
{
    return intl_alias_pool + off;
}

static bool variants_subset(const LangId *id, const char *vlist)
{
    while (*vlist) {
        const char *e = strchr(vlist, '-');
        size_t len = e ? (size_t)(e - vlist) : strlen(vlist);
        if (!langid_has_variant(id, vlist, len))
            return false;
        vlist += len;
        if (*vlist == '-')
            vlist++;
    }
    return true;
}

static bool rule_matches(const IntlAliasRule *r, const LangId *id)
{
    const char *l = pool_str(r->l), *s = pool_str(r->s), *rg = pool_str(r->r);
    if (l[0] && strcmp(l, id->lang))
        return false;
    if (s[0] && strcmp(s, id->script))
        return false;
    if (rg[0] && strcmp(rg, id->region))
        return false;
    return variants_subset(id, pool_str(r->v));
}

static bool rule_apply(const IntlAliasRule *r, LangId *id)
{
    const char *l = pool_str(r->l), *s = pool_str(r->s), *rg = pool_str(r->r), *v = pool_str(r->v);
    const char *rl = pool_str(r->rl), *rs = pool_str(r->rs), *rr = pool_str(r->rr), *rv = pool_str(r->rv);
    char region[4];
    if (l[0])
        snprintf(id->lang, sizeof(id->lang), "%s", rl);
    else if (!id->lang[0] && rl[0])
        snprintf(id->lang, sizeof(id->lang), "%s", rl);
    if (s[0])
        snprintf(id->script, sizeof(id->script), "%s", rs);
    else if (!id->script[0] && rs[0])
        snprintf(id->script, sizeof(id->script), "%s", rs);
    if (strchr(rr, ' ')) {
        const char *p = rr;
        size_t first_len = strcspn(rr, " ");
        snprintf(region, sizeof(region), "%.*s", (int)first_len, rr);
        {
            char likely[4];
            if (likely_region_for(id->lang, id->script, likely)) {
                size_t ll = strlen(likely);
                while (*p) {
                    size_t len = strcspn(p, " ");
                    if (len == ll && !memcmp(p, likely, len)) {
                        snprintf(region, sizeof(region), "%s", likely);
                        break;
                    }
                    p += len;
                    if (*p == ' ')
                        p++;
                }
            }
        }
        if (rg[0] || !id->region[0])
            snprintf(id->region, sizeof(id->region), "%s", region);
    } else if (rg[0]) {
        snprintf(id->region, sizeof(id->region), "%s", rr);
    } else if (!id->region[0] && rr[0]) {
        snprintf(id->region, sizeof(id->region), "%s", rr);
    }
    if (v[0]) {
        const char *p = v;
        while (*p) {
            size_t len = strcspn(p, "-");
            langid_remove_variant(id, p, len);
            p += len;
            if (*p == '-')
                p++;
        }
        p = rv;
        while (*p) {
            size_t len = strcspn(p, "-");
            if (!langid_has_variant(id, p, len) && !langid_add_variant(id, p, len))
                return false;
            p += len;
            if (*p == '-')
                p++;
        }
        langid_sort_variants(id);
    } else if (id->nvariants == 0 && rv[0]) {
        const char *p = rv;
        while (*p) {
            size_t len = strcspn(p, "-");
            if (!langid_add_variant(id, p, len))
                return false;
            p += len;
            if (*p == '-')
                p++;
        }
        langid_sort_variants(id);
    }
    return true;
}

static bool langid_canonicalize(LangId *id)
{
    for (int iter = 0; iter < 64; iter++) {
        const IntlAliasRule *found = NULL;
        for (size_t i = 0; i < countof(intl_alias_rules); i++) {
            const IntlAliasRule *r = &intl_alias_rules[i];
            const char *l = pool_str(r->l);
            if (l[0] && (l[0] != id->lang[0] || strcmp(l, id->lang)))
                continue;
            if (rule_matches(r, id)) {
                found = r;
                break;
            }
        }
        if (!found)
            return true;
        if (!rule_apply(found, id))
            return false;
    }
    return true;
}

static const char *ext_alias_lookup(char ext, const char *key, const char *value, size_t len)
{
    int lo = 0, hi = countof(intl_ext_aliases) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const IntlExtAlias *e = &intl_ext_aliases[mid];
        int c = e->ext - ext;
        if (c == 0)
            c = strcmp(e->key, key);
        if (c == 0) {
            const char *f = intl_ext_alias_pool + e->from;
            size_t flen = strlen(f);
            c = memcmp(f, value, flen < len ? flen : len);
            if (c == 0)
                c = flen < len ? -1 : flen > len ? 1 : 0;
        }
        if (c == 0)
            return intl_ext_alias_pool + e->to;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static const char *subdivision_alias_lookup(const char *value, size_t len)
{
    int lo = 0, hi = countof(intl_subdivision_alias) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *f = intl_subdivision_pool + intl_subdivision_alias[mid][0];
        size_t flen = strlen(f);
        int c = memcmp(f, value, flen < len ? flen : len);
        if (c == 0)
            c = flen < len ? -1 : flen > len ? 1 : 0;
        if (c == 0)
            return intl_subdivision_pool + intl_subdivision_alias[mid][1];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

typedef struct Field {
    char key[3];
    const char *value;
    size_t len;
    int order;
} Field;

static int cmp_field(const void *a, const void *b)
{
    const Field *fa = a, *fb = b;
    int c = strcmp(fa->key, fb->key);
    return c ? c : fa->order - fb->order;
}

static int cmp_subtag(const void *a, const void *b)
{
    const Subtag *sa = a, *sb = b;
    size_t n = sa->len < sb->len ? sa->len : sb->len;
    int c = memcmp(sa->s, sb->s, n);
    if (c)
        return c;
    return (int)sa->len - (int)sb->len;
}

static void put_uvalue(DynBuf *b, const char *key, const char *value, size_t len)
{
    const char *alias;
    char tmp[16];
    if (len == 0)
        return;
    if ((!strcmp(key, "rg") || !strcmp(key, "sd")) && (alias = subdivision_alias_lookup(value, len))) {
        if (strlen(alias) == 2) {
            snprintf(tmp, sizeof(tmp), "%szzzz", alias);
            intl_ascii_lower(tmp);
            alias = tmp;
        }
        value = alias;
        len = strlen(alias);
    } else if ((alias = ext_alias_lookup('u', key, value, len))) {
        value = alias;
        len = strlen(alias);
    }
    if (len == 4 && !memcmp(value, "true", 4))
        return;
    dbuf_putc(b, '-');
    dbuf_put(b, (const uint8_t *)value, len);
}

static bool canon_u_ext(DynBuf *b, const Tokens *t, int start, int end)
{
    int i = start, nattrs = 0, nfields = 0;
    Subtag *attrs = malloc(sizeof(Subtag) * (end - start + 1));
    Field *fields = malloc(sizeof(Field) * (end - start + 1));
    if (!attrs || !fields) {
        free(attrs);
        free(fields);
        return false;
    }
    while (i < end && t->tok[i].len != 2)
        attrs[nattrs++] = t->tok[i++];
    while (i < end) {
        Field *f = &fields[nfields];
        int vs;
        memcpy(f->key, t->tok[i].s, 2);
        f->key[2] = '\0';
        f->order = nfields;
        i++;
        vs = i;
        while (i < end && t->tok[i].len != 2)
            i++;
        if (i > vs) {
            f->value = t->tok[vs].s;
            f->len = (size_t)(t->tok[i - 1].s + t->tok[i - 1].len - t->tok[vs].s);
        } else {
            f->value = "";
            f->len = 0;
        }
        nfields++;
    }
    qsort(attrs, nattrs, sizeof(Subtag), cmp_subtag);
    qsort(fields, nfields, sizeof(Field), cmp_field);
    dbuf_putstr(b, "-u");
    for (int k = 0; k < nattrs; k++) {
        if (k > 0 && subtag_eq(&attrs[k - 1], &attrs[k]))
            continue;
        dbuf_putc(b, '-');
        dbuf_put(b, (const uint8_t *)attrs[k].s, attrs[k].len);
    }
    for (int k = 0; k < nfields; k++) {
        if (k > 0 && !strcmp(fields[k - 1].key, fields[k].key))
            continue;
        dbuf_putc(b, '-');
        dbuf_putstr(b, fields[k].key);
        put_uvalue(b, fields[k].key, fields[k].value, fields[k].len);
    }
    free(attrs);
    free(fields);
    return true;
}

static bool canon_t_ext(DynBuf *b, const Tokens *t, int start, int end)
{
    int i = start, nfields = 0;
    Field *fields;
    dbuf_putstr(b, "-t");
    if (i < end && intl_is_lang_subtag(t->tok[i].s, t->tok[i].len)) {
        LangId id;
        int j = langid_from_tokens(t, i, &id);
        if (j < 0 || !langid_canonicalize(&id)) {
            langid_free(&id);
            return false;
        }
        dbuf_putc(b, '-');
        langid_format(b, &id, true);
        langid_free(&id);
        i = j;
    }
    fields = malloc(sizeof(Field) * (end - start + 1));
    if (!fields)
        return false;
    while (i < end) {
        Field *f = &fields[nfields];
        int vs;
        memcpy(f->key, t->tok[i].s, 2);
        f->key[2] = '\0';
        f->order = nfields;
        i++;
        vs = i;
        while (i < end && !is_tkey(t->tok[i].s, t->tok[i].len))
            i++;
        f->value = t->tok[vs].s;
        f->len = (size_t)(t->tok[i - 1].s + t->tok[i - 1].len - t->tok[vs].s);
        nfields++;
    }
    qsort(fields, nfields, sizeof(Field), cmp_field);
    for (int k = 0; k < nfields; k++) {
        const char *alias = ext_alias_lookup('t', fields[k].key, fields[k].value, fields[k].len);
        dbuf_putc(b, '-');
        dbuf_putstr(b, fields[k].key);
        dbuf_putc(b, '-');
        if (alias)
            dbuf_putstr(b, alias);
        else
            dbuf_put(b, (const uint8_t *)fields[k].value, fields[k].len);
    }
    free(fields);
    return true;
}

typedef struct ExtRange {
    char singleton;
    int start, end;
} ExtRange;

static int cmp_ext(const void *a, const void *b)
{
    return ((const ExtRange *)a)->singleton - ((const ExtRange *)b)->singleton;
}

char *intl_canonicalize_tag(JSContext *ctx, const char *tag)
{
    Tokens t;
    LangId id;
    DynBuf b;
    ExtRange *exts = NULL;
    int i, next, nexts = 0;
    char *res = NULL;

    if (!tokenize(tag, &t))
        goto oom;
    langid_init(&id);
    i = langid_from_tokens(&t, 0, &id);
    if (i < 0 || !langid_canonicalize(&id))
        goto oom_tokens;
    dbuf_init(&b);
    langid_format(&b, &id, false);
    exts = malloc(sizeof(ExtRange) * (t.n + 1));
    if (!exts)
        goto oom_buf;
    while (i < t.n && !(t.tok[i].len == 1 && t.tok[i].s[0] == 'x')) {
        exts[nexts].singleton = t.tok[i].s[0];
        exts[nexts].start = i + 1;
        next = i + 1;
        while (next < t.n && t.tok[next].len != 1)
            next++;
        exts[nexts].end = next;
        nexts++;
        i = next;
    }
    qsort(exts, nexts, sizeof(ExtRange), cmp_ext);
    for (int k = 0; k < nexts; k++) {
        ExtRange *e = &exts[k];
        if (e->singleton == 'u') {
            if (!canon_u_ext(&b, &t, e->start, e->end))
                goto oom_buf;
        } else if (e->singleton == 't') {
            if (!canon_t_ext(&b, &t, e->start, e->end))
                goto oom_buf;
        } else {
            dbuf_putc(&b, '-');
            dbuf_putc(&b, e->singleton);
            for (int j = e->start; j < e->end; j++) {
                dbuf_putc(&b, '-');
                dbuf_put(&b, (const uint8_t *)t.tok[j].s, t.tok[j].len);
            }
        }
    }
    for (; i < t.n; i++) {
        dbuf_putc(&b, '-');
        dbuf_put(&b, (const uint8_t *)t.tok[i].s, t.tok[i].len);
    }
    dbuf_putc(&b, '\0');
    if (b.error)
        goto oom_buf;
    res = intl_strdup(ctx, (const char *)b.buf);
    dbuf_free(&b);
    free(exts);
    langid_free(&id);
    tokens_free(&t);
    return res;
oom_buf:
    dbuf_free(&b);
oom_tokens:
    free(exts);
    langid_free(&id);
    tokens_free(&t);
oom:
    JS_ThrowOutOfMemory(ctx);
    return NULL;
}

size_t intl_base_name_len(const char *tag)
{
    const char *p = tag;
    size_t pos = strcspn(p, "-");
    while (p[pos] == '-') {
        const char *s = p + pos + 1;
        size_t len = strcspn(s, "-");
        if (len <= 1)
            break;
        pos = (size_t)(s - p) + len;
    }
    return pos;
}

const char *intl_find_unicode_ext(const char *tag, size_t *len)
{
    const char *p = tag;
    for (;;) {
        const char *d = strchr(p, '-');
        if (!d)
            return NULL;
        if (d[1] && (d[2] == '-' || d[2] == '\0')) {
            if (d[1] == 'x' || d[1] == 'X')
                return NULL;
            if (d[1] == 'u' || d[1] == 'U') {
                const char *e = d + 2;
                while (*e == '-' && e[1] && e[2] != '-' && e[2] != '\0')
                    e += 1 + strcspn(e + 1, "-");
                *len = (size_t)(e - d);
                return d;
            }
        }
        p = d + 1;
    }
}

bool intl_unicode_ext_value(const char *tag, const char *key, char *buf, size_t size, bool *has_value)
{
    size_t len, i;
    const char *ext = intl_find_unicode_ext(tag, &len);
    *has_value = false;
    buf[0] = '\0';
    if (!ext)
        return false;
    i = 3;
    while (i < len && !(ext[i + 2] == '-' || i + 2 == len))
        i += strcspn(ext + i, "-") + 1;
    while (i < len) {
        size_t klen = strcspn(ext + i, "-");
        if (klen == 2 && !memcmp(ext + i, key, 2)) {
            size_t vs = i + 3, ve = vs;
            if (vs > len)
                return true;
            while (ve < len) {
                size_t sl = strcspn(ext + ve, "-");
                if (sl == 2 || ve + sl > len)
                    break;
                ve += sl + 1;
            }
            if (ve > vs) {
                size_t vlen = ve - vs - 1;
                if (ve >= len)
                    vlen = len - vs;
                if (vlen >= size)
                    vlen = size - 1;
                memcpy(buf, ext + vs, vlen);
                buf[vlen] = '\0';
                *has_value = vlen > 0;
            }
            return true;
        }
        i += klen + 1;
    }
    return false;
}

char *intl_strdup(JSContext *ctx, const char *s)
{
    return intl_strndup(ctx, s, strlen(s));
}

char *intl_strndup(JSContext *ctx, const char *s, size_t len)
{
    char *r = malloc(len + 1);
    if (!r) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

char *intl_strip_unicode_ext(JSContext *ctx, const char *tag)
{
    size_t len;
    const char *ext = intl_find_unicode_ext(tag, &len);
    char *r;
    if (!ext)
        return intl_strdup(ctx, tag);
    r = malloc(strlen(tag) - len + 1);
    if (!r) {
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    memcpy(r, tag, (size_t)(ext - tag));
    strcpy(r + (ext - tag), ext + len);
    return r;
}

bool intl_parse_lang_id(const char *s, size_t len, IntlLangId *out)
{
    char tmp[INTL_TAG_MAX];
    Tokens t;
    LangId id;
    int i;
    bool ok = false;
    if (len >= sizeof(tmp))
        return false;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    if (!tokenize(tmp, &t))
        return false;
    if (t.n > 0 && intl_is_lang_subtag(t.tok[0].s, t.tok[0].len)) {
        i = langid_from_tokens(&t, 0, &id);
        if (i == t.n && id.nvariants <= (int)countof(out->variants)) {
            memset(out, 0, sizeof(*out));
            strcpy(out->lang, id.lang[0] ? id.lang : "und");
            strcpy(out->script, id.script);
            strcpy(out->region, id.region);
            out->nvariants = id.nvariants;
            for (int k = 0; k < id.nvariants; k++)
                strcpy(out->variants[k], id.variants[k]);
            ok = true;
        }
        langid_free(&id);
    }
    tokens_free(&t);
    return ok;
}

void intl_format_lang_id(DynBuf *b, const IntlLangId *id)
{
    dbuf_putstr(b, id->lang);
    if (id->script[0]) {
        dbuf_putc(b, '-');
        dbuf_putstr(b, id->script);
    }
    if (id->region[0]) {
        dbuf_putc(b, '-');
        dbuf_putstr(b, id->region);
    }
    for (int i = 0; i < id->nvariants; i++) {
        dbuf_putc(b, '-');
        dbuf_putstr(b, id->variants[i]);
    }
}

bool intl_add_likely_subtags(IntlLangId *id)
{
    return add_likely(id->lang, id->script, id->region);
}

bool intl_remove_likely_subtags(IntlLangId *id)
{
    char ml[9], ms[5], mr[4];
    char tl[9], ts[5], tr[4];
    snprintf(ml, sizeof(ml), "%s", id->lang);
    snprintf(ms, sizeof(ms), "%s", id->script);
    snprintf(mr, sizeof(mr), "%s", id->region);
    if (!add_likely(ml, ms, mr))
        return false;
    for (int trial = 0; trial < 3; trial++) {
        snprintf(tl, sizeof(tl), "%s", ml);
        ts[0] = tr[0] = '\0';
        if (trial == 1)
            snprintf(tr, sizeof(tr), "%s", mr);
        else if (trial == 2)
            snprintf(ts, sizeof(ts), "%s", ms);
        if (add_likely(tl, ts, tr) && !strcmp(tl, ml) && !strcmp(ts, ms) && !strcmp(tr, mr)) {
            snprintf(id->lang, sizeof(id->lang), "%s", ml);
            snprintf(id->script, sizeof(id->script), "%s", trial == 2 ? ms : "");
            snprintf(id->region, sizeof(id->region), "%s", trial == 1 ? mr : "");
            return true;
        }
    }
    snprintf(id->lang, sizeof(id->lang), "%s", ml);
    snprintf(id->script, sizeof(id->script), "%s", ms);
    snprintf(id->region, sizeof(id->region), "%s", mr);
    return true;
}

char *intl_canonicalize_uvalue(JSContext *ctx, const char *key, const char *value)
{
    char tmp[INTL_TAG_MAX];
    DynBuf b;
    char *r;
    snprintf(tmp, sizeof(tmp), "%s", value);
    intl_ascii_lower(tmp);
    dbuf_init(&b);
    put_uvalue(&b, key, tmp, strlen(tmp));
    dbuf_putc(&b, '\0');
    if (b.error) {
        dbuf_free(&b);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    r = intl_strdup(ctx, b.size > 1 ? (const char *)b.buf + 1 : "");
    dbuf_free(&b);
    return r;
}

void intl_unicode_ext_components(const char *ext, size_t len, IntlExtension *out)
{
    size_t k = 3;
    IntlKeyword *kw = NULL;
    memset(out, 0, sizeof(*out));
    while (k < len) {
        size_t e = k;
        while (e < len && ext[e] != '-')
            e++;
        if (!kw && e - k != 2) {
            bool dup = false;
            for (int i = 0; i < out->nattrs; i++)
                if (strlen(out->attrs[i]) == e - k && !memcmp(out->attrs[i], ext + k, e - k))
                    dup = true;
            if (!dup && out->nattrs < (int)countof(out->attrs) && e - k < sizeof(out->attrs[0])) {
                memcpy(out->attrs[out->nattrs], ext + k, e - k);
                out->attrs[out->nattrs][e - k] = '\0';
                out->nattrs++;
            }
        } else if (e - k == 2) {
            bool dup = false;
            for (int i = 0; i < out->nkeywords; i++)
                if (!memcmp(out->keywords[i].key, ext + k, 2))
                    dup = true;
            if (dup || out->nkeywords >= (int)countof(out->keywords)) {
                static IntlKeyword scratch;
                kw = &scratch;
            } else {
                kw = &out->keywords[out->nkeywords++];
            }
            memcpy(kw->key, ext + k, 2);
            kw->key[2] = '\0';
            kw->value[0] = '\0';
        } else {
            size_t cur = strlen(kw->value);
            if (cur + 1 + (e - k) < sizeof(kw->value)) {
                if (cur)
                    kw->value[cur++] = '-';
                memcpy(kw->value + cur, ext + k, e - k);
                kw->value[cur + e - k] = '\0';
            }
        }
        k = e + 1;
    }
}

char *intl_insert_unicode_ext(JSContext *ctx, const char *locale, const IntlExtension *ext)
{
    DynBuf b;
    const char *pu;
    char *tmp, *r;
    dbuf_init(&b);
    pu = strstr(locale, "-x-");
    if (pu)
        dbuf_put(&b, (const uint8_t *)locale, (size_t)(pu - locale));
    else
        dbuf_putstr(&b, locale);
    if (ext->nattrs || ext->nkeywords) {
        dbuf_putstr(&b, "-u");
        for (int i = 0; i < ext->nattrs; i++) {
            dbuf_putc(&b, '-');
            dbuf_putstr(&b, ext->attrs[i]);
        }
        for (int i = 0; i < ext->nkeywords; i++) {
            dbuf_putc(&b, '-');
            dbuf_putstr(&b, ext->keywords[i].key);
            if (ext->keywords[i].value[0]) {
                dbuf_putc(&b, '-');
                dbuf_putstr(&b, ext->keywords[i].value);
            }
        }
    }
    if (pu)
        dbuf_putstr(&b, pu);
    dbuf_putc(&b, '\0');
    if (b.error) {
        dbuf_free(&b);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    tmp = (char *)b.buf;
    r = intl_canonicalize_tag(ctx, tmp);
    dbuf_free(&b);
    return r;
}

const void *intl_region_info(const char *region)
{
    int lo = 0, hi = countof(intl_region_infos) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(intl_region_infos[mid].region, region);
        if (c == 0)
            return &intl_region_infos[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static void tag_region(const char *tag, char *region)
{
    IntlLangId id;
    region[0] = '\0';
    if (intl_parse_lang_id(tag, intl_base_name_len(tag), &id))
        strcpy(region, id.region);
}

static bool canonical_subdivision_region(JSContext *ctx, const char *tag, const char *key, char *region)
{
    char value[64], buf[16];
    bool has;
    size_t rlen;
    char *canon;
    region[0] = '\0';
    if (!intl_unicode_ext_value(tag, key, value, sizeof(value), &has) || !has)
        return false;
    if (strlen(value) < 3 || strlen(value) > 7 || !all_alnum(value, strlen(value)))
        return false;
    if (is_digit(value[0])) {
        if (!all_digit(value, 3))
            return false;
        rlen = 3;
    } else {
        if (!all_alpha(value, 2))
            return false;
        rlen = 2;
    }
    if (strlen(value) - rlen < 1 || strlen(value) - rlen > 4)
        return false;
    snprintf(buf, sizeof(buf), "und-%.*s", (int)rlen, value);
    canon = intl_canonicalize_tag(ctx, buf);
    if (!canon) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    tag_region(canon, region);
    free(canon);
    return region[0] != '\0';
}

static void region_preference(JSContext *ctx, const char *locale, char *region, char *override)
{
    tag_region(locale, region);
    if (!region[0])
        canonical_subdivision_region(ctx, locale, "sd", region);
    if (!region[0]) {
        IntlLangId id;
        if (intl_parse_lang_id(locale, intl_base_name_len(locale), &id) && intl_add_likely_subtags(&id))
            strcpy(region, id.region);
    }
    if (!region[0])
        strcpy(region, "001");
    if (!canonical_subdivision_region(ctx, locale, "rg", override))
        override[0] = '\0';
}

const char *intl_script_direction(const char *script)
{
    int lo = 0, hi = countof(intl_script_dirs) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = memcmp(intl_script_dirs[mid], script, 4);
        if (c == 0)
            return intl_script_dirs[mid][4] == 'R' ? "rtl" : "ltr";
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

int intl_find_time_zone(const char *name, const char **identifier, const char **primary)
{
    int lo = 0, hi = countof(intl_time_zones) - 1;
    char lower[64], cur[64];
    size_t n = strlen(name);
    if (n >= sizeof(lower))
        return -1;
    for (size_t i = 0; i <= n; i++)
        lower[i] = to_lower(name[i]);
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *s = intl_tz_pool + intl_time_zones[mid].name;
        int c;
        size_t i;
        for (i = 0; s[i] && i < sizeof(cur) - 1; i++)
            cur[i] = to_lower(s[i]);
        cur[i] = '\0';
        c = strcmp(cur, lower);
        if (c == 0) {
            int k = mid;
            while (k > 0) {
                const char *p = intl_tz_pool + intl_time_zones[k - 1].name;
                if (!intl_ascii_eq_nocase(p, name))
                    break;
                k--;
            }
            *identifier = intl_tz_pool + intl_time_zones[k].name;
            *primary = intl_tz_pool + intl_time_zones[k].primary;
            return k;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    for (size_t i = 0; i < countof(intl_time_zones); i++) {
        const char *s = intl_tz_pool + intl_time_zones[i].name;
        if (intl_ascii_eq_nocase(s, name)) {
            *identifier = s;
            *primary = intl_tz_pool + intl_time_zones[i].primary;
            return (int)i;
        }
    }
    return -1;
}

JSValue intl_primary_time_zones(JSContext *ctx, const char *region)
{
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    if (JS_IsException(arr))
        return arr;
    for (size_t i = 0; i < countof(intl_time_zones); i++) {
        const IntlTimeZoneId *z = &intl_time_zones[i];
        if (z->name != z->primary || !strcmp(intl_tz_pool + z->name, "Etc/Unknown") ||
            tp_tzdb_is_link(intl_tz_pool + z->name))
            continue;
        if (region && strcmp(z->region, region))
            continue;
        if (JS_DefinePropertyValueUint32(ctx, arr, n++, JS_NewString(ctx, intl_tz_pool + z->name),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

bool intl_is_well_formed_currency(const char *s)
{
    return strlen(s) == 3 && all_alpha(s, 3);
}

int intl_currency_digits(const char *code)
{
    int lo = 0, hi = countof(intl_currencies) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(intl_currencies[mid].code, code);
        if (c == 0)
            return intl_currencies[mid].digits;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 2;
}

JSValue intl_available_currencies(JSContext *ctx)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (size_t i = 0; i < countof(intl_currencies); i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, intl_currencies[i].code),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

const char *intl_numbering_system_digits(const char *name)
{
    for (size_t i = 0; i < countof(intl_numbering_systems); i++)
        if (!strcmp(intl_numbering_systems[i].name, name))
            return intl_numbering_systems[i].digits;
    return NULL;
}

bool intl_is_numbering_system(const char *name)
{
    return intl_numbering_system_digits(name) != NULL;
}

JSValue intl_available_numbering_systems(JSContext *ctx)
{
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr))
        return arr;
    for (size_t i = 0; i < countof(intl_numbering_systems); i++) {
        if (JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i,
                                         JS_NewString(ctx, intl_numbering_systems[i].name),
                                         JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, arr);
            return JS_EXCEPTION;
        }
    }
    return arr;
}

static const char *const intl_calendars_sorted[] = {
    "buddhist", "chinese", "coptic", "dangi", "ethioaa", "ethiopic", "gregory", "hebrew",
    "indian", "islamic-civil", "islamic-tbla", "islamic-umalqura", "iso8601", "japanese",
    "persian", "roc",
};

bool intl_is_calendar(const char *name)
{
    for (size_t i = 0; i < countof(intl_calendars_sorted); i++)
        if (!strcmp(intl_calendars_sorted[i], name))
            return true;
    return false;
}

JSValue intl_available_calendars(JSContext *ctx)
{
    return intl_new_string_array(ctx, intl_calendars_sorted, countof(intl_calendars_sorted));
}

typedef struct IntlLocale {
    char *locale;
    char *calendar;
    char *collation;
    char *first_day;
    char *hour_cycle;
    char *case_first;
    char *numbering;
    int numeric;
} IntlLocale;

static void intl_locale_finalizer(JSRuntime *rt, JSValueConst val)
{
    IntlLocale *loc = JS_GetOpaque(val, js_intl_class_id(INTL_CLASS_LOCALE));
    if (loc) {
        free(loc->locale);
        free(loc->calendar);
        free(loc->collation);
        free(loc->first_day);
        free(loc->hour_cycle);
        free(loc->case_first);
        free(loc->numbering);
        free(loc);
    }
}

JSValue intl_locale_get_tag(JSContext *ctx, JSValueConst obj, bool *is_locale)
{
    IntlLocale *loc = JS_GetOpaque(obj, js_intl_class_id(INTL_CLASS_LOCALE));
    *is_locale = loc != NULL;
    if (!loc)
        return JS_UNDEFINED;
    return JS_NewString(ctx, loc->locale);
}

static int locale_get_option(JSContext *ctx, JSValueConst options, const char *name,
                             const char *const *values, char **out)
{
    char buf[INTL_TAG_MAX];
    bool present;
    JSValue v;
    const char *s;
    size_t len;
    *out = NULL;
    v = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsException(v))
        return -1;
    if (JS_IsUndefined(v))
        return 0;
    s = JS_ToCStringLen(ctx, &len, v);
    JS_FreeValue(ctx, v);
    if (!s)
        return -1;
    if (values) {
        bool found = false;
        for (int i = 0; values[i]; i++)
            if (strlen(values[i]) == len && !memcmp(values[i], s, len))
                found = true;
        if (!found) {
            JS_ThrowRangeError(ctx, "invalid value for option %s: %s", name, s);
            JS_FreeCString(ctx, s);
            return -1;
        }
    }
    (void)buf;
    (void)present;
    if (strlen(s) != len) {
        *out = intl_strdup(ctx, "\x01");
    } else {
        *out = intl_strdup(ctx, s);
    }
    JS_FreeCString(ctx, s);
    return *out ? 1 : -1;
}

static int locale_get_bool_option(JSContext *ctx, JSValueConst options, const char *name, int *out)
{
    JSValue v = JS_GetPropertyStr(ctx, options, name);
    *out = -1;
    if (JS_IsException(v))
        return -1;
    if (!JS_IsUndefined(v))
        *out = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return 0;
}

static bool is_type_value(const char *s)
{
    return s && s[0] && intl_is_type_sequence(s);
}

static char *update_language_id(JSContext *ctx, const char *tag, JSValueConst options)
{
    IntlLangId id;
    size_t base_len = intl_base_name_len(tag);
    char *language = NULL, *script = NULL, *region = NULL, *variants = NULL;
    DynBuf b;
    char *res = NULL;

    if (!intl_parse_lang_id(tag, base_len, &id)) {
        memset(&id, 0, sizeof(id));
        snprintf(id.lang, sizeof(id.lang), "%.*s", (int)strcspn(tag, "-"), tag);
    }
    if (locale_get_option(ctx, options, "language", NULL, &language) < 0)
        goto done;
    if (language && !intl_is_lang_subtag(language, strlen(language))) {
        JS_ThrowRangeError(ctx, "invalid language subtag: %s", language);
        goto done;
    }
    if (locale_get_option(ctx, options, "script", NULL, &script) < 0)
        goto done;
    if (script && !intl_is_script_subtag(script, strlen(script))) {
        JS_ThrowRangeError(ctx, "invalid script subtag: %s", script);
        goto done;
    }
    if (locale_get_option(ctx, options, "region", NULL, &region) < 0)
        goto done;
    if (region && !intl_is_region_subtag(region, strlen(region))) {
        JS_ThrowRangeError(ctx, "invalid region subtag: %s", region);
        goto done;
    }
    if (locale_get_option(ctx, options, "variants", NULL, &variants) < 0)
        goto done;
    if (variants) {
        const char *p = variants;
        int n = 0;
        char seen[64][9];
        if (!*p)
            goto bad_variants;
        for (;;) {
            size_t len = strcspn(p, "-");
            char v[9];
            if (!intl_is_variant_subtag(p, len))
                goto bad_variants;
            for (size_t i = 0; i < len; i++)
                v[i] = to_lower(p[i]);
            v[len] = '\0';
            for (int i = 0; i < n; i++)
                if (!strcmp(seen[i], v))
                    goto bad_variants;
            if (n < 64)
                strcpy(seen[n++], v);
            p += len;
            if (!*p)
                break;
            p++;
        }
    }
    dbuf_init(&b);
    dbuf_putstr(&b, language ? language : id.lang);
    if (script) {
        dbuf_putc(&b, '-');
        dbuf_putstr(&b, script);
    } else if (id.script[0]) {
        dbuf_putc(&b, '-');
        dbuf_putstr(&b, id.script);
    }
    if (region) {
        dbuf_putc(&b, '-');
        dbuf_putstr(&b, region);
    } else if (id.region[0]) {
        dbuf_putc(&b, '-');
        dbuf_putstr(&b, id.region);
    }
    if (variants) {
        dbuf_putc(&b, '-');
        dbuf_putstr(&b, variants);
    } else {
        for (int i = 0; i < id.nvariants; i++) {
            dbuf_putc(&b, '-');
            dbuf_putstr(&b, id.variants[i]);
        }
    }
    dbuf_putstr(&b, tag + base_len);
    dbuf_putc(&b, '\0');
    if (b.error)
        JS_ThrowOutOfMemory(ctx);
    else
        res = intl_strdup(ctx, (const char *)b.buf);
    dbuf_free(&b);
    goto done;
bad_variants:
    JS_ThrowRangeError(ctx, "invalid variants: %s", variants);
done:
    free(language);
    free(script);
    free(region);
    free(variants);
    return res;
}

static const char *weekday_to_uvalue(const char *fw)
{
    static const char *const names[] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    if (fw[0] >= '0' && fw[0] <= '7' && fw[1] == '\0')
        return names[fw[0] - '0'];
    return fw;
}

static int weekday_uvalue_to_number(const char *fw)
{
    static const char *const names[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
    for (int i = 0; i < 7; i++)
        if (!strcmp(fw, names[i]))
            return i + 1;
    return 0;
}

static const char *const locale_ext_keys[] = { "ca", "co", "fw", "hc", "kf", "kn", "nu" };

static JSValue js_intl_locale_ctor(JSContext *ctx, JSValueConst new_target,
                                   int argc, JSValueConst *argv)
{
    static const char *const hc_values[] = { "h11", "h12", "h23", "h24", NULL };
    static const char *const kf_values[] = { "upper", "lower", "false", NULL };
    JSValueConst tag_val = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValueConst opt_val = argc > 1 ? argv[1] : JS_UNDEFINED;
    JSValue obj = JS_UNDEFINED, options = JS_UNDEFINED;
    IntlLocale *loc = NULL;
    char *tag = NULL, *canon = NULL, *updated = NULL, *tmp;
    char *opts[7] = { NULL };
    int kn = -1;
    const char *s;
    bool is_locale;
    IntlExtension ext;
    char *base;

    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Intl.Locale must be called with new");
    obj = js_intl_create_from_ctor(ctx, new_target, js_intl_class_id(INTL_CLASS_LOCALE));
    if (JS_IsException(obj))
        return obj;
    if (!JS_IsString(tag_val) && !JS_IsObject(tag_val)) {
        JS_ThrowTypeError(ctx, "Intl.Locale: tag must be a string or object");
        goto fail;
    }
    {
        JSValue t = intl_locale_get_tag(ctx, tag_val, &is_locale);
        if (!is_locale)
            t = JS_ToString(ctx, tag_val);
        if (JS_IsException(t))
            goto fail;
        size_t slen;
        s = JS_ToCStringLen(ctx, &slen, t);
        JS_FreeValue(ctx, t);
        if (!s)
            goto fail;
        tag = intl_strdup(ctx, strlen(s) == slen ? s : "\x01");
        JS_FreeCString(ctx, s);
        if (!tag)
            goto fail;
    }
    options = intl_coerce_options(ctx, opt_val);
    if (JS_IsException(options))
        goto fail;
    if (!intl_is_well_formed_tag(tag)) {
        JS_ThrowRangeError(ctx, "invalid language tag: %s", tag);
        goto fail;
    }
    canon = intl_canonicalize_tag(ctx, tag);
    if (!canon)
        goto fail;
    updated = update_language_id(ctx, canon, options);
    if (!updated)
        goto fail;
    if (locale_get_option(ctx, options, "calendar", NULL, &opts[0]) < 0)
        goto fail;
    if (opts[0] && !is_type_value(opts[0])) {
        JS_ThrowRangeError(ctx, "invalid calendar: %s", opts[0]);
        goto fail;
    }
    if (locale_get_option(ctx, options, "collation", NULL, &opts[1]) < 0)
        goto fail;
    if (opts[1] && !is_type_value(opts[1])) {
        JS_ThrowRangeError(ctx, "invalid collation: %s", opts[1]);
        goto fail;
    }
    if (locale_get_option(ctx, options, "firstDayOfWeek", NULL, &opts[2]) < 0)
        goto fail;
    if (opts[2]) {
        const char *fw = weekday_to_uvalue(opts[2]);
        if (!is_type_value(fw)) {
            JS_ThrowRangeError(ctx, "invalid firstDayOfWeek: %s", opts[2]);
            goto fail;
        }
        if (fw != opts[2]) {
            tmp = intl_strdup(ctx, fw);
            if (!tmp)
                goto fail;
            free(opts[2]);
            opts[2] = tmp;
        }
    }
    if (locale_get_option(ctx, options, "hourCycle", hc_values, &opts[3]) < 0)
        goto fail;
    if (locale_get_option(ctx, options, "caseFirst", kf_values, &opts[4]) < 0)
        goto fail;
    if (locale_get_bool_option(ctx, options, "numeric", &kn) < 0)
        goto fail;
    if (kn >= 0) {
        opts[5] = intl_strdup(ctx, kn ? "true" : "false");
        if (!opts[5])
            goto fail;
    }
    if (locale_get_option(ctx, options, "numberingSystem", NULL, &opts[6]) < 0)
        goto fail;
    if (opts[6] && !is_type_value(opts[6])) {
        JS_ThrowRangeError(ctx, "invalid numberingSystem: %s", opts[6]);
        goto fail;
    }

    {
        size_t elen;
        const char *e = intl_find_unicode_ext(updated, &elen);
        if (e)
            intl_unicode_ext_components(e, elen, &ext);
        else
            memset(&ext, 0, sizeof(ext));
    }
    loc = calloc(1, sizeof(*loc));
    if (!loc) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    loc->numeric = -1;
    for (int k = 0; k < 7; k++) {
        const char *key = locale_ext_keys[k];
        IntlKeyword *entry = NULL;
        char *value = NULL;
        bool has_value = false;
        for (int i = 0; i < ext.nkeywords; i++)
            if (!strcmp(ext.keywords[i].key, key))
                entry = &ext.keywords[i];
        if (entry) {
            value = intl_strdup(ctx, entry->value);
            if (!value)
                goto fail;
            has_value = true;
        }
        if (opts[k]) {
            free(value);
            value = intl_canonicalize_uvalue(ctx, key, opts[k]);
            if (!value)
                goto fail;
            has_value = true;
            if (entry) {
                snprintf(entry->value, sizeof(entry->value), "%s", value);
            } else if (ext.nkeywords < (int)countof(ext.keywords)) {
                IntlKeyword *nk = &ext.keywords[ext.nkeywords++];
                strcpy(nk->key, key);
                snprintf(nk->value, sizeof(nk->value), "%s", value);
            }
        }
        if (!has_value)
            continue;
        switch (k) {
        case 0: loc->calendar = value; break;
        case 1: loc->collation = value; break;
        case 2: loc->first_day = value; break;
        case 3: loc->hour_cycle = value; break;
        case 4: loc->case_first = value; break;
        case 5:
            loc->numeric = (!value[0] || !strcmp(value, "true")) ? 1 : 0;
            free(value);
            break;
        case 6: loc->numbering = value; break;
        }
    }
    base = intl_strip_unicode_ext(ctx, updated);
    if (!base)
        goto fail;
    if (ext.nattrs || ext.nkeywords)
        loc->locale = intl_insert_unicode_ext(ctx, base, &ext);
    else
        loc->locale = intl_canonicalize_tag(ctx, base);
    free(base);
    if (!loc->locale)
        goto fail;
    js_intl_set_opaque(obj, loc);
    loc = NULL;
    free(tag);
    free(canon);
    free(updated);
    for (int k = 0; k < 7; k++)
        free(opts[k]);
    JS_FreeValue(ctx, options);
    return obj;
fail:
    if (loc) {
        free(loc->locale);
        free(loc->calendar);
        free(loc->collation);
        free(loc->first_day);
        free(loc->hour_cycle);
        free(loc->case_first);
        free(loc->numbering);
        free(loc);
    }
    free(tag);
    free(canon);
    free(updated);
    for (int k = 0; k < 7; k++)
        free(opts[k]);
    JS_FreeValue(ctx, options);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static IntlLocale *get_locale(JSContext *ctx, JSValueConst this_val, const char *name)
{
    IntlLocale *loc = JS_GetOpaque(this_val, js_intl_class_id(INTL_CLASS_LOCALE));
    if (!loc)
        JS_ThrowTypeError(ctx, "Intl.Locale.prototype.%s called on incompatible receiver", name);
    return loc;
}

static JSValue str_or_undefined(JSContext *ctx, const char *s)
{
    return s ? JS_NewString(ctx, s) : JS_UNDEFINED;
}

enum {
    LOCALE_GET_BASENAME,
    LOCALE_GET_CALENDAR,
    LOCALE_GET_CASEFIRST,
    LOCALE_GET_COLLATION,
    LOCALE_GET_FIRSTDAY,
    LOCALE_GET_HOURCYCLE,
    LOCALE_GET_LANGUAGE,
    LOCALE_GET_NUMBERING,
    LOCALE_GET_NUMERIC,
    LOCALE_GET_REGION,
    LOCALE_GET_SCRIPT,
    LOCALE_GET_VARIANTS,
};

static const char *const locale_getter_names[] = {
    "baseName", "calendar", "caseFirst", "collation", "firstDayOfWeek", "hourCycle",
    "language", "numberingSystem", "numeric", "region", "script", "variants",
};

static JSValue js_intl_locale_getter(JSContext *ctx, JSValueConst this_val, int magic)
{
    IntlLocale *loc = get_locale(ctx, this_val, locale_getter_names[magic]);
    IntlLangId id;
    size_t base_len;
    if (!loc)
        return JS_EXCEPTION;
    base_len = intl_base_name_len(loc->locale);
    switch (magic) {
    case LOCALE_GET_BASENAME:
        return JS_NewStringLen(ctx, loc->locale, base_len);
    case LOCALE_GET_CALENDAR:
        return str_or_undefined(ctx, loc->calendar);
    case LOCALE_GET_CASEFIRST:
        return str_or_undefined(ctx, loc->case_first);
    case LOCALE_GET_COLLATION:
        return str_or_undefined(ctx, loc->collation);
    case LOCALE_GET_FIRSTDAY:
        return str_or_undefined(ctx, loc->first_day);
    case LOCALE_GET_HOURCYCLE:
        return str_or_undefined(ctx, loc->hour_cycle);
    case LOCALE_GET_NUMBERING:
        return str_or_undefined(ctx, loc->numbering);
    case LOCALE_GET_NUMERIC:
        return JS_NewBool(ctx, loc->numeric == 1);
    default:
        break;
    }
    if (!intl_parse_lang_id(loc->locale, base_len, &id)) {
        if (magic == LOCALE_GET_LANGUAGE)
            return JS_NewStringLen(ctx, loc->locale, strcspn(loc->locale, "-"));
        if (magic == LOCALE_GET_VARIANTS) {
            const char *p = loc->locale;
            size_t pos = strcspn(p, "-");
            while (pos < base_len) {
                const char *s = p + pos + 1;
                size_t len = strcspn(s, "-");
                if (intl_is_variant_subtag(s, len))
                    return JS_NewStringLen(ctx, s, base_len - (size_t)(s - p));
                pos = (size_t)(s - p) + len;
            }
        }
        return JS_UNDEFINED;
    }
    switch (magic) {
    case LOCALE_GET_LANGUAGE:
        return JS_NewString(ctx, id.lang);
    case LOCALE_GET_REGION:
        return id.region[0] ? JS_NewString(ctx, id.region) : JS_UNDEFINED;
    case LOCALE_GET_SCRIPT:
        return id.script[0] ? JS_NewString(ctx, id.script) : JS_UNDEFINED;
    case LOCALE_GET_VARIANTS:
        {
            const char *p = loc->locale;
            size_t pos = strcspn(p, "-");
            while (pos < base_len) {
                const char *s = p + pos + 1;
                size_t len = strcspn(s, "-");
                if (intl_is_variant_subtag(s, len))
                    return JS_NewStringLen(ctx, s, base_len - (size_t)(s - p));
                pos = (size_t)(s - p) + len;
            }
            return JS_UNDEFINED;
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_intl_locale_toString(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "toString");
    if (!loc)
        return JS_EXCEPTION;
    return JS_NewString(ctx, loc->locale);
}

static JSValue construct_locale(JSContext *ctx, const char *tag)
{
    JSValue ctor = js_intl_get_intrinsic(ctx, INTL_INTRINSIC_LOCALE);
    JSValue arg = JS_NewString(ctx, tag);
    JSValue r;
    if (JS_IsException(arg))
        return arg;
    r = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, ctor);
    return r;
}

static JSValue js_intl_locale_maximize(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv, int minimize)
{
    IntlLocale *loc = get_locale(ctx, this_val, minimize ? "minimize" : "maximize");
    IntlLangId id;
    size_t base_len;
    DynBuf b;
    JSValue r;
    bool ok;
    if (!loc)
        return JS_EXCEPTION;
    base_len = intl_base_name_len(loc->locale);
    if (!intl_parse_lang_id(loc->locale, base_len, &id))
        return construct_locale(ctx, loc->locale);
    ok = minimize ? intl_remove_likely_subtags(&id) : intl_add_likely_subtags(&id);
    if (!ok)
        return construct_locale(ctx, loc->locale);
    dbuf_init(&b);
    intl_format_lang_id(&b, &id);
    dbuf_putstr(&b, loc->locale + base_len);
    dbuf_putc(&b, '\0');
    if (b.error) {
        dbuf_free(&b);
        return JS_ThrowOutOfMemory(ctx);
    }
    r = construct_locale(ctx, (const char *)b.buf);
    dbuf_free(&b);
    return r;
}

static JSValue single_array(JSContext *ctx, const char *s)
{
    return intl_new_string_array(ctx, &s, 1);
}

static JSValue js_intl_locale_getCalendars(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "getCalendars");
    char region[8], override[8];
    const char *list[8];
    int n = 0;
    if (!loc)
        return JS_EXCEPTION;
    if (loc->calendar)
        return single_array(ctx, loc->calendar);
    region_preference(ctx, loc->locale, region, override);
    for (int pass = 0; pass < 2 && n == 0; pass++) {
        const char *r = pass == 0 ? override : region;
        const IntlRegionInfo *info;
        if (!r[0])
            continue;
        info = intl_region_info(r);
        if (!info || !info->ncal)
            continue;
        for (int i = 0; i < info->ncal; i++) {
            const char *c = intl_region_calendar_names[info->cals[i]];
            bool dup = false;
            if (!intl_is_calendar(c))
                continue;
            for (int j = 0; j < n; j++)
                if (!strcmp(list[j], c))
                    dup = true;
            if (!dup)
                list[n++] = c;
        }
    }
    if (n == 0)
        list[n++] = "gregory";
    return intl_new_string_array(ctx, list, n);
}

JSValue intl_collations_of_locale(JSContext *ctx, const char *locale);
const char *intl_default_numbering_system(const char *locale);

static JSValue js_intl_locale_getCollations(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "getCollations");
    if (!loc)
        return JS_EXCEPTION;
    if (loc->collation)
        return single_array(ctx, loc->collation);
    return intl_collations_of_locale(ctx, loc->locale);
}

static JSValue js_intl_locale_getHourCycles(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv)
{
    static const char *const names[] = { "h11", "h12", "h23", "h24" };
    IntlLocale *loc = get_locale(ctx, this_val, "getHourCycles");
    char region[8], override[8];
    const char *list[4];
    int n = 0;
    if (!loc)
        return JS_EXCEPTION;
    if (loc->hour_cycle)
        return single_array(ctx, loc->hour_cycle);
    region_preference(ctx, loc->locale, region, override);
    for (int pass = 0; pass < 2 && n == 0; pass++) {
        const char *r = pass == 0 ? override : region;
        const IntlRegionInfo *info;
        char key[16];
        int count = 0, bits = 0;
        if (!r[0])
            continue;
        snprintf(key, sizeof(key), "%.*s-%s", (int)strcspn(loc->locale, "-"), loc->locale, r);
        for (size_t i = 0; i < countof(intl_locale_hour_cycles); i++) {
            if (!strcmp(intl_locale_hour_cycles[i].locale, key)) {
                count = intl_locale_hour_cycles[i].hc_count;
                bits = intl_locale_hour_cycles[i].hc_list;
            }
        }
        if (!count) {
            info = intl_region_info(r);
            if (info) {
                count = info->hc_count;
                bits = info->hc_list;
            }
        }
        for (int i = 0; i < count; i++)
            list[n++] = names[(bits >> (2 * i)) & 3];
    }
    if (n == 0)
        list[n++] = "h23";
    return intl_new_string_array(ctx, list, n);
}

static JSValue js_intl_locale_getNumberingSystems(JSContext *ctx, JSValueConst this_val,
                                                  int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "getNumberingSystems");
    if (!loc)
        return JS_EXCEPTION;
    if (loc->numbering)
        return single_array(ctx, loc->numbering);
    return single_array(ctx, intl_default_numbering_system(loc->locale));
}

static JSValue js_intl_locale_getTimeZones(JSContext *ctx, JSValueConst this_val,
                                           int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "getTimeZones");
    char region[8];
    if (!loc)
        return JS_EXCEPTION;
    tag_region(loc->locale, region);
    if (!region[0])
        return JS_UNDEFINED;
    return intl_primary_time_zones(ctx, region);
}

static JSValue js_intl_locale_getTextInfo(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "getTextInfo");
    IntlLangId id;
    const char *dir = NULL;
    JSValue info;
    if (!loc)
        return JS_EXCEPTION;
    info = JS_NewObject(ctx);
    if (JS_IsException(info))
        return info;
    if (intl_parse_lang_id(loc->locale, intl_base_name_len(loc->locale), &id)) {
        if (id.script[0] || (intl_add_likely_subtags(&id) && id.script[0]))
            dir = intl_script_direction(id.script);
    }
    if (JS_DefinePropertyValueStr(ctx, info, "direction", str_or_undefined(ctx, dir),
                                  JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, info);
        return JS_EXCEPTION;
    }
    return info;
}

static JSValue js_intl_locale_getWeekInfo(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    IntlLocale *loc = get_locale(ctx, this_val, "getWeekInfo");
    char region[8], override[8];
    const IntlRegionInfo *info = NULL, *world = intl_region_info("001");
    int first, ws, we, n = 0;
    JSValue obj, weekend;
    if (!loc)
        return JS_EXCEPTION;
    region_preference(ctx, loc->locale, region, override);
    if (override[0])
        info = intl_region_info(override);
    if (!info || !info->first_day)
        info = intl_region_info(region);
    if (!info || !info->first_day)
        info = world;
    first = info->first_day;
    ws = info->weekend_start ? info->weekend_start : world->weekend_start;
    we = info->weekend_end ? info->weekend_end : world->weekend_end;
    if (loc->first_day) {
        int fd = weekday_uvalue_to_number(loc->first_day);
        if (fd)
            first = fd;
    }
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    weekend = JS_NewArray(ctx);
    if (JS_IsException(weekend)) {
        JS_FreeValue(ctx, obj);
        return weekend;
    }
    {
        int days[7], nd = 0, d = ws;
        for (int i = 0; i < 7; i++) {
            days[nd++] = d;
            if (d == we)
                break;
            d = d % 7 + 1;
        }
        for (int i = 1; i < nd; i++)
            for (int j = i; j > 0 && days[j - 1] > days[j]; j--) {
                int t = days[j];
                days[j] = days[j - 1];
                days[j - 1] = t;
            }
        for (int i = 0; i < nd; i++)
            JS_DefinePropertyValueUint32(ctx, weekend, n++, JS_NewInt32(ctx, days[i]), JS_PROP_C_W_E);
    }
    JS_DefinePropertyValueStr(ctx, obj, "firstDay", JS_NewInt32(ctx, first), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, obj, "weekend", weekend, JS_PROP_C_W_E);
    return obj;
}

static const JSCFunctionListEntry js_intl_locale_proto_funcs[] = {
    JS_CFUNC_MAGIC_DEF("maximize", 0, js_intl_locale_maximize, 0),
    JS_CFUNC_MAGIC_DEF("minimize", 0, js_intl_locale_maximize, 1),
    JS_CFUNC_DEF("toString", 0, js_intl_locale_toString),
    JS_CFUNC_DEF("getCalendars", 0, js_intl_locale_getCalendars),
    JS_CFUNC_DEF("getCollations", 0, js_intl_locale_getCollations),
    JS_CFUNC_DEF("getHourCycles", 0, js_intl_locale_getHourCycles),
    JS_CFUNC_DEF("getNumberingSystems", 0, js_intl_locale_getNumberingSystems),
    JS_CFUNC_DEF("getTimeZones", 0, js_intl_locale_getTimeZones),
    JS_CFUNC_DEF("getTextInfo", 0, js_intl_locale_getTextInfo),
    JS_CFUNC_DEF("getWeekInfo", 0, js_intl_locale_getWeekInfo),
    JS_CGETSET_MAGIC_DEF("baseName", js_intl_locale_getter, NULL, LOCALE_GET_BASENAME),
    JS_CGETSET_MAGIC_DEF("calendar", js_intl_locale_getter, NULL, LOCALE_GET_CALENDAR),
    JS_CGETSET_MAGIC_DEF("caseFirst", js_intl_locale_getter, NULL, LOCALE_GET_CASEFIRST),
    JS_CGETSET_MAGIC_DEF("collation", js_intl_locale_getter, NULL, LOCALE_GET_COLLATION),
    JS_CGETSET_MAGIC_DEF("firstDayOfWeek", js_intl_locale_getter, NULL, LOCALE_GET_FIRSTDAY),
    JS_CGETSET_MAGIC_DEF("hourCycle", js_intl_locale_getter, NULL, LOCALE_GET_HOURCYCLE),
    JS_CGETSET_MAGIC_DEF("language", js_intl_locale_getter, NULL, LOCALE_GET_LANGUAGE),
    JS_CGETSET_MAGIC_DEF("numberingSystem", js_intl_locale_getter, NULL, LOCALE_GET_NUMBERING),
    JS_CGETSET_MAGIC_DEF("numeric", js_intl_locale_getter, NULL, LOCALE_GET_NUMERIC),
    JS_CGETSET_MAGIC_DEF("region", js_intl_locale_getter, NULL, LOCALE_GET_REGION),
    JS_CGETSET_MAGIC_DEF("script", js_intl_locale_getter, NULL, LOCALE_GET_SCRIPT),
    JS_CGETSET_MAGIC_DEF("variants", js_intl_locale_getter, NULL, LOCALE_GET_VARIANTS),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Intl.Locale", JS_PROP_CONFIGURABLE),
};

int js_intl_init_locale(JSContext *ctx, JSValueConst intl)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassID cid = js_intl_class_id(INTL_CLASS_LOCALE);
    JSValue proto, ctor;
    if (!JS_IsRegisteredClass(rt, cid)) {
        JSClassDef def = { "Intl.Locale", .finalizer = intl_locale_finalizer };
        if (JS_NewClass(rt, cid, &def) < 0)
            return -1;
    }
    proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return -1;
    JS_SetPropertyFunctionList(ctx, proto, js_intl_locale_proto_funcs,
                               countof(js_intl_locale_proto_funcs));
    ctor = JS_NewCFunction2(ctx, js_intl_locale_ctor, "Locale", 1, JS_CFUNC_constructor, 0);
    if (JS_IsException(ctor)) {
        JS_FreeValue(ctx, proto);
        return -1;
    }
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetClassProto(ctx, cid, proto);
    js_intl_set_intrinsic(ctx, INTL_INTRINSIC_LOCALE, JS_DupValue(ctx, ctor));
    return JS_DefinePropertyValueStr(ctx, intl, "Locale", ctor,
                                     JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

bool intl_locale_available(const char *tag, size_t len)
{
    int lo = 0, hi = countof(intl_available_locales) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *s = intl_available_pool + intl_available_locales[mid];
        size_t slen = strlen(s);
        int c = memcmp(s, tag, slen < len ? slen : len);
        if (c == 0)
            c = slen < len ? -1 : slen > len ? 1 : 0;
        if (c == 0)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

const char *intl_metazone_for(const char *zone, int64_t epoch_minutes)
{
    int lo = 0, hi = countof(intl_zone_metazones) - 1, first = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(intl_metazone_pool + intl_zone_metazones[mid].zone, zone);
        if (c == 0) {
            first = mid;
            hi = mid - 1;
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (first < 0)
        return NULL;
    for (int i = first; i < (int)countof(intl_zone_metazones) &&
                        !strcmp(intl_metazone_pool + intl_zone_metazones[i].zone, zone); i++) {
        if (epoch_minutes >= intl_zone_metazones[i].from && epoch_minutes < intl_zone_metazones[i].to)
            return intl_metazone_pool + intl_zone_metazones[i].metazone;
    }
    return NULL;
}

const char *intl_time_zone_cldr_id(const char *name)
{
    const char *id, *primary;
    int k = intl_find_time_zone(name, &id, &primary);
    if (k < 0)
        return NULL;
    return intl_tz_pool + intl_time_zones[k].cldr;
}

int intl_region_default_calendar(const char *region)
{
    static const char *const supported[] = {
        "buddhist", "chinese", "coptic", "dangi", "ethioaa", "ethiopic", "gregory", "hebrew", "indian",
        "islamic-civil", "islamic-tbla", "islamic-umalqura", "iso8601", "japanese", "persian", "roc",
    };
    const IntlRegionInfo *info = intl_region_info(region);
    if (!info || !info->ncal)
        info = intl_region_info("001");
    for (int i = 0; info && i < info->ncal; i++) {
        const char *c = intl_region_calendar_names[info->cals[i]];
        for (size_t k = 0; k < countof(supported); k++)
            if (!strcmp(supported[k], c))
                return (int)k;
    }
    return 6;
}

void intl_region_hour_cycle_prefs(const char *locale, const char *region, int *hc12, int *hc24)
{
    char key[16];
    int count = 0, bits = 0;
    const IntlRegionInfo *info;
    *hc12 = 1;
    *hc24 = 2;
    snprintf(key, sizeof(key), "%.*s-%s", (int)strcspn(locale, "-"), locale, region);
    for (size_t i = 0; i < countof(intl_locale_hour_cycles); i++) {
        if (!strcmp(intl_locale_hour_cycles[i].locale, key)) {
            count = intl_locale_hour_cycles[i].hc_count;
            bits = intl_locale_hour_cycles[i].hc_list;
        }
    }
    if (!count) {
        info = intl_region_info(region);
        if (!info || !info->hc_count)
            info = intl_region_info("001");
        if (info) {
            count = info->hc_count;
            bits = info->hc_list;
        }
    }
    {
        bool got12 = false, got24 = false;
        for (int i = 0; i < count; i++) {
            int hc = (bits >> (2 * i)) & 3;
            if ((hc == 0 || hc == 1) && !got12) {
                *hc12 = hc;
                got12 = true;
            }
            if ((hc == 2 || hc == 3) && !got24) {
                *hc24 = hc;
                got24 = true;
            }
        }
    }
}

const char *intl_iana_to_windows_zone(const char *iana)
{
    int lo = 0, hi = countof(intl_iana_windows_zones) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(intl_windows_zone_pool + intl_iana_windows_zones[mid][0], iana);
        if (c == 0)
            return intl_windows_zone_pool + intl_iana_windows_zones[mid][1];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}
