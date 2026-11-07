/* Nordstjernen — Google Safe Browsing v5 local-list client: hash-prefix lists on disk, full-hash confirmation online. */

#define _GNU_SOURCE 1
#include "safebrowsing_google.h"
#include "config.h"
#include "net.h"
#include "ns_sb_key.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <glib/gstdio.h>
#include <openssl/evp.h>

#ifdef G_OS_WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define SBG_API_BASE        "https://safebrowsing.googleapis.com/v5/"
#define SBG_DB_MAGIC        0x31425353u
#define SBG_MAX_ENTRIES     (1u << 25)
#define SBG_MAX_LIST_BYTES  (64u << 20)
#define SBG_MAX_SEARCH_BYTES (1u << 20)
#define SBG_MAX_PREFIXES    64
#define SBG_MIN_WAIT_S      300
#define SBG_MAX_WAIT_S      86400
#define SBG_RETRY_MIN_S     900
#define SBG_RETRY_MAX_S     28800

enum {
    SBG_THREAT_MALWARE = 1,
    SBG_THREAT_SOCIAL_ENGINEERING = 2,
    SBG_THREAT_UNWANTED_SOFTWARE = 3,
    SBG_THREAT_POTENTIALLY_HARMFUL_APPLICATION = 4,
};

static const char *const sbg_lists[] = { "se-4b", "mw-4b", "uws-4b" };

typedef struct {
    gboolean       present;
    guint32        first;
    guint32        rice;
    guint32        count;
    const guint8  *data;
    gsize          len;
} sbg_rice;

typedef struct {
    GBytes        *version;
    gboolean       partial;
    sbg_rice       additions;
    sbg_rice       removals;
    gint64         min_wait_s;
    const guint8  *checksum;
    gsize          checksum_len;
} sbg_update;

typedef struct {
    GBytes  *version;
    gint64   next_update;
    guint32 *values;
    gsize    count;
} sbg_list;

typedef struct {
    ns_sb_verdict verdict;
    gint64        expires;
} sbg_cached;

typedef struct {
    const guint8 *p;
    const guint8 *end;
    gboolean      bad;
} pb_reader;

static GMutex      g_sbg_lock;
static GHashTable *g_sbg_full_hits;
static GHashTable *g_sbg_prefix_misses;
static gint64      g_sbg_retry_at[G_N_ELEMENTS(sbg_lists)];
static guint       g_sbg_failures[G_N_ELEMENTS(sbg_lists)];

static const char *
sbg_api_key(void)
{
    const char *env = g_getenv("NS_SAFE_BROWSING_API_KEY");
    if (env && *env)
        return env;
    return NS_SAFE_BROWSING_API_KEY;
}

gboolean
ns_sbg_available(void)
{
    const ns_config *cfg = ns_config_get();
    const char *key = sbg_api_key();
    return cfg && cfg->safe_browsing && key && *key;
}

static gint64
sbg_now(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static char *
sbg_dir(void)
{
    return g_build_filename(g_get_user_cache_dir(), NS_APP_DIR_NAME,
                            "safebrowsing", NULL);
}

static char *
sbg_list_path(const char *name)
{
    g_autofree char *dir = sbg_dir();
    g_autofree char *file = g_strconcat(name, ".db", NULL);
    return g_build_filename(dir, file, NULL);
}

static size_t
sbg_collect(char *data, size_t size, size_t nmemb, void *user)
{
    GByteArray *buf = user;
    size_t n = size * nmemb;
    if (buf->len + n > SBG_MAX_LIST_BYTES)
        return 0;
    g_byte_array_append(buf, (const guint8 *)data, (guint)n);
    return n;
}

static GByteArray *
sbg_http_get(const char *url, long timeout_s, gsize max_bytes)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return NULL;
    GByteArray *buf = g_byte_array_new();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sbg_collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_bytes);
    ns_net_apply_curl_proxy(curl, url);
    ns_net_apply_curl_tls(curl);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200 || buf->len > max_bytes) {
        g_byte_array_free(buf, TRUE);
        return NULL;
    }
    return buf;
}

static guint64
pb_varint(pb_reader *r)
{
    guint64 v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) {
            r->bad = TRUE;
            return 0;
        }
        guint8 c = *r->p++;
        v |= (guint64)(c & 0x7f) << shift;
        if (!(c & 0x80))
            return v;
    }
    r->bad = TRUE;
    return 0;
}

static gboolean
pb_next(pb_reader *r, guint32 *field, guint32 *wire, guint64 *num,
        const guint8 **data, gsize *len)
{
    if (r->bad || r->p >= r->end)
        return FALSE;
    guint64 key = pb_varint(r);
    *field = (guint32)(key >> 3);
    *wire = (guint32)(key & 7);
    *num = 0;
    *data = NULL;
    *len = 0;
    gsize left = (gsize)(r->end - r->p);
    switch (*wire) {
    case 0:
        *num = pb_varint(r);
        break;
    case 1:
    case 5: {
        gsize n = *wire == 1 ? 8 : 4;
        if (left < n) {
            r->bad = TRUE;
            break;
        }
        *data = r->p;
        *len = n;
        r->p += n;
        break;
    }
    case 2: {
        guint64 n = pb_varint(r);
        if (r->bad || n > (guint64)(r->end - r->p)) {
            r->bad = TRUE;
            break;
        }
        *data = r->p;
        *len = (gsize)n;
        r->p += n;
        break;
    }
    default:
        r->bad = TRUE;
    }
    return !r->bad;
}

