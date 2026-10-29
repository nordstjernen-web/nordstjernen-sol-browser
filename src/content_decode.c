/* Nordstjernen — HTTP Content-Encoding decoder that passes unknown codings through.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "content_decode.h"

#include <string.h>
#include <zlib.h>
#ifdef NS_HAVE_BROTLI_DEC
#include <brotli/decode.h>
#endif
#ifdef NS_HAVE_ZSTD
#include <zstd.h>
#endif

#define NS_DECODE_CHUNK 16384
#define NS_DECODE_MAX_STAGES 5
#define NS_DEFLATE_PROBE_BYTES 64

typedef enum {
    NS_CODING_GZIP,
    NS_CODING_DEFLATE,
    NS_CODING_BR,
    NS_CODING_ZSTD,
} ns_coding;

typedef struct {
    ns_coding   coding;
    gboolean    done;
    gboolean    zlib_ready;
    gboolean    deflate_raw;
    gboolean    deflate_settled;
    GByteArray *deflate_probe;
    z_stream    zs;
#ifdef NS_HAVE_BROTLI_DEC
    BrotliDecoderState *br;
#endif
#ifdef NS_HAVE_ZSTD
    ZSTD_DStream *zstd;
#endif
} ns_decode_stage;

struct ns_content_decoder {
    ns_decode_stage stages[NS_DECODE_MAX_STAGES];
    int             n_stages;
    gsize           limit;
    gboolean        over_limit;
};

const char *
ns_content_decode_accept_encoding(void)
{
    return "gzip, deflate"
#ifdef NS_HAVE_BROTLI_DEC
           ", br"
#endif
#ifdef NS_HAVE_ZSTD
           ", zstd"
#endif
        ;
}

static gboolean
coding_from_token(const char *tok, gsize len, ns_coding *out)
{
    struct { const char *name; ns_coding coding; } known[] = {
        { "gzip", NS_CODING_GZIP },
        { "x-gzip", NS_CODING_GZIP },
        { "deflate", NS_CODING_DEFLATE },
#ifdef NS_HAVE_BROTLI_DEC
        { "br", NS_CODING_BR },
#endif
#ifdef NS_HAVE_ZSTD
        { "zstd", NS_CODING_ZSTD },
#endif
    };
    for (gsize i = 0; i < G_N_ELEMENTS(known); i++) {
        if (strlen(known[i].name) == len &&
            g_ascii_strncasecmp(tok, known[i].name, len) == 0) {
            *out = known[i].coding;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
stage_zlib_init(ns_decode_stage *st, int window_bits)
{
    if (st->zlib_ready) inflateEnd(&st->zs);
    memset(&st->zs, 0, sizeof st->zs);
    st->zlib_ready = inflateInit2(&st->zs, window_bits) == Z_OK;
    return st->zlib_ready;
}

static gboolean
stage_init(ns_decode_stage *st, ns_coding coding)
{
    memset(st, 0, sizeof *st);
    st->coding = coding;
    switch (coding) {
    case NS_CODING_GZIP:
        return stage_zlib_init(st, 15 + 32);
    case NS_CODING_DEFLATE:
        st->deflate_probe = g_byte_array_new();
        return stage_zlib_init(st, 15);
    case NS_CODING_BR:
#ifdef NS_HAVE_BROTLI_DEC
        st->br = BrotliDecoderCreateInstance(NULL, NULL, NULL);
        return st->br != NULL;
#else
        return FALSE;
#endif
    case NS_CODING_ZSTD:
#ifdef NS_HAVE_ZSTD
        st->zstd = ZSTD_createDStream();
        return st->zstd && !ZSTD_isError(ZSTD_initDStream(st->zstd));
#else
        return FALSE;
#endif
    }
    return FALSE;
}

static void
stage_clear(ns_decode_stage *st)
{
    if (st->zlib_ready) inflateEnd(&st->zs);
    if (st->deflate_probe) g_byte_array_free(st->deflate_probe, TRUE);
#ifdef NS_HAVE_BROTLI_DEC
    if (st->br) BrotliDecoderDestroyInstance(st->br);
#endif
#ifdef NS_HAVE_ZSTD
    if (st->zstd) ZSTD_freeDStream(st->zstd);
#endif
    memset(st, 0, sizeof *st);
}

static gboolean
append_capped(ns_content_decoder *dec, GByteArray *out, const guint8 *buf,
              gsize n)
{
    if (out->len + n > dec->limit) {
        dec->over_limit = TRUE;
        return FALSE;
    }
    g_byte_array_append(out, buf, (guint)n);
    return TRUE;
}

static int
zlib_run(ns_content_decoder *dec, ns_decode_stage *st, const guint8 *data,
         gsize len, GByteArray *out, gsize *produced)
{
    guint8 buf[NS_DECODE_CHUNK];
    st->zs.next_in = (Bytef *)data;
    st->zs.avail_in = (uInt)len;
    *produced = 0;
    while (st->zs.avail_in > 0 && !st->done) {
        st->zs.next_out = buf;
        st->zs.avail_out = sizeof buf;
        int rc = inflate(&st->zs, Z_NO_FLUSH);
        gsize n = sizeof buf - st->zs.avail_out;
        if (n) {
            if (!append_capped(dec, out, buf, n)) return Z_MEM_ERROR;
            *produced += n;
        }
        if (rc == Z_STREAM_END) {
            gboolean next_gzip_member = st->coding == NS_CODING_GZIP &&
                st->zs.avail_in >= 2 && st->zs.next_in[0] == 0x1f &&
                st->zs.next_in[1] == 0x8b;
            if (next_gzip_member && inflateReset(&st->zs) == Z_OK) continue;
            st->done = TRUE;
        } else if (rc == Z_BUF_ERROR && n == 0) {
            break;
        } else if (rc != Z_OK) {
            return rc;
        }
    }
    return Z_OK;
}

static gboolean
stage_feed_deflate(ns_content_decoder *dec, ns_decode_stage *st,
                   const guint8 *data, gsize len, GByteArray *out)
{
    gsize produced = 0;
    if (st->deflate_settled)
        return zlib_run(dec, st, data, len, out, &produced) == Z_OK;
    g_byte_array_append(st->deflate_probe, data, (guint)len);
    guint mark = out->len;
    int rc = zlib_run(dec, st, data, len, out, &produced);
    if (rc == Z_DATA_ERROR && !st->deflate_raw) {
        g_byte_array_set_size(out, mark);
        st->deflate_raw = TRUE;
        if (!stage_zlib_init(st, -15)) return FALSE;
        rc = zlib_run(dec, st, st->deflate_probe->data, st->deflate_probe->len,
                      out, &produced);
    }
    if (rc != Z_OK) return FALSE;
    if (produced || st->deflate_probe->len > NS_DEFLATE_PROBE_BYTES) {
        st->deflate_settled = TRUE;
        g_byte_array_free(st->deflate_probe, TRUE);
        st->deflate_probe = NULL;
    }
    return TRUE;
}

static gboolean
stage_feed(ns_content_decoder *dec, ns_decode_stage *st, const guint8 *data,
           gsize len, GByteArray *out)
{
    if (st->done || len == 0) return TRUE;
    gsize produced = 0;
    switch (st->coding) {
    case NS_CODING_GZIP:
        return zlib_run(dec, st, data, len, out, &produced) == Z_OK;
    case NS_CODING_DEFLATE:
        return stage_feed_deflate(dec, st, data, len, out);
    case NS_CODING_BR: {
#ifdef NS_HAVE_BROTLI_DEC
        const uint8_t *in = data;
        size_t avail_in = len;
        for (;;) {
            guint8 buf[NS_DECODE_CHUNK];
            uint8_t *next_out = buf;
            size_t avail_out = sizeof buf;
            BrotliDecoderResult r = BrotliDecoderDecompressStream(
                st->br, &avail_in, &in, &avail_out, &next_out, NULL);
            if (sizeof buf - avail_out &&
                !append_capped(dec, out, buf, sizeof buf - avail_out))
                return FALSE;
            if (r == BROTLI_DECODER_RESULT_ERROR) return FALSE;
            if (r == BROTLI_DECODER_RESULT_SUCCESS) { st->done = TRUE; return TRUE; }
            if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) return TRUE;
        }
#else
        return FALSE;
#endif
    }
    case NS_CODING_ZSTD: {
#ifdef NS_HAVE_ZSTD
        ZSTD_inBuffer in = { data, len, 0 };
        while (in.pos < in.size) {
            guint8 buf[NS_DECODE_CHUNK];
            ZSTD_outBuffer o = { buf, sizeof buf, 0 };
            size_t rc = ZSTD_decompressStream(st->zstd, &o, &in);
            if (ZSTD_isError(rc)) return FALSE;
            if (o.pos && !append_capped(dec, out, buf, o.pos)) return FALSE;
        }
        return TRUE;
#else
        return FALSE;
#endif
    }
    }
    return FALSE;
}

ns_content_decoder *
ns_content_decoder_new(const char *content_encoding)
{
    if (!content_encoding || !*content_encoding) return NULL;
    ns_coding listed[NS_DECODE_MAX_STAGES];
    int n = 0;
    const char *p = content_encoding;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        ns_coding c;
        if (end > start && coding_from_token(start, (gsize)(end - start), &c)) {
            if (n == NS_DECODE_MAX_STAGES) return NULL;
            listed[n++] = c;
        }
    }
    if (n == 0) return NULL;
    ns_content_decoder *dec = g_new0(ns_content_decoder, 1);
    for (int i = 0; i < n; i++) {
        if (!stage_init(&dec->stages[i], listed[n - 1 - i])) {
            dec->n_stages = i + 1;
            ns_content_decoder_free(dec);
            return NULL;
        }
        dec->n_stages = i + 1;
    }
    return dec;
}

gboolean
ns_content_decoder_feed(ns_content_decoder *dec, const guint8 *data, gsize len,
                        gsize limit, GByteArray *out)
{
    dec->limit = limit;
    GByteArray *in = NULL;
    const guint8 *cur = data;
    gsize cur_len = len;
    for (int i = 0; i < dec->n_stages; i++) {
        gboolean last = i == dec->n_stages - 1;
        GByteArray *dst = last ? out : g_byte_array_new();
        gboolean ok = stage_feed(dec, &dec->stages[i], cur, cur_len, dst);
        if (in) g_byte_array_free(in, TRUE);
        in = last ? NULL : dst;
        if (!ok) {
            if (in) g_byte_array_free(in, TRUE);
            return FALSE;
        }
        if (in) {
            cur = in->data;
            cur_len = in->len;
        }
    }
    return TRUE;
}

gboolean
ns_content_decoder_over_limit(const ns_content_decoder *dec)
{
    return dec && dec->over_limit;
}

void
ns_content_decoder_free(ns_content_decoder *dec)
{
    if (!dec) return;
    for (int i = 0; i < dec->n_stages; i++) stage_clear(&dec->stages[i]);
    g_free(dec);
}
