/* Nordstjernen — WebGL size and bounds arithmetic, with no GL calls and no JS engine.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_WEBGL_VALIDATE_H
#define NS_WEBGL_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_WEBGL_MAX_ALLOC (1024u * 1024u * 1024u)

typedef struct {
    int alignment;
    int row_length;
    int skip_rows;
    int skip_pixels;
    int image_height;
    int skip_images;
} ns_wgl_pixel_store;

int ns_wgl_components(int format);
int ns_wgl_type_bytes(int type);
size_t ns_wgl_pixel_bytes(int format, int type);
size_t ns_wgl_tight_bytes(int w, int h, int format, int type);
size_t ns_wgl_transfer_bytes(const ns_wgl_pixel_store *store, int w, int h,
                             int depth, int format, int type);
bool ns_wgl_flip_fits(int w, int h, int bpp, size_t len);
bool ns_wgl_flip_safe(bool flip_y, int w, int h, int format, int type,
                      size_t need, size_t len);

int ns_wgl_attr_elem_bytes(unsigned type, int size);
bool ns_wgl_attrib_covers(int64_t stride, int elem_bytes, uint64_t pointer,
                          int64_t vertex_last, int64_t instances,
                          unsigned divisor, uint64_t buffer_size);

int ns_wgl_index_bytes(unsigned type);
bool ns_wgl_index_span_fits(int64_t count, int index_bytes, int64_t offset,
                            uint64_t buffer_size);
bool ns_wgl_elem_max_index(const uint8_t *data, size_t len, int64_t offset,
                           int64_t count, int index_bytes, bool skip_restart,
                           uint64_t *out_max);

uint64_t ns_wgl_uniform_range_available(uint64_t buffer_size, int64_t start,
                                        int64_t size);

#endif