static gboolean
sbg_rice_parse(const guint8 *p, gsize n, sbg_rice *out)
{
    memset(out, 0, sizeof *out);
    pb_reader r = { p, p + n, FALSE };
    guint32 field, wire;
    guint64 num;
    const guint8 *data;
    gsize len;
    while (pb_next(&r, &field, &wire, &num, &data, &len)) {
        if (field == 1 && wire == 0)
            out->first = (guint32)num;
        else if (field == 2 && wire == 0)
            out->rice = (guint32)num;
        else if (field == 3 && wire == 0)
            out->count = (guint32)num;
        else if (field == 4 && wire == 2) {
            out->data = data;
            out->len = len;
        }
    }
    out->present = !r.bad;
    return out->present;
}

static guint32 *
sbg_rice_decode(const sbg_rice *rc, gsize *n_out)
{
    *n_out = 0;
    if (!rc->present)
        return g_new0(guint32, 1);
    if (rc->count >= SBG_MAX_ENTRIES ||
        (rc->count > 0 && (rc->rice < 1 || rc->rice > 31)))
        return NULL;
    guint32 *values = g_new(guint32, (gsize)rc->count + 1);
    values[0] = rc->first;
    guint64 current = rc->first;
    gsize bit = 0;
    gsize nbits = rc->len * 8;
    for (guint32 i = 1; i <= rc->count; i++) {
        guint64 quotient = 0;
        for (;;) {
            if (bit >= nbits)
                goto bad;
            guint8 b = (rc->data[bit >> 3] >> (bit & 7)) & 1;
            bit++;
            if (!b)
                break;
            quotient++;
        }
        guint64 remainder = 0;
        for (guint32 j = 0; j < rc->rice; j++) {
            if (bit >= nbits)
                goto bad;
            remainder |= (guint64)((rc->data[bit >> 3] >> (bit & 7)) & 1) << j;
            bit++;
        }
        if (quotient > (G_MAXUINT32 >> rc->rice))
            goto bad;
        current += (quotient << rc->rice) + remainder;
        if (current > G_MAXUINT32)
            goto bad;
        values[i] = (guint32)current;
    }
    *n_out = (gsize)rc->count + 1;
    return values;
bad:
    g_free(values);
    return NULL;
}

static gboolean
sbg_parse_hash_list(const guint8 *p, gsize n, sbg_update *u)
{
    memset(u, 0, sizeof *u);
    pb_reader r = { p, p + n, FALSE };
    guint32 field, wire;
    guint64 num;
    const guint8 *data;
    gsize len;
    while (pb_next(&r, &field, &wire, &num, &data, &len)) {
        if (field == 2 && wire == 2) {
            g_clear_pointer(&u->version, g_bytes_unref);
            u->version = g_bytes_new(data, len);
        } else if (field == 3 && wire == 0) {
            u->partial = num != 0;
        } else if (field == 4 && wire == 2) {
            if (!sbg_rice_parse(data, len, &u->additions))
                r.bad = TRUE;
        } else if (field == 5 && wire == 2) {
            if (!sbg_rice_parse(data, len, &u->removals))
                r.bad = TRUE;
        } else if (field == 6 && wire == 2) {
            pb_reader d = { data, data + len, FALSE };
            guint32 df, dw;
            guint64 dn;
            const guint8 *dd;
            gsize dl;
            while (pb_next(&d, &df, &dw, &dn, &dd, &dl))
                if (df == 1 && dw == 0)
                    u->min_wait_s = (gint64)MIN(dn, (guint64)G_MAXINT32);
        } else if (field == 7 && wire == 2) {
            u->checksum = data;
            u->checksum_len = len;
        } else if ((field == 9 || field == 10 || field == 11) && wire == 2) {
            r.bad = TRUE;
        }
    }
    if (r.bad)
        g_clear_pointer(&u->version, g_bytes_unref);
    return !r.bad;
}

static int
sbg_cmp_u32(const void *a, const void *b)
{
    guint32 x = *(const guint32 *)a;
    guint32 y = *(const guint32 *)b;
    return x < y ? -1 : x > y;
}

static guint32 *
sbg_apply_update(const sbg_list *old, const sbg_update *u, gsize *n_out)
{
    gsize base_n = u->partial ? old->count : 0;
    gsize add_n = 0, rem_n = 0;
    guint32 *adds = sbg_rice_decode(&u->additions, &add_n);
    guint32 *rems = sbg_rice_decode(&u->removals, &rem_n);
    guint32 *out = NULL;
    guint8 *drop = NULL;
    if (!adds || !rems || (rem_n > 0 && !u->partial))
        goto done;
    drop = g_new0(guint8, base_n + 1);
    for (gsize i = 0; i < rem_n; i++) {
        if (rems[i] >= base_n)
            goto done;
        drop[rems[i]] = 1;
    }
    out = g_new(guint32, base_n + add_n + 1);
    gsize n = 0;
    for (gsize i = 0; i < base_n; i++)
        if (!drop[i])
            out[n++] = old->values[i];
    memcpy(out + n, adds, add_n * sizeof *adds);
    n += add_n;
    qsort(out, n, sizeof *out, sbg_cmp_u32);
    *n_out = n;
done:
    g_free(adds);
    g_free(rems);
    g_free(drop);
    return out;
}

