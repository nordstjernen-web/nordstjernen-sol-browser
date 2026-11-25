/* Nordstjernen — WebGL size and bounds arithmetic, with no GL calls and no JS engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "webgl_validate.h"

#include <string.h>

#include <epoxy/gl.h>

int
ns_wgl_components(int format)
{
    switch (format) {
    case GL_RED:  case GL_RED_INTEGER:
    case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT:                       return 1;
    case GL_RG:   case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: return 2;
    case GL_RGB:  case GL_RGB_INTEGER:
    case GL_SRGB_EXT:                              return 3;
    default:                                       return 4;
    }
}

int
ns_wgl_type_bytes(int type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:                return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES:         return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT:
    default:                                            return 4;
    }
}

size_t
ns_wgl_pixel_bytes(int format, int type)
{
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        return (size_t)ns_wgl_components(format) * (size_t)ns_wgl_type_bytes(type);
    }
}

size_t
ns_wgl_tight_bytes(int w, int h, int format, int type)
{
    if (w <= 0 || h <= 0) return 0;
    size_t total;
    if (__builtin_mul_overflow((size_t)w, (size_t)h, &total) ||
        __builtin_mul_overflow(total, ns_wgl_pixel_bytes(format, type), &total))
        return SIZE_MAX;
    return total;
}

size_t
ns_wgl_transfer_bytes(const ns_wgl_pixel_store *store, int w, int h, int depth,
                      int format, int type)
{
    if (w <= 0 || h <= 0 || depth <= 0) return 0;
    size_t pix = ns_wgl_pixel_bytes(format, type);
    int align = store->alignment < 1 ? 1 : store->alignment;
    int rowlen = store->row_length < 0 ? 0 : store->row_length;
    int skiprows = store->skip_rows < 0 ? 0 : store->skip_rows;
    int skippix = store->skip_pixels < 0 ? 0 : store->skip_pixels;
    int imgh = store->image_height < 0 ? 0 : store->image_height;
    int skipimg = store->skip_images < 0 ? 0 : store->skip_images;

    size_t row, ih, full_rows, last, total;
    if (__builtin_mul_overflow(pix, (size_t)(rowlen > 0 ? rowlen : w), &row))
        return SIZE_MAX;
    size_t rem = row % (size_t)align;
    if (rem && __builtin_add_overflow(row, (size_t)align - rem, &row))
        return SIZE_MAX;
    ih = (size_t)(imgh > 0 ? imgh : h);
    if (__builtin_add_overflow((size_t)skipimg, (size_t)(depth - 1), &full_rows) ||
        __builtin_mul_overflow(ih, full_rows, &full_rows) ||
        __builtin_add_overflow(full_rows, (size_t)skiprows, &full_rows) ||
        __builtin_add_overflow(full_rows, (size_t)(h - 1), &full_rows))
        return SIZE_MAX;
    if (__builtin_add_overflow((size_t)skippix, (size_t)w, &last) ||
        __builtin_mul_overflow(last, pix, &last) ||
        __builtin_mul_overflow(row, full_rows, &total) ||
        __builtin_add_overflow(total, last, &total))
        return SIZE_MAX;
    return total;
}

bool
ns_wgl_flip_fits(int w, int h, int bpp, size_t len)
{
    size_t row, total;
    if (w <= 0 || h <= 0 || bpp <= 0) return false;
    if (__builtin_mul_overflow((size_t)w, (size_t)bpp, &row) ||
        __builtin_mul_overflow(row, (size_t)h, &total))
        return false;
    return total <= len;
}

bool
ns_wgl_flip_safe(bool flip_y, int w, int h, int format, int type,
                 size_t need, size_t len)
{
    if (!flip_y || type != GL_UNSIGNED_BYTE) return false;
    int bpp = ns_wgl_components(format);
    size_t tight;
    if (w <= 0 || h <= 0 ||
        __builtin_mul_overflow((size_t)w, (size_t)h, &tight) ||
        __builtin_mul_overflow(tight, (size_t)bpp, &tight))
        return false;
    if (need != tight) return false;
    return ns_wgl_flip_fits(w, h, bpp, len);
}

int
ns_wgl_attr_elem_bytes(unsigned type, int size)
{
    switch (type) {
    case GL_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
        return 4;
    default:
        break;
    }
    int comp;
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:                     comp = 1; break;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: comp = 2; break;
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: case GL_FIXED: comp = 4; break;
    default: return 0;
    }
    if (size < 1 || size > 4) return 0;
    return comp * size;
}

bool
ns_wgl_attrib_covers(int64_t stride, int elem_bytes, uint64_t pointer,
                     int64_t vertex_last, int64_t instances, unsigned divisor,
                     uint64_t buffer_size)
{
    if (elem_bytes <= 0 || stride < 0) return false;
    int64_t last = divisor == 0 ? vertex_last
                                : (instances - 1) / (int64_t)divisor;
    if (last < 0) return true;
    uint64_t step = stride ? (uint64_t)stride : (uint64_t)elem_bytes;
    uint64_t need;
    if (__builtin_mul_overflow(step, (uint64_t)last, &need) ||
        __builtin_add_overflow(need, pointer, &need) ||
        __builtin_add_overflow(need, (uint64_t)elem_bytes, &need))
        return false;
    return need <= buffer_size;
}

int
ns_wgl_index_bytes(unsigned type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:  return 1;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_UNSIGNED_INT:   return 4;
    default:                return 0;
    }
}

bool
ns_wgl_index_span_fits(int64_t count, int index_bytes, int64_t offset,
                       uint64_t buffer_size)
{
    if (count < 0 || offset < 0 || index_bytes <= 0 || buffer_size == 0)
        return false;
    uint64_t span;
    if (__builtin_mul_overflow((uint64_t)count, (uint64_t)index_bytes, &span) ||
        __builtin_add_overflow(span, (uint64_t)offset, &span))
        return false;
    return span <= buffer_size;
}

bool
ns_wgl_elem_max_index(const uint8_t *data, size_t len, int64_t offset,
                      int64_t count, int index_bytes, bool skip_restart,
                      uint64_t *out_max)
{
    if (!data || count <= 0 || offset < 0 ||
        (index_bytes != 1 && index_bytes != 2 && index_bytes != 4))
        return false;
    uint64_t span;
    if (__builtin_mul_overflow((uint64_t)count, (uint64_t)index_bytes, &span) ||
        __builtin_add_overflow(span, (uint64_t)offset, &span) || span > len)
        return false;
    uint64_t restart = index_bytes == 1 ? 0xFFu
                     : index_bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    const uint8_t *p = data + offset;
    uint64_t highest = 0;
    bool any = false;
    for (int64_t i = 0; i < count; i++) {
        uint64_t v;
        if (index_bytes == 1) {
            v = p[i];
        } else if (index_bytes == 2) {
            uint16_t t;
            memcpy(&t, p + (size_t)i * 2, 2);
            v = t;
        } else {
            uint32_t t;
            memcpy(&t, p + (size_t)i * 4, 4);
            v = t;
        }
        if (skip_restart && v == restart) continue;
        if (!any || v > highest) { highest = v; any = true; }
    }
    *out_max = any ? highest : 0;
    return true;
}

uint64_t
ns_wgl_uniform_range_available(uint64_t buffer_size, int64_t start, int64_t size)
{
    if (start < 0 || size < 0) return 0;
    uint64_t available = (uint64_t)start >= buffer_size
                       ? 0 : buffer_size - (uint64_t)start;
    if (size > 0 && (uint64_t)size < available) available = (uint64_t)size;
    return available;
}
