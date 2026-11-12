/* Nordstjernen — minimal MP4 / fragmented-MP4 demuxer for builds without libav.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "mp4.h"

#include <stdlib.h>
#include <string.h>

#define NS_MP4_MAX_SAMPLES 2000000u
#define NS_MP4_MAX_RUN_SAMPLES 65536u
#define NS_MP4_MAX_TRACKS  16

#define FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

typedef struct {
    const uint8_t *p;
    size_t         len;
} ns_span;

typedef struct {
    ns_span stsz, stco, co64, stsc, stts, ctts, stss;
} ns_stbl;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

static int
box_at(const uint8_t *p, size_t len, size_t off, uint32_t *type, size_t *hdr,
       uint64_t *size)
{
    if (len < 8 || off > len - 8) return 0;
    uint64_t sz = rd32(p + off);
    *type = rd32(p + off + 4);
    *hdr = 8;
    if (sz == 1) {
        if (len - off < 16) return 0;
        sz = rd64(p + off + 8);
        *hdr = 16;
    } else if (sz == 0) {
        sz = len - off;
    }
    if (sz < *hdr) return -1;
    *size = sz;
    return 1;
}

static int
child_next(ns_span parent, size_t *off, uint32_t *type, ns_span *body)
{
    size_t hdr;
    uint64_t size;
    if (box_at(parent.p, parent.len, *off, type, &hdr, &size) != 1) return 0;
    if (size > parent.len - *off) return 0;
    body->p = parent.p + *off + hdr;
    body->len = (size_t)size - hdr;
    *off += (size_t)size;
    return 1;
}

int
ns_mp4_bytes_are_mp4(const uint8_t *data, size_t len)
{
    if (!data || len < 12) return 0;
    uint32_t t = rd32(data + 4);
    return t == FOURCC('f', 't', 'y', 'p') || t == FOURCC('s', 't', 'y', 'p') ||
           t == FOURCC('m', 'o', 'o', 'v') || t == FOURCC('m', 'o', 'o', 'f');
}

void
ns_mp4_init(ns_mp4 *m)
{
    memset(m, 0, sizeof *m);
}

void
ns_mp4_clear(ns_mp4 *m)
{
    for (int i = 0; i < m->ntracks; i++) {
        free(m->tracks[i].samples);
        free(m->tracks[i].config);
    }
    free(m->tracks);
    memset(m, 0, sizeof *m);
}

static ns_mp4_track *
track_by_id(ns_mp4 *m, uint32_t id)
{
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i].id == id) return &m->tracks[i];
    return NULL;
}

ns_mp4_track *
ns_mp4_track_of(ns_mp4 *m, char kind)
{
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i].kind == kind && m->tracks[i].config) return &m->tracks[i];
    return NULL;
}

static ns_mp4_sample *
track_push(ns_mp4_track *t)
{
    if (t->count >= NS_MP4_MAX_SAMPLES) return NULL;
    if (t->count == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        ns_mp4_sample *grown = realloc(t->samples, cap * sizeof *grown);
        if (!grown) return NULL;
        t->samples = grown;
        t->cap = cap;
    }
    return &t->samples[t->count++];
}

static void
parse_avcc_entry(ns_mp4_track *t, ns_span body)
{
    if (body.len < 78) return;
    t->width = rd16(body.p + 24);
    t->height = rd16(body.p + 26);
    ns_span kids = { body.p + 78, body.len - 78 };
    size_t off = 0;
    uint32_t type;
    ns_span b;
    while (child_next(kids, &off, &type, &b)) {
        if (type == FOURCC('a', 'v', 'c', 'C') && b.len >= 7 && !t->config) {
            t->config = malloc(b.len);
            if (t->config) {
                memcpy(t->config, b.p, b.len);
                t->config_len = b.len;
            }
        }
    }
}

static size_t
desc_len(const uint8_t *p, size_t len, size_t *pos)
{
    size_t n = 0;
    for (int i = 0; i < 4 && *pos < len; i++) {
        uint8_t c = p[(*pos)++];
        n = (n << 7) | (c & 0x7f);
        if (!(c & 0x80)) break;
    }
    return n;
}

static void
parse_esds(ns_mp4_track *t, ns_span b)
{
    size_t pos = 4;
    const uint8_t *p = b.p;
    size_t len = b.len;
    if (pos >= len || p[pos++] != 0x03) return;
    desc_len(p, len, &pos);
    if (pos + 3 > len) return;
    pos += 2;
    uint8_t flags = p[pos++];
    if (flags & 0x80) pos += 2;
    if (flags & 0x40) {
        if (pos >= len) return;
        pos += 1 + p[pos];
    }
    if (flags & 0x20) pos += 2;
    if (pos >= len || p[pos++] != 0x04) return;
    desc_len(p, len, &pos);
    pos += 13;
    if (pos >= len || p[pos++] != 0x05) return;
    size_t n = desc_len(p, len, &pos);
    if (n == 0 || n > 64 || pos + n > len || t->config) return;
    t->config = malloc(n);
    if (!t->config) return;
    memcpy(t->config, p + pos, n);
    t->config_len = n;
}

static void
parse_mp4a_entry(ns_mp4_track *t, ns_span body)
{
    if (body.len < 28) return;
    uint16_t version = rd16(body.p + 8);
    t->channels = rd16(body.p + 16);
    t->rate = (int)(rd32(body.p + 24) >> 16);
    size_t skip = version == 1 ? 44 : version == 2 ? 64 : 28;
    if (body.len < skip) return;
    ns_span kids = { body.p + skip, body.len - skip };
    size_t off = 0;
    uint32_t type;
    ns_span b;
    while (child_next(kids, &off, &type, &b))
        if (type == FOURCC('e', 's', 'd', 's')) parse_esds(t, b);
}

static void
parse_stsd(ns_mp4_track *t, ns_span b)
{
    if (b.len < 8) return;
    ns_span kids = { b.p + 8, b.len - 8 };
    size_t off = 0;
    uint32_t type;
    ns_span e;
    if (!child_next(kids, &off, &type, &e)) return;
    t->codec[0] = (char)(type >> 24);
    t->codec[1] = (char)(type >> 16);
    t->codec[2] = (char)(type >> 8);
    t->codec[3] = (char)type;
    t->codec[4] = '\0';
    if (type == FOURCC('a', 'v', 'c', '1') || type == FOURCC('a', 'v', 'c', '3'))
        parse_avcc_entry(t, e);
    else if (type == FOURCC('m', 'p', '4', 'a'))
        parse_mp4a_entry(t, e);
}

static void
parse_stbl(ns_mp4_track *t, ns_span stbl, ns_stbl *tab)
{
    size_t off = 0;
    uint32_t type;
    ns_span b;
    while (child_next(stbl, &off, &type, &b)) {
        switch (type) {
        case FOURCC('s', 't', 's', 'd'): parse_stsd(t, b); break;
        case FOURCC('s', 't', 's', 'z'): tab->stsz = b; break;
        case FOURCC('s', 't', 'c', 'o'): tab->stco = b; break;
        case FOURCC('c', 'o', '6', '4'): tab->co64 = b; break;
        case FOURCC('s', 't', 's', 'c'): tab->stsc = b; break;
        case FOURCC('s', 't', 't', 's'): tab->stts = b; break;
        case FOURCC('c', 't', 't', 's'): tab->ctts = b; break;
        case FOURCC('s', 't', 's', 's'): tab->stss = b; break;
        default: break;
        }
    }
}

static uint32_t
table_count(ns_span b, size_t entry)
{
    if (b.len < 8) return 0;
    uint32_t n = rd32(b.p + 4);
    size_t room = (b.len - 8) / entry;
    return n > room ? (uint32_t)room : n;
}

static void
build_samples(ns_mp4_track *t, const ns_stbl *tab, uint32_t budget)
{
    if (tab->stsz.len < 12) return;
    uint32_t fixed = rd32(tab->stsz.p + 4);
    uint32_t n = rd32(tab->stsz.p + 8);
    if (!fixed && n > (tab->stsz.len - 12) / 4) n = (uint32_t)((tab->stsz.len - 12) / 4);
    if (n > budget) n = budget;
    if (n == 0) return;
    int wide = tab->co64.len > 0;
    uint32_t nchunks = wide ? table_count(tab->co64, 8) : table_count(tab->stco, 4);
    uint32_t nstsc = table_count(tab->stsc, 12);
    if (!nchunks || !nstsc) return;

    t->samples = calloc(n, sizeof *t->samples);
    if (!t->samples) return;
    t->cap = n;

    uint32_t s = 0;
    for (uint32_t c = 0, e = 0; c < nchunks && s < n; c++) {
        while (e + 1 < nstsc && rd32(tab->stsc.p + 8 + (size_t)(e + 1) * 12) <= c + 1) e++;
        uint32_t per = rd32(tab->stsc.p + 8 + (size_t)e * 12 + 4);
        uint64_t off = wide ? rd64(tab->co64.p + 8 + (size_t)c * 8)
                            : rd32(tab->stco.p + 8 + (size_t)c * 4);
        for (uint32_t k = 0; k < per && s < n; k++, s++) {
            uint32_t sz = fixed ? fixed : rd32(tab->stsz.p + 12 + (size_t)s * 4);
            t->samples[s].offset = off;
            t->samples[s].size = sz;
            t->samples[s].key = tab->stss.len == 0;
            off += sz;
        }
    }
    t->count = s;

    int64_t dts = 0;
    uint32_t nstts = table_count(tab->stts, 8);
    for (uint32_t e = 0, i = 0; e < nstts && i < t->count; e++) {
        uint32_t cnt = rd32(tab->stts.p + 8 + (size_t)e * 8);
        uint32_t delta = rd32(tab->stts.p + 12 + (size_t)e * 8);
        for (uint32_t k = 0; k < cnt && i < t->count; k++, i++) {
            t->samples[i].dts = dts;
            t->samples[i].pts = dts;
            t->samples[i].duration = delta;
            dts += delta;
        }
    }
    t->next_dts = dts;
    uint32_t nctts = table_count(tab->ctts, 8);
    for (uint32_t e = 0, i = 0; e < nctts && i < t->count; e++) {
        uint32_t cnt = rd32(tab->ctts.p + 8 + (size_t)e * 8);
        int32_t ofs = (int32_t)rd32(tab->ctts.p + 12 + (size_t)e * 8);
        for (uint32_t k = 0; k < cnt && i < t->count; k++, i++)
            t->samples[i].pts = t->samples[i].dts + ofs;
    }
    uint32_t nstss = table_count(tab->stss, 4);
    for (uint32_t e = 0; e < nstss; e++) {
        uint32_t idx = rd32(tab->stss.p + 8 + (size_t)e * 4);
        if (idx >= 1 && idx <= t->count) t->samples[idx - 1].key = 1;
    }
}

static void
parse_elst(ns_mp4_track *t, ns_span b)
{
    if (b.len < 8) return;
    int v1 = b.p[0] == 1;
    size_t entry = v1 ? 20 : 12;
    uint32_t n = table_count(b, entry);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = b.p + 8 + (size_t)i * entry;
        int64_t media_time = v1 ? (int64_t)rd64(e + 8) : (int64_t)(int32_t)rd32(e + 4);
        if (media_time >= 0) {
            t->pts_shift = media_time;
            return;
        }
    }
}

static void
parse_trak(ns_mp4 *m, ns_span trak)
{
    if (m->ntracks >= NS_MP4_MAX_TRACKS) return;
    ns_mp4_track t;
    memset(&t, 0, sizeof t);
    ns_stbl tab;
    memset(&tab, 0, sizeof tab);
    size_t off = 0;
    uint32_t type;
    ns_span b;
    while (child_next(trak, &off, &type, &b)) {
        if (type == FOURCC('t', 'k', 'h', 'd') && b.len >= 24) {
            t.id = rd32(b.p + (b.p[0] == 1 ? 20 : 12));
        } else if (type == FOURCC('e', 'd', 't', 's')) {
            size_t eo = 0;
            ns_span eb;
            uint32_t et;
            while (child_next(b, &eo, &et, &eb))
                if (et == FOURCC('e', 'l', 's', 't')) parse_elst(&t, eb);
        } else if (type == FOURCC('m', 'd', 'i', 'a')) {
            size_t mo = 0;
            ns_span mb;
            uint32_t mt;
            while (child_next(b, &mo, &mt, &mb)) {
                if (mt == FOURCC('m', 'd', 'h', 'd') && mb.len >= 24) {
                    t.timescale = rd32(mb.p + (mb.p[0] == 1 ? 20 : 12));
                } else if (mt == FOURCC('h', 'd', 'l', 'r') && mb.len >= 12) {
                    uint32_t h = rd32(mb.p + 8);
                    t.kind = h == FOURCC('v', 'i', 'd', 'e') ? 'v'
                           : h == FOURCC('s', 'o', 'u', 'n') ? 'a' : 0;
                } else if (mt == FOURCC('m', 'i', 'n', 'f')) {
                    size_t io = 0;
                    ns_span ib;
                    uint32_t it;
                    while (child_next(mb, &io, &it, &ib))
                        if (it == FOURCC('s', 't', 'b', 'l')) parse_stbl(&t, ib, &tab);
                }
            }
        }
    }
    if (!t.kind || !t.timescale) {
        free(t.config);
        return;
    }
    uint32_t budget = NS_MP4_MAX_SAMPLES;
    for (int i = 0; i < m->ntracks; i++)
        budget -= m->tracks[i].count < budget
            ? (uint32_t)m->tracks[i].count : budget;
    build_samples(&t, &tab, budget);
    ns_mp4_track *grown = realloc(m->tracks, (size_t)(m->ntracks + 1) * sizeof *grown);
    if (!grown) {
        free(t.config);
        free(t.samples);
        return;
    }
    m->tracks = grown;
    m->tracks[m->ntracks++] = t;
}

static void
parse_moov(ns_mp4 *m, ns_span moov)
{
    size_t off = 0;
    uint32_t type;
    ns_span b;
    while (child_next(moov, &off, &type, &b)) {
        if (type == FOURCC('m', 'v', 'h', 'd') && b.len >= 24 &&
            (b.p[0] != 1 || b.len >= 32)) {
            int v1 = b.p[0] == 1;
            m->movie_timescale = rd32(b.p + (v1 ? 20 : 12));
            m->movie_duration = v1 ? rd64(b.p + 24) : rd32(b.p + 16);
        } else if (type == FOURCC('t', 'r', 'a', 'k')) {
            parse_trak(m, b);
        } else if (type == FOURCC('m', 'v', 'e', 'x')) {
            size_t xo = 0;
            ns_span xb;
            uint32_t xt;
            while (child_next(b, &xo, &xt, &xb)) {
                if (xt == FOURCC('t', 'r', 'e', 'x') && xb.len >= 24) {
                    ns_mp4_track *t = track_by_id(m, rd32(xb.p + 4));
                    if (t) {
                        t->trex_duration = rd32(xb.p + 12);
                        t->trex_size = rd32(xb.p + 16);
                        t->trex_flags = rd32(xb.p + 20);
                    }
                } else if (xt == FOURCC('m', 'e', 'h', 'd') && xb.len >= 8) {
                    m->fragment_duration = xb.p[0] == 1 && xb.len >= 12
                        ? rd64(xb.p + 4) : rd32(xb.p + 4);
                }
            }
        }
    }
    m->have_moov = 1;
}

static void
parse_trun(ns_mp4_track *t, ns_span b, uint64_t base, uint64_t *cursor,
           int64_t *dts, uint32_t def_dur, uint32_t def_size, uint32_t def_flags,
           int have_def_flags)
{
    if (b.len < 8) return;
    uint32_t flags = rd32(b.p) & 0xffffff;
    int v1 = b.p[0] == 1;
    uint32_t n = rd32(b.p + 4);
    size_t pos = 8;
    if (flags & 0x1) {
        if (pos + 4 > b.len) return;
        *cursor = base + (uint64_t)(int64_t)(int32_t)rd32(b.p + pos);
        pos += 4;
    }
    uint32_t first_flags = 0;
    int have_first = 0;
    if (flags & 0x4) {
        if (pos + 4 > b.len) return;
        first_flags = rd32(b.p + pos);
        have_first = 1;
        pos += 4;
    }
    size_t per = (size_t)(!!(flags & 0x100) + !!(flags & 0x200) +
                          !!(flags & 0x400) + !!(flags & 0x800)) * 4;
    if (per && n > (b.len - pos) / per) n = (uint32_t)((b.len - pos) / per);
    if (!(flags & 0x200) && def_size == 0) return;
    if (!per && n > NS_MP4_MAX_RUN_SAMPLES) n = NS_MP4_MAX_RUN_SAMPLES;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dur = def_dur, size = def_size, sflags = def_flags;
        int have_flags = have_def_flags;
        int32_t cto = 0;
        if (flags & 0x100) { dur = rd32(b.p + pos); pos += 4; }
        if (flags & 0x200) { size = rd32(b.p + pos); pos += 4; }
        if (flags & 0x400) { sflags = rd32(b.p + pos); have_flags = 1; pos += 4; }
        else if (i == 0 && have_first) { sflags = first_flags; have_flags = 1; }
        if (flags & 0x800) {
            uint32_t raw = rd32(b.p + pos);
            cto = v1 ? (int32_t)raw : (int32_t)(raw & 0x7fffffffu);
            pos += 4;
        }
        ns_mp4_sample *s = track_push(t);
        if (!s) return;
        s->offset = *cursor;
        s->size = size;
        s->dts = *dts;
        s->pts = *dts + cto;
        s->duration = dur;
        s->key = t->kind == 'a' ? 1
               : have_flags ? !(sflags & 0x10000u) : i == 0;
        *cursor += size;
        *dts += dur;
    }
}

static void
parse_moof(ns_mp4 *m, ns_span moof, uint64_t moof_start)
{
    size_t off = 0;
    uint32_t type;
    ns_span traf;
    while (child_next(moof, &off, &type, &traf)) {
        if (type != FOURCC('t', 'r', 'a', 'f')) continue;
        ns_mp4_track *t = NULL;
        uint64_t base = moof_start, cursor = moof_start;
        uint32_t def_dur = 0, def_size = 0, def_flags = 0;
        int have_def_flags = 0, have_tfdt = 0;
        int64_t dts = 0;
        size_t to = 0;
        uint32_t tt;
        ns_span tb;
        while (child_next(traf, &to, &tt, &tb)) {
            if (tt == FOURCC('t', 'f', 'h', 'd') && tb.len >= 8) {
                uint32_t flags = rd32(tb.p) & 0xffffff;
                t = track_by_id(m, rd32(tb.p + 4));
                if (!t) break;
                def_dur = t->trex_duration;
                def_size = t->trex_size;
                def_flags = t->trex_flags;
                have_def_flags = 1;
                size_t pos = 8;
                if ((flags & 0x1) && pos + 8 <= tb.len) { base = rd64(tb.p + pos); pos += 8; }
                if (flags & 0x2) pos += 4;
                if ((flags & 0x8) && pos + 4 <= tb.len) { def_dur = rd32(tb.p + pos); pos += 4; }
                if ((flags & 0x10) && pos + 4 <= tb.len) { def_size = rd32(tb.p + pos); pos += 4; }
                if ((flags & 0x20) && pos + 4 <= tb.len) { def_flags = rd32(tb.p + pos); pos += 4; }
                cursor = base;
                dts = t->next_dts;
            } else if (tt == FOURCC('t', 'f', 'd', 't') && t && tb.len >= 8) {
                dts = tb.p[0] == 1 && tb.len >= 12 ? (int64_t)rd64(tb.p + 4)
                                                   : (int64_t)rd32(tb.p + 4);
                have_tfdt = 1;
            } else if (tt == FOURCC('t', 'r', 'u', 'n') && t) {
                parse_trun(t, tb, base, &cursor, &dts, def_dur, def_size,
                           def_flags, have_def_flags);
            }
        }
        (void)have_tfdt;
        if (t) t->next_dts = dts;
    }
}

int
ns_mp4_parse(ns_mp4 *m, const uint8_t *data, size_t len)
{
    while (m->pos < len) {
        uint32_t type;
        size_t hdr;
        uint64_t size;
        int r = box_at(data, len, m->pos, &type, &hdr, &size);
        if (r < 0) return -1;
        if (r == 0 || size > len - m->pos) break;
        ns_span body = { data + m->pos + hdr, (size_t)size - hdr };
        if (type == FOURCC('m', 'o', 'o', 'v') && !m->have_moov)
            parse_moov(m, body);
        else if (type == FOURCC('m', 'o', 'o', 'f') && m->have_moov)
            parse_moof(m, body, m->pos);
        m->pos += (size_t)size;
    }
    return m->have_moov;
}

double
ns_mp4_sample_time(const ns_mp4_track *t, const ns_mp4_sample *s)
{
    return t->timescale ? (double)(s->pts - t->pts_shift) / (double)t->timescale : 0.0;
}

double
ns_mp4_duration(const ns_mp4 *m)
{
    if (m->movie_timescale && m->movie_duration)
        return (double)m->movie_duration / (double)m->movie_timescale;
    if (m->movie_timescale && m->fragment_duration)
        return (double)m->fragment_duration / (double)m->movie_timescale;
    return 0.0;
}

int
ns_mp4_time_range(ns_mp4 *m, double *out_start, double *out_end)
{
    double start = 0.0, end = 0.0;
    int any = 0;
    for (int i = 0; i < m->ntracks; i++) {
        ns_mp4_track *t = &m->tracks[i];
        for (size_t k = 0; k < t->count; k++) {
            double a = ns_mp4_sample_time(t, &t->samples[k]);
            double b = a + (t->timescale ? (double)t->samples[k].duration / t->timescale : 0.0);
            if (!any || a < start) start = a;
            if (!any || b > end) end = b;
            any = 1;
        }
    }
    if (!any || end <= start) return 0;
    if (out_start) *out_start = start;
    if (out_end) *out_end = end;
    return 1;
}