static gboolean
sbg_checksum_matches(const guint32 *values, gsize n, const guint8 *want,
                     gsize want_len)
{
    if (!want)
        return TRUE;
    if (want_len != 32)
        return FALSE;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return FALSE;
    gboolean ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    guint8 chunk[4096];
    gsize used = 0;
    for (gsize i = 0; ok && i < n; i++) {
        guint32 be = GUINT32_TO_BE(values[i]);
        memcpy(chunk + used, &be, 4);
        used += 4;
        if (used == sizeof chunk) {
            ok = EVP_DigestUpdate(ctx, chunk, used) == 1;
            used = 0;
        }
    }
    if (ok && used)
        ok = EVP_DigestUpdate(ctx, chunk, used) == 1;
    guint8 digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, digest, &dlen) == 1;
    EVP_MD_CTX_free(ctx);
    return ok && dlen == 32 && memcmp(digest, want, 32) == 0;
}

static void
sbg_list_clear(sbg_list *l)
{
    g_clear_pointer(&l->version, g_bytes_unref);
    g_clear_pointer(&l->values, g_free);
    l->count = 0;
    l->next_update = 0;
}

static gboolean
sbg_read_exact(int fd, void *buf, gsize n)
{
    guint8 *p = buf;
    while (n > 0) {
        gssize got = read(fd, p, (unsigned int)MIN(n, (gsize)G_MAXINT32));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return FALSE;
        p += got;
        n -= (gsize)got;
    }
    return TRUE;
}

static gboolean
sbg_read_header(int fd, GBytes **version, gint64 *next_update, gsize *count,
                gint64 *data_offset)
{
    guint32 magic, vlen, n;
    gint64 next;
    if (!sbg_read_exact(fd, &magic, 4) || GUINT32_FROM_LE(magic) != SBG_DB_MAGIC)
        return FALSE;
    if (!sbg_read_exact(fd, &vlen, 4))
        return FALSE;
    vlen = GUINT32_FROM_LE(vlen);
    if (vlen > 4096)
        return FALSE;
    guint8 *v = g_malloc(vlen + 1);
    if (!sbg_read_exact(fd, v, vlen) || !sbg_read_exact(fd, &next, 8) ||
        !sbg_read_exact(fd, &n, 4)) {
        g_free(v);
        return FALSE;
    }
    n = GUINT32_FROM_LE(n);
    if (n > SBG_MAX_ENTRIES) {
        g_free(v);
        return FALSE;
    }
    if (version)
        *version = g_bytes_new_take(v, vlen);
    else
        g_free(v);
    *next_update = GINT64_FROM_LE(next);
    *count = n;
    *data_offset = 4 + 4 + (gint64)vlen + 8 + 4;
    return TRUE;
}

static gboolean
sbg_list_load(const char *name, sbg_list *l, gboolean with_values)
{
    memset(l, 0, sizeof *l);
    g_autofree char *path = sbg_list_path(name);
    int fd = g_open(path, O_RDONLY | O_BINARY, 0);
    if (fd < 0)
        return FALSE;
    gint64 offset = 0;
    gboolean ok = sbg_read_header(fd, &l->version, &l->next_update, &l->count,
                                  &offset);
    if (ok && with_values) {
        l->values = g_new(guint32, l->count + 1);
        ok = sbg_read_exact(fd, l->values, l->count * 4);
        for (gsize i = 0; ok && i < l->count; i++)
            l->values[i] = GUINT32_FROM_LE(l->values[i]);
    }
    close(fd);
    if (!ok)
        sbg_list_clear(l);
    return ok;
}

static gboolean
sbg_list_save(const char *name, const sbg_list *l)
{
    gsize vlen = l->version ? g_bytes_get_size(l->version) : 0;
    gsize size = 4 + 4 + vlen + 8 + 4 + l->count * 4;
    guint8 *buf = g_malloc(size);
    guint8 *p = buf;
    guint32 u32 = GUINT32_TO_LE(SBG_DB_MAGIC);
    memcpy(p, &u32, 4);
    p += 4;
    u32 = GUINT32_TO_LE((guint32)vlen);
    memcpy(p, &u32, 4);
    p += 4;
    if (vlen)
        memcpy(p, g_bytes_get_data(l->version, NULL), vlen);
    p += vlen;
    gint64 next = GINT64_TO_LE(l->next_update);
    memcpy(p, &next, 8);
    p += 8;
    u32 = GUINT32_TO_LE((guint32)l->count);
    memcpy(p, &u32, 4);
    p += 4;
    for (gsize i = 0; i < l->count; i++) {
        u32 = GUINT32_TO_LE(l->values[i]);
        memcpy(p, &u32, 4);
        p += 4;
    }
    g_autofree char *path = sbg_list_path(name);
    gboolean ok = FALSE;
    for (int attempt = 0; attempt < 5 && !ok; attempt++) {
        ok = g_file_set_contents(path, (const char *)buf, (gssize)size, NULL);
        if (!ok)
            g_usleep(200 * 1000);
    }
    g_free(buf);
    return ok;
}

