/* Nordstjernen — framing of GPU commands between a renderer and a GPU process, and its validated decoding.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0 OR GPL-3.0-or-later
 */

#include "gpu_wire.h"

#include <string.h>

static bool
align_up(size_t value, size_t *out)
{
    size_t rounded;
    if (__builtin_add_overflow(value, (size_t)NS_GPU_WIRE_ALIGNMENT - 1, &rounded))
        return false;
    *out = rounded & ~((size_t)NS_GPU_WIRE_ALIGNMENT - 1);
    return true;
}

static bool
payload_size_of(size_t struct_size, size_t data_size, size_t *out)
{
    if (data_size == 0) {
        *out = struct_size;
        return true;
    }
    size_t data_offset;
    return align_up(struct_size, &data_offset) &&
           !__builtin_add_overflow(data_offset, data_size, out);
}

size_t
ns_gpu_wire_data_offset(size_t struct_size)
{
    size_t offset;
    return align_up(struct_size, &offset) ? offset : SIZE_MAX;
}

bool
ns_gpu_wire_record_size(size_t struct_size, size_t data_size, size_t *out)
{
    size_t payload_size, total;
    if (struct_size == 0 || !payload_size_of(struct_size, data_size, &payload_size) ||
        payload_size > NS_GPU_WIRE_MAX_PAYLOAD ||
        __builtin_add_overflow(payload_size, (size_t)NS_GPU_WIRE_HEADER_SIZE, &total))
        return false;
    return align_up(total, out);
}

ns_gpu_wire_span
ns_gpu_wire_span_following(size_t struct_size, size_t data_size)
{
    ns_gpu_wire_span span = { 0, 0 };
    size_t offset;
    if (data_size == 0 || data_size > UINT32_MAX || !align_up(struct_size, &offset) ||
        offset > UINT32_MAX)
        return span;
    span.offset = (uint32_t)offset;
    span.size = (uint32_t)data_size;
    return span;
}

void
ns_gpu_wire_writer_init(ns_gpu_wire_writer *writer, uint8_t *bytes, size_t capacity)
{
    writer->bytes = bytes;
    writer->capacity = bytes ? capacity : 0;
    writer->used = 0;
}

bool
ns_gpu_wire_write(ns_gpu_wire_writer *writer, uint16_t type, const void *command,
                  size_t struct_size, const void *data, size_t data_size)
{
    size_t record_size, payload_size;
    if (!command || (data_size && !data) ||
        !ns_gpu_wire_record_size(struct_size, data_size, &record_size) ||
        !payload_size_of(struct_size, data_size, &payload_size) ||
        record_size > writer->capacity - writer->used)
        return false;
    uint8_t *record = writer->bytes + writer->used;
    uint32_t payload_size_field = (uint32_t)payload_size;
    memset(record, 0, record_size);
    memcpy(record, &type, sizeof type);
    memcpy(record + 4, &payload_size_field, sizeof payload_size_field);
    memcpy(record + NS_GPU_WIRE_HEADER_SIZE, command, struct_size);
    if (data_size)
        memcpy(record + NS_GPU_WIRE_HEADER_SIZE + ns_gpu_wire_data_offset(struct_size), data,
               data_size);
    writer->used += record_size;
    return true;
}

void
ns_gpu_wire_reader_init(ns_gpu_wire_reader *reader, const uint8_t *bytes, size_t len)
{
    reader->bytes = bytes;
    reader->len = bytes ? len : 0;
    reader->cursor = 0;
    reader->failed = false;
}

bool
ns_gpu_wire_next(ns_gpu_wire_reader *reader, ns_gpu_wire_record *out)
{
    if (reader->failed || reader->cursor == reader->len)
        return false;
    size_t remaining = reader->len - reader->cursor;
    const uint8_t *record = reader->bytes + reader->cursor;
    uint16_t type, reserved;
    uint32_t payload_size;
    uint64_t reserved_tail;
    size_t record_size;
    if (remaining < NS_GPU_WIRE_HEADER_SIZE)
        goto malformed;
    memcpy(&type, record, sizeof type);
    memcpy(&reserved, record + 2, sizeof reserved);
    memcpy(&payload_size, record + 4, sizeof payload_size);
    memcpy(&reserved_tail, record + 8, sizeof reserved_tail);
    if (reserved != 0 || reserved_tail != 0 || payload_size == 0 ||
        payload_size > NS_GPU_WIRE_MAX_PAYLOAD ||
        !align_up((size_t)NS_GPU_WIRE_HEADER_SIZE + payload_size, &record_size) ||
        record_size > remaining)
        goto malformed;
    out->type = type;
    out->payload = record + NS_GPU_WIRE_HEADER_SIZE;
    out->payload_size = payload_size;
    reader->cursor += record_size;
    return true;

malformed:
    reader->failed = true;
    return false;
}

bool
ns_gpu_wire_read_struct(const ns_gpu_wire_record *record, void *out, size_t struct_size)
{
    if (struct_size == 0 || record->payload_size < struct_size)
        return false;
    memcpy(out, record->payload, struct_size);
    return true;
}

bool
ns_gpu_wire_span_bytes(const ns_gpu_wire_record *record, ns_gpu_wire_span span,
                       const uint8_t **out)
{
    uint64_t end = (uint64_t)span.offset + (uint64_t)span.size;
    if (end > record->payload_size)
        return false;
    *out = span.size ? record->payload + span.offset : NULL;
    return true;
}

bool
ns_gpu_wire_span_string(const ns_gpu_wire_record *record, ns_gpu_wire_span span,
                        const char **out)
{
    const uint8_t *bytes;
    if (span.size == 0 || !ns_gpu_wire_span_bytes(record, span, &bytes) ||
        bytes[span.size - 1] != 0)
        return false;
    *out = (const char *)bytes;
    return true;
}

static size_t
alignment_of_element(size_t element_size)
{
    if (element_size & (element_size - 1))
        return 1;
    return element_size > NS_GPU_WIRE_ALIGNMENT ? NS_GPU_WIRE_ALIGNMENT : element_size;
}

bool
ns_gpu_wire_span_elements(const ns_gpu_wire_record *record, ns_gpu_wire_span span,
                          size_t element_size, size_t count, const void **out)
{
    size_t expected;
    const uint8_t *bytes;
    if (element_size == 0 || __builtin_mul_overflow(element_size, count, &expected) ||
        expected != span.size || !ns_gpu_wire_span_bytes(record, span, &bytes))
        return false;
    if (bytes && (uintptr_t)bytes % alignment_of_element(element_size) != 0)
        return false;
    *out = bytes;
    return true;
}

bool
ns_gpu_wire_bool(uint8_t value, bool *out)
{
    if (value > 1)
        return false;
    *out = value == 1;
    return true;
}
