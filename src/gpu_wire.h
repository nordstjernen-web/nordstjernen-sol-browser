/* Nordstjernen — framing of GPU commands between a renderer and a GPU process, and its validated decoding.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#ifndef NS_GPU_WIRE_H
#define NS_GPU_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_GPU_WIRE_ALIGNMENT   16u
#define NS_GPU_WIRE_HEADER_SIZE 16u
#define NS_GPU_WIRE_MAX_PAYLOAD (64u * 1024u * 1024u)

typedef struct {
    uint32_t offset;
    uint32_t size;
} ns_gpu_wire_span;

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    size_t used;
} ns_gpu_wire_writer;

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t cursor;
    bool failed;
} ns_gpu_wire_reader;

typedef struct {
    uint16_t type;
    const uint8_t *payload;
    size_t payload_size;
} ns_gpu_wire_record;

size_t ns_gpu_wire_data_offset(size_t struct_size);
bool ns_gpu_wire_record_size(size_t struct_size, size_t data_size, size_t *out);
ns_gpu_wire_span ns_gpu_wire_span_following(size_t struct_size, size_t data_size);

void ns_gpu_wire_writer_init(ns_gpu_wire_writer *writer, uint8_t *bytes, size_t capacity);
bool ns_gpu_wire_write(ns_gpu_wire_writer *writer, uint16_t type, const void *command,
                       size_t struct_size, const void *data, size_t data_size);

void ns_gpu_wire_reader_init(ns_gpu_wire_reader *reader, const uint8_t *bytes, size_t len);
bool ns_gpu_wire_next(ns_gpu_wire_reader *reader, ns_gpu_wire_record *out);

bool ns_gpu_wire_read_struct(const ns_gpu_wire_record *record, void *out, size_t struct_size);
bool ns_gpu_wire_span_bytes(const ns_gpu_wire_record *record, ns_gpu_wire_span span,
                            const uint8_t **out);
bool ns_gpu_wire_span_string(const ns_gpu_wire_record *record, ns_gpu_wire_span span,
                             const char **out);
bool ns_gpu_wire_span_elements(const ns_gpu_wire_record *record, ns_gpu_wire_span span,
                               size_t element_size, size_t count, const void **out);
bool ns_gpu_wire_bool(uint8_t value, bool *out);

#endif