#ifdef G_OS_WIN32
typedef HANDLE sbg_lock;
#define SBG_NO_LOCK INVALID_HANDLE_VALUE

static sbg_lock
sbg_lock_acquire(const char *path)
{
    wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
    if (!wpath)
        return SBG_NO_LOCK;
    HANDLE h = CreateFileW(wpath, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    g_free(wpath);
    return h;
}

static void
sbg_lock_release(sbg_lock lock)
{
    CloseHandle(lock);
}
#else
typedef int sbg_lock;
#define SBG_NO_LOCK (-1)

static sbg_lock
sbg_lock_acquire(const char *path)
{
    int fd = g_open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return SBG_NO_LOCK;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return SBG_NO_LOCK;
    }
    return fd;
}

static void
sbg_lock_release(sbg_lock lock)
{
    close(lock);
}
#endif

static char *
sbg_query_escape_b64(const guint8 *data, gsize len)
{
    g_autofree char *b64 = g_base64_encode(data, len);
    return g_uri_escape_string(b64, NULL, FALSE);
}

static gint64
sbg_retry_delay(gsize index)
{
    guint fails = MIN(g_sbg_failures[index], 5u);
    gint64 delay = (gint64)SBG_RETRY_MIN_S << fails;
    return MIN(delay, (gint64)SBG_RETRY_MAX_S);
}

static gboolean
sbg_update_list(gsize index)
{
    const char *name = sbg_lists[index];
    sbg_list old;
    if (!sbg_list_load(name, &old, TRUE))
        memset(&old, 0, sizeof old);
    GString *url = g_string_new(SBG_API_BASE "hashList/");
    g_string_append(url, name);
    g_string_append(url, "?key=");
    g_autofree char *key = g_uri_escape_string(sbg_api_key(), NULL, FALSE);
    g_string_append(url, key);
    if (old.version && g_bytes_get_size(old.version) > 0) {
        gsize vlen = 0;
        const guint8 *v = g_bytes_get_data(old.version, &vlen);
        g_autofree char *esc = sbg_query_escape_b64(v, vlen);
        g_string_append(url, "&version=");
        g_string_append(url, esc);
    }
    GByteArray *body = sbg_http_get(url->str, 180, SBG_MAX_LIST_BYTES);
    g_string_free(url, TRUE);
    gboolean ok = FALSE;
    sbg_update u;
    memset(&u, 0, sizeof u);
    if (body && sbg_parse_hash_list(body->data, body->len, &u)) {
        gsize n = 0;
        guint32 *values = sbg_apply_update(&old, &u, &n);
        if (values && sbg_checksum_matches(values, n, u.checksum,
                                           u.checksum_len)) {
            sbg_list fresh = {
                .version = u.version ? g_bytes_ref(u.version) : NULL,
                .next_update = sbg_now() +
                    CLAMP(u.min_wait_s, SBG_MIN_WAIT_S, SBG_MAX_WAIT_S),
                .values = values,
                .count = n,
            };
            ok = sbg_list_save(name, &fresh);
            fresh.values = NULL;
            sbg_list_clear(&fresh);
        } else if (values) {
            g_autofree char *path = sbg_list_path(name);
            g_unlink(path);
        }
        g_free(values);
    }
    g_clear_pointer(&u.version, g_bytes_unref);
    if (body)
        g_byte_array_free(body, TRUE);
    sbg_list_clear(&old);
    if (ok) {
        g_sbg_failures[index] = 0;
        g_sbg_retry_at[index] = 0;
    } else {
        g_sbg_retry_at[index] = sbg_now() + sbg_retry_delay(index);
        g_sbg_failures[index]++;
    }
    return ok;
}

static gint64
sbg_list_due_at(gsize index)
{
    sbg_list l;
    gint64 due = sbg_list_load(sbg_lists[index], &l, FALSE) ? l.next_update : 0;
    sbg_list_clear(&l);
    return MAX(due, g_sbg_retry_at[index]);
}

static gint64
sbg_update_due_lists(void)
{
    gint64 now = sbg_now();
    gboolean any_due = FALSE;
    for (gsize i = 0; i < G_N_ELEMENTS(sbg_lists); i++)
        if (sbg_list_due_at(i) <= now)
            any_due = TRUE;
    if (any_due) {
        g_autofree char *dir = sbg_dir();
        g_autofree char *lock = g_build_filename(dir, "update.lock", NULL);
        sbg_lock held = sbg_lock_acquire(lock);
        if (held != SBG_NO_LOCK) {
            for (gsize i = 0; i < G_N_ELEMENTS(sbg_lists); i++)
                if (sbg_list_due_at(i) <= sbg_now())
                    sbg_update_list(i);
            sbg_lock_release(held);
        } else {
            return 60;
        }
    }
    gint64 next = G_MAXINT64;
    for (gsize i = 0; i < G_N_ELEMENTS(sbg_lists); i++)
        next = MIN(next, sbg_list_due_at(i));
    return next - sbg_now();
}

