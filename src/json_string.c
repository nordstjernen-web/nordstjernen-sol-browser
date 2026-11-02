/* Nordstjernen — decoding of JSON string literals, shared by the engine, the shell and the IPC framing. */

#include "json_string.h"

#include <stdint.h>

static int
json_at_end(const char *p, const char *end)
{
    return end ? p >= end : *p == '\0';
}

static long
json_hex4(const char *p, const char *end)
{
    long v = 0;
    for (int i = 0; i < 4; i++) {
        if (json_at_end(p + i, end))
            return -1;
        int c = (unsigned char)p[i];
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
        else if (c >= 'A' && c <= 'F') c -= 'A' - 10;
        else return -1;
        v = (v << 4) | c;
    }
    return v;
}

static size_t
json_utf8_put(uint32_t cp, char *out)
{
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (unsigned char)cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (out)
        for (size_t i = 0; i < n; i++)
            out[i] = (char)b[i];
    return n;
}

static char
json_simple_escape(char e)
{
    switch (e) {
    case '"':  return '"';
    case '\\': return '\\';
    case '/':  return '/';
    case 'b':  return '\b';
    case 'f':  return '\f';
    case 'n':  return '\n';
    case 'r':  return '\r';
    case 't':  return '\t';
    default:   return 0;
    }
}

static long
json_decode_unicode_escape(const char **pp, const char *end, int strict)
{
    const char *p = *pp;
    long cp = json_hex4(p, end);
    if (cp < 0 || (cp == 0 && strict))
        return -1;
    p += 4;
    if (cp >= 0xD800 && cp <= 0xDBFF && !json_at_end(p, end) && p[0] == '\\' &&
        !json_at_end(p + 1, end) && p[1] == 'u') {
        long lo = json_hex4(p + 2, end);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
        }
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
        cp = 0xFFFD;
    *pp = p;
    return cp;
}

long
ns_json_decode_string(const char *s, const char *end, char *out,
                      const char **stop, int strict)
{
    const char *p = s;
    size_t len = 0;
    while (!json_at_end(p, end) && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c < 0x20 && strict)
            return -1;
        if (c != '\\') {
            if (out) out[len] = (char)c;
            len++;
            continue;
        }
        if (json_at_end(p, end))
            break;
        char e = *p++;
        char simple = json_simple_escape(e);
        if (simple) {
            if (out) out[len] = simple;
            len++;
            continue;
        }
        if (e == 'u') {
            long cp = json_decode_unicode_escape(&p, end, strict);
            if (cp > 0)
                len += json_utf8_put((uint32_t)cp, out ? out + len : NULL);
            else if (strict)
                return -1;
            continue;
        }
        if (strict)
            return -1;
        if (out) out[len] = e;
        len++;
    }
    if (strict && (json_at_end(p, end) || *p != '"'))
        return -1;
    if (out) out[len] = '\0';
    if (stop) *stop = p;
    return (long)len;
}