static gpointer
sbg_updater(gpointer data)
{
    (void)data;
    for (;;) {
        gint64 wait = ns_sbg_available() ? sbg_update_due_lists() : 1800;
        g_usleep((gulong)CLAMP(wait, 60, 1800) * G_USEC_PER_SEC);
    }
    return NULL;
}

void
ns_sbg_start(void)
{
    static gsize started = 0;
    if (!g_once_init_enter(&started))
        return;
    g_autofree char *dir = sbg_dir();
    g_mkdir_with_parents(dir, 0700);
    g_mutex_lock(&g_sbg_lock);
    g_sbg_full_hits = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
                                            (GDestroyNotify)g_bytes_unref,
                                            g_free);
    g_sbg_prefix_misses = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_mutex_unlock(&g_sbg_lock);
    GThread *t = g_thread_try_new("safebrowsing", sbg_updater, NULL, NULL);
    if (t)
        g_thread_unref(t);
    g_once_init_leave(&started, 1);
}

static gboolean
sbg_file_contains(int fd, gint64 offset, gsize count, guint32 want)
{
    gsize lo = 0, hi = count;
    while (lo < hi) {
        gsize mid = lo + (hi - lo) / 2;
        guint32 v;
        if (lseek(fd, (off_t)(offset + (gint64)mid * 4), SEEK_SET) < 0 ||
            !sbg_read_exact(fd, &v, 4))
            return FALSE;
        v = GUINT32_FROM_LE(v);
        if (v == want)
            return TRUE;
        if (v < want)
            lo = mid + 1;
        else
            hi = mid;
    }
    return FALSE;
}

static void
sbg_mark_list_hits(const char *name, const guint32 *prefixes, gsize n,
                   gboolean *hit)
{
    g_autofree char *path = sbg_list_path(name);
    int fd = g_open(path, O_RDONLY | O_BINARY, 0);
    if (fd < 0)
        return;
    gint64 next = 0, offset = 0;
    gsize count = 0;
    if (sbg_read_header(fd, NULL, &next, &count, &offset))
        for (gsize i = 0; i < n; i++)
            if (!hit[i] && sbg_file_contains(fd, offset, count, prefixes[i]))
                hit[i] = TRUE;
    close(fd);
}

static gboolean
sbg_hex_value(char c, int *out)
{
    if (c >= '0' && c <= '9') *out = c - '0';
    else if (c >= 'a' && c <= 'f') *out = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') *out = c - 'A' + 10;
    else return FALSE;
    return TRUE;
}

static char *
sbg_unescape_once(const char *s, gboolean *changed)
{
    GString *out = g_string_sized_new(strlen(s));
    *changed = FALSE;
    for (const char *p = s; *p; p++) {
        int hi, lo;
        if (p[0] == '%' && sbg_hex_value(p[1], &hi) && sbg_hex_value(p[2], &lo) &&
            (hi || lo)) {
            g_string_append_c(out, (char)(hi * 16 + lo));
            p += 2;
            *changed = TRUE;
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

static void
sbg_escape_append(GString *out, const char *s, gsize len)
{
    for (gsize i = 0; i < len; i++) {
        guint8 c = (guint8)s[i];
        if (c <= 0x20 || c >= 0x7f || c == '#' || c == '%')
            g_string_append_printf(out, "%%%02X", c);
        else
            g_string_append_c(out, (char)c);
    }
}

static gboolean
sbg_parse_ipv4_part(const char *s, gsize len, guint64 *out)
{
    if (len == 0)
        return FALSE;
    int base = 10;
    if (len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        len -= 2;
        if (len == 0) {
            *out = 0;
            return TRUE;
        }
    } else if (len > 1 && s[0] == '0') {
        base = 8;
        s++;
        len--;
    }
    guint64 v = 0;
    for (gsize i = 0; i < len; i++) {
        int d;
        if (!sbg_hex_value(s[i], &d) || d >= base)
            return FALSE;
        v = v * (guint64)base + (guint64)d;
        if (v > G_MAXUINT32)
            return FALSE;
    }
    *out = v;
    return TRUE;
}

static char *
sbg_ipv4_canonical(const char *host)
{
    gchar **parts = g_strsplit(host, ".", -1);
    guint n = g_strv_length(parts);
    char *out = NULL;
    guint64 vals[4];
    gboolean ok = n >= 1 && n <= 4;
    for (guint i = 0; ok && i < n; i++)
        ok = sbg_parse_ipv4_part(parts[i], strlen(parts[i]), &vals[i]);
    if (ok) {
        guint64 addr = 0;
        for (guint i = 0; ok && i + 1 < n; i++) {
            if (vals[i] > 255)
                ok = FALSE;
            addr |= vals[i] << (24 - 8 * i);
        }
        guint64 last_max = G_MAXUINT32 >> (8 * (n - 1));
        if (ok && vals[n - 1] <= last_max) {
            addr |= vals[n - 1];
            out = g_strdup_printf("%u.%u.%u.%u", (guint)(addr >> 24) & 255,
                                  (guint)(addr >> 16) & 255,
                                  (guint)(addr >> 8) & 255, (guint)addr & 255);
        }
    }
    g_strfreev(parts);
    return out;
}

static char *
sbg_canonical_host(const char *raw, gboolean *is_ip)
{
    GString *h = g_string_new(NULL);
    for (const char *p = raw; *p; p++) {
        if (*p == '.' && (h->len == 0 || h->str[h->len - 1] == '.'))
            continue;
        g_string_append_c(h, g_ascii_tolower(*p));
    }
    while (h->len && h->str[h->len - 1] == '.')
        g_string_truncate(h, h->len - 1);
    char *ip = h->len ? sbg_ipv4_canonical(h->str) : NULL;
    *is_ip = ip != NULL || (h->len && h->str[0] == '[');
    if (ip) {
        g_string_free(h, TRUE);
        return ip;
    }
    return g_string_free(h, FALSE);
}

static char *
sbg_canonical_path(const char *raw)
{
    gchar **segs = g_strsplit(raw, "/", -1);
    GPtrArray *keep = g_ptr_array_new();
    gboolean trailing = FALSE;
    for (guint i = 0; segs[i]; i++) {
        const char *s = segs[i];
        gboolean last = segs[i + 1] == NULL;
        if (!*s || strcmp(s, ".") == 0) {
            if (last)
                trailing = TRUE;
            continue;
        }
        if (strcmp(s, "..") == 0) {
            if (keep->len)
                g_ptr_array_remove_index(keep, keep->len - 1);
            if (last)
                trailing = TRUE;
            continue;
        }
        g_ptr_array_add(keep, (gpointer)s);
    }
    GString *out = g_string_new("/");
    for (guint i = 0; i < keep->len; i++) {
        if (i)
            g_string_append_c(out, '/');
        g_string_append(out, g_ptr_array_index(keep, i));
    }
    if (trailing && keep->len)
        g_string_append_c(out, '/');
    g_ptr_array_free(keep, TRUE);
    g_strfreev(segs);
    return g_string_free(out, FALSE);
}

static gboolean
sbg_canonicalize(const char *url, char **host_out, char **path_out,
                 char **query_out, gboolean *is_ip)
{
    GString *clean = g_string_new(NULL);
    for (const char *p = url; *p; p++)
        if (*p != '\t' && *p != '\r' && *p != '\n')
            g_string_append_c(clean, *p);
    char *s = g_strstrip(clean->str);
    char *hash = strchr(s, '#');
    if (hash)
        *hash = '\0';
    char *cur = g_strdup(s);
    g_string_free(clean, TRUE);
    for (int i = 0; i < 32; i++) {
        gboolean changed = FALSE;
        char *next = sbg_unescape_once(cur, &changed);
        g_free(cur);
        cur = next;
        if (!changed)
            break;
    }
    const char *rest = cur;
    const char *scheme_end = strstr(cur, "://");
    if (scheme_end)
        rest = scheme_end + 3;
    gsize auth_len = strcspn(rest, "/?");
    g_autofree char *auth = g_strndup(rest, auth_len);
    const char *after = rest + auth_len;
    char *at = strrchr(auth, '@');
    char *hostport = at ? at + 1 : auth;
    if (hostport[0] != '[') {
        char *colon = strrchr(hostport, ':');
        if (colon)
            *colon = '\0';
    } else {
        char *close_br = strchr(hostport, ']');
        if (close_br)
            close_br[1] = '\0';
    }
    g_autofree char *host = sbg_canonical_host(hostport, is_ip);
    const char *q = strchr(after, '?');
    g_autofree char *raw_path = q ? g_strndup(after, (gsize)(q - after))
                                  : g_strdup(after);
    g_autofree char *path = sbg_canonical_path(raw_path);
    gboolean ok = host && *host;
    if (ok) {
        GString *eh = g_string_new(NULL);
        sbg_escape_append(eh, host, strlen(host));
        GString *ep = g_string_new(NULL);
        sbg_escape_append(ep, path, strlen(path));
        *host_out = g_string_free(eh, FALSE);
        *path_out = g_string_free(ep, FALSE);
        if (q) {
            GString *eq = g_string_new(NULL);
            sbg_escape_append(eq, q + 1, strlen(q + 1));
            *query_out = g_string_free(eq, FALSE);
        } else {
            *query_out = NULL;
        }
    }
    g_free(cur);
    return ok;
}

static GPtrArray *
sbg_expressions(const char *url)
{
    char *host = NULL, *path = NULL, *query = NULL;
    gboolean is_ip = FALSE;
    if (!sbg_canonicalize(url, &host, &path, &query, &is_ip))
        return NULL;
    GPtrArray *hosts = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(hosts, g_strdup(host));
    if (!is_ip) {
        gchar **parts = g_strsplit(host, ".", -1);
        guint n = g_strv_length(parts);
        guint start = n > 5 ? n - 5 : 1;
        for (guint i = start; i + 1 < n; i++)
            g_ptr_array_add(hosts, g_strjoinv(".", parts + i));
        g_strfreev(parts);
    }
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    if (query)
        g_ptr_array_add(paths, g_strconcat(path, "?", query, NULL));
    g_ptr_array_add(paths, g_strdup(path));
    GString *prefix = g_string_new("/");
    const char *p = path + 1;
    for (int added = 0; added < 4; added++) {
        if (strcmp(prefix->str, path) != 0)
            g_ptr_array_add(paths, g_strdup(prefix->str));
        const char *slash = strchr(p, '/');
        if (!slash)
            break;
        g_string_append_len(prefix, p, slash - p + 1);
        p = slash + 1;
    }
    g_string_free(prefix, TRUE);
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < hosts->len; i++)
        for (guint j = 0; j < paths->len; j++) {
            char *e = g_strconcat(g_ptr_array_index(hosts, i),
                                  g_ptr_array_index(paths, j), NULL);
            if (g_hash_table_contains(seen, e)) {
                g_free(e);
                continue;
            }
            g_hash_table_add(seen, e);
            g_ptr_array_add(out, e);
        }
    g_hash_table_destroy(seen);
    g_ptr_array_free(hosts, TRUE);
    g_ptr_array_free(paths, TRUE);
    g_free(host);
    g_free(path);
    g_free(query);
    return out;
}

static ns_sb_verdict
sbg_verdict_for_threat(guint64 threat)
{
    switch (threat) {
    case SBG_THREAT_MALWARE:
    case SBG_THREAT_POTENTIALLY_HARMFUL_APPLICATION:
        return NS_SB_MALWARE;
    case SBG_THREAT_SOCIAL_ENGINEERING:
        return NS_SB_SOCIAL_ENGINEERING;
    case SBG_THREAT_UNWANTED_SOFTWARE:
        return NS_SB_UNWANTED_SOFTWARE;
    default:
        return NS_SB_SAFE;
    }
}

static ns_sb_verdict
sbg_worse(ns_sb_verdict a, ns_sb_verdict b)
{
    static const int rank[] = {
        [NS_SB_SAFE] = 0,
        [NS_SB_UNWANTED_SOFTWARE] = 1,
        [NS_SB_SOCIAL_ENGINEERING] = 2,
        [NS_SB_LOCAL_LIST] = 3,
        [NS_SB_MALWARE] = 4,
    };
    return rank[a] >= rank[b] ? a : b;
}

static ns_sb_verdict
sbg_detail_verdict(const guint8 *p, gsize n)
{
    pb_reader r = { p, p + n, FALSE };
    guint32 field, wire;
    guint64 num;
    const guint8 *data;
    gsize len;
    ns_sb_verdict verdict = NS_SB_SAFE;
    gboolean usable = TRUE;
    while (pb_next(&r, &field, &wire, &num, &data, &len)) {
        if (field == 1 && wire == 0) {
            verdict = sbg_verdict_for_threat(num);
        } else if (field == 2 && wire == 0) {
            usable = FALSE;
        } else if (field == 2 && wire == 2 && len > 0) {
            usable = FALSE;
        }
    }
    return r.bad || !usable ? NS_SB_SAFE : verdict;
}

static void
sbg_store_search_response(const guint8 *p, gsize n, const guint32 *queried,
                          gsize queried_n)
{
    pb_reader r = { p, p + n, FALSE };
    guint32 field, wire;
    guint64 num;
    const guint8 *data;
    gsize len;
    gint64 cache_s = SBG_MIN_WAIT_S;
    GPtrArray *found = g_ptr_array_new();
    while (pb_next(&r, &field, &wire, &num, &data, &len)) {
        if (field == 1 && wire == 2) {
            g_ptr_array_add(found, (gpointer)data);
            g_ptr_array_add(found, GSIZE_TO_POINTER(len));
        } else if (field == 2 && wire == 2) {
            pb_reader d = { data, data + len, FALSE };
            guint32 df, dw;
            guint64 dn;
            const guint8 *dd;
            gsize dl;
            while (pb_next(&d, &df, &dw, &dn, &dd, &dl))
                if (df == 1 && dw == 0)
                    cache_s = (gint64)MIN(dn, (guint64)SBG_MAX_WAIT_S);
        }
    }
    if (r.bad) {
        g_ptr_array_free(found, TRUE);
        return;
    }
    gint64 expires = sbg_now() + CLAMP(cache_s, 60, SBG_MAX_WAIT_S);
    g_mutex_lock(&g_sbg_lock);
    for (guint i = 0; i + 1 < found->len; i += 2) {
        const guint8 *fh = g_ptr_array_index(found, i);
        gsize fl = GPOINTER_TO_SIZE(g_ptr_array_index(found, i + 1));
        pb_reader f = { fh, fh + fl, FALSE };
        const guint8 *hash = NULL;
        ns_sb_verdict verdict = NS_SB_SAFE;
        while (pb_next(&f, &field, &wire, &num, &data, &len)) {
            if (field == 1 && wire == 2 && len == 32)
                hash = data;
            else if (field == 2 && wire == 2)
                verdict = sbg_worse(verdict, sbg_detail_verdict(data, len));
        }
        if (f.bad || !hash || verdict == NS_SB_SAFE)
            continue;
        sbg_cached *c = g_new(sbg_cached, 1);
        c->verdict = verdict;
        c->expires = expires;
        g_hash_table_replace(g_sbg_full_hits, g_bytes_new(hash, 32), c);
    }
    for (gsize i = 0; i < queried_n; i++)
        g_hash_table_replace(g_sbg_prefix_misses, GUINT_TO_POINTER(queried[i]),
                             GSIZE_TO_POINTER((gsize)expires));
    g_mutex_unlock(&g_sbg_lock);
    g_ptr_array_free(found, TRUE);
}

static gboolean
sbg_search(const guint32 *prefixes, gsize n)
{
    GString *url = g_string_new(SBG_API_BASE "hashes:search?key=");
    g_autofree char *key = g_uri_escape_string(sbg_api_key(), NULL, FALSE);
    g_string_append(url, key);
    for (gsize i = 0; i < n; i++) {
        guint32 be = GUINT32_TO_BE(prefixes[i]);
        g_autofree char *esc = sbg_query_escape_b64((const guint8 *)&be, 4);
        g_string_append(url, "&hashPrefixes=");
        g_string_append(url, esc);
    }
    GByteArray *body = sbg_http_get(url->str, 5, SBG_MAX_SEARCH_BYTES);
    g_string_free(url, TRUE);
    if (!body)
        return FALSE;
    sbg_store_search_response(body->data, body->len, prefixes, n);
    g_byte_array_free(body, TRUE);
    return TRUE;
}

static ns_sb_verdict
sbg_cached_verdict(guint8 (*hashes)[32], gsize n, const guint32 *prefixes,
                   const gboolean *hit, gboolean *need_search)
{
    ns_sb_verdict verdict = NS_SB_SAFE;
    gint64 now = sbg_now();
    *need_search = FALSE;
    g_mutex_lock(&g_sbg_lock);
    for (gsize i = 0; i < n; i++) {
        if (!hit[i])
            continue;
        GBytes *k = g_bytes_new_static(hashes[i], 32);
        sbg_cached *c = g_hash_table_lookup(g_sbg_full_hits, k);
        g_bytes_unref(k);
        if (c && c->expires > now) {
            verdict = sbg_worse(verdict, c->verdict);
            continue;
        }
        gsize miss_until = GPOINTER_TO_SIZE(
            g_hash_table_lookup(g_sbg_prefix_misses,
                                GUINT_TO_POINTER(prefixes[i])));
        if ((gint64)miss_until <= now)
            *need_search = TRUE;
    }
    g_mutex_unlock(&g_sbg_lock);
    return verdict;
}

ns_sb_verdict
ns_sbg_check_url(const char *url)
{
    if (!url || !ns_sbg_available())
        return NS_SB_SAFE;
    if (g_ascii_strncasecmp(url, "http://", 7) != 0 &&
        g_ascii_strncasecmp(url, "https://", 8) != 0)
        return NS_SB_SAFE;
    ns_sbg_start();
    GPtrArray *exprs = sbg_expressions(url);
    if (!exprs)
        return NS_SB_SAFE;
    gsize n = MIN(exprs->len, (guint)SBG_MAX_PREFIXES);
    guint8 (*hashes)[32] = g_malloc0((n + 1) * 32);
    guint32 *prefixes = g_new0(guint32, n + 1);
    gboolean *hit = g_new0(gboolean, n + 1);
    for (gsize i = 0; i < n; i++) {
        const char *e = g_ptr_array_index(exprs, i);
        unsigned int dlen = 0;
        EVP_Digest(e, strlen(e), hashes[i], &dlen, EVP_sha256(), NULL);
        guint32 be;
        memcpy(&be, hashes[i], 4);
        prefixes[i] = GUINT32_FROM_BE(be);
    }
    g_ptr_array_free(exprs, TRUE);
    for (gsize l = 0; l < G_N_ELEMENTS(sbg_lists); l++)
        sbg_mark_list_hits(sbg_lists[l], prefixes, n, hit);
    gboolean any = FALSE;
    for (gsize i = 0; i < n; i++)
        any = any || hit[i];
    ns_sb_verdict verdict = NS_SB_SAFE;
    if (any) {
        gboolean need_search = FALSE;
        verdict = sbg_cached_verdict(hashes, n, prefixes, hit, &need_search);
        if (verdict == NS_SB_SAFE && need_search) {
            guint32 query[SBG_MAX_PREFIXES];
            gsize qn = 0;
            for (gsize i = 0; i < n; i++) {
                gboolean dup = FALSE;
                for (gsize j = 0; j < qn; j++)
                    dup = dup || query[j] == prefixes[i];
                if (hit[i] && !dup)
                    query[qn++] = prefixes[i];
            }
            if (sbg_search(query, qn))
                verdict = sbg_cached_verdict(hashes, n, prefixes, hit,
                                             &need_search);
        }
    }
    g_free(hashes);
    g_free(prefixes);
    g_free(hit);
    return verdict;
}
