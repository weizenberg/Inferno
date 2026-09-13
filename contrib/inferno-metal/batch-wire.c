/*
 * Inferno guest-to-host Metal execution bridge.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "batch-wire.h"
#include <stdlib.h>
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static int64_t getSigned64(const uint8_t *p)
{
    uint64_t bits = get64(p);
    int64_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static bool allZero(const uint8_t *p, size_t size)
{
    while (size--) {
        if (*p++) {
            return false;
        }
    }
    return true;
}

static bool validUtf8(const uint8_t *p, size_t size)
{
    size_t i = 0;
    while (i < size) {
        uint8_t a = p[i++];
        if (a < 0x80) {
            continue;
        }
        unsigned need;
        uint32_t code;
        if (a >= 0xc2 && a <= 0xdf) {
            need = 1;
            code = a & 0x1f;
        } else if (a >= 0xe0 && a <= 0xef) {
            need = 2;
            code = a & 0x0f;
        } else if (a >= 0xf0 && a <= 0xf4) {
            need = 3;
            code = a & 7;
        } else {
            return false;
        }
        if (need > size - i) {
            return false;
        }
        for (unsigned j = 0; j < need; j++) {
            uint8_t b = p[i++];
            if ((b & 0xc0) != 0x80) {
                return false;
            }
            code = code << 6 | (b & 0x3f);
        }
        if ((need == 2 && code < 0x800) || (need == 3 && code < 0x10000) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

static bool validString(const uint8_t *p, uint32_t length, uint32_t capacity)
{
    return length < capacity && p[length] == 0 && !memchr(p, 0, length) &&
           validUtf8(p, length) &&
           allZero(p + length + 1, capacity - length - 1);
}

static bool checkedAdd(size_t *value, size_t addition)
{
    if (addition > SIZE_MAX - *value) {
        return false;
    }
    *value += addition;
    return true;
}

static bool checkedProduct(uint32_t a, uint32_t b, uint32_t c, uint64_t limit,
                           uint64_t *product)
{
    if (!a || !b || !c || a > limit || b > limit / a) {
        return false;
    }
    uint64_t ab = (uint64_t)a * b;
    if (c > limit / ab) {
        return false;
    }
    *product = ab * c;
    return true;
}

bool imtl_batch_builder_build(const ImtlBatchManifest *m, uint8_t **out,
                              size_t *out_size, uint32_t *out_images_size)
{
    if (out) {
        *out = NULL;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (out_images_size) {
        *out_images_size = 0;
    }
    if (!m || !out || !out_size || !out_images_size ||
        m->pipeline_count > INFERNO_METAL_BATCH_MAX_PIPELINES ||
        m->buffer_count > INFERNO_METAL_BATCH_MAX_BUFFERS ||
        m->dispatch_count > INFERNO_METAL_BATCH_MAX_DISPATCHES ||
        m->binding_count > INFERNO_METAL_BATCH_MAX_BINDINGS ||
        (m->pipeline_count && !m->pipelines) ||
        (m->buffer_count && !m->buffers) ||
        (m->dispatch_count && !m->dispatches) ||
        (m->binding_count && !m->bindings)) {
        return false;
    }
    bool empty = !m->pipeline_count && !m->buffer_count && !m->dispatch_count &&
                 !m->binding_count;
    if ((!empty && (!m->pipeline_count || !m->dispatch_count)) ||
        (!m->dispatch_count && m->binding_count)) {
        return false;
    }

    size_t source_size = 0, inline_size = 0, images_size = 0;
    uint32_t *source_offsets =
        calloc(m->pipeline_count ? m->pipeline_count : 1, sizeof(uint32_t));
    if (!source_offsets) {
        return false;
    }
    for (uint32_t i = 0; i < m->pipeline_count; i++) {
        const ImtlBatchPipeline *p = &m->pipelines[i];
        size_t name_size = p->function_name ? strnlen(p->function_name, 64) : 0;
        if (!p->source || !p->source_size ||
            p->source_size > INFERNO_METAL_MAX_SOURCE || !name_size ||
            name_size == 64 ||
            !validUtf8((const uint8_t *)p->source, p->source_size) ||
            !validUtf8((const uint8_t *)p->function_name, name_size)) {
            free(source_offsets);
            return false;
        }
        bool shared =
            i && p->source_size == m->pipelines[i - 1].source_size &&
            !memcmp(p->source, m->pipelines[i - 1].source, p->source_size);
        source_offsets[i] = shared ? source_offsets[i - 1] : source_size;
        if (!shared && !checkedAdd(&source_size, p->source_size)) {
            free(source_offsets);
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (p->source_size == m->pipelines[j].source_size &&
                !memcmp(p->source, m->pipelines[j].source, p->source_size) &&
                !strcmp(p->function_name, m->pipelines[j].function_name)) {
                free(source_offsets);
                return false;
            }
        }
    }
    bool referenced[INFERNO_METAL_BATCH_MAX_BUFFERS] = { false };
    for (uint32_t i = 0; i < m->buffer_count; i++) {
        if (!m->buffers[i].bytes || !m->buffers[i].length ||
            m->buffers[i].length > INFERNO_METAL_BATCH_MAX_BUFFER_LENGTH ||
            !checkedAdd(&images_size, m->buffers[i].length)) {
            free(source_offsets);
            return false;
        }
    }
    uint32_t expected_binding = 0;
    for (uint32_t i = 0; i < m->dispatch_count; i++) {
        const ImtlBatchDispatch *d = &m->dispatches[i];
        uint64_t grid_product = 0, group_product = 0;
        bool grid_valid =
            checkedProduct(d->grid_width, d->grid_height, d->grid_depth,
                           INFERNO_METAL_MAX_THREADS, &grid_product);
        bool group_valid =
            checkedProduct(d->group_width, d->group_height, d->group_depth,
                           INFERNO_METAL_MAX_THREADS, &group_product);
        if (d->pipeline_id >= m->pipeline_count ||
            (d->mode != INFERNO_METAL_BATCH_DISPATCH_THREADS &&
             d->mode != INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS) ||
            d->binding_start != expected_binding ||
            d->binding_start > m->binding_count ||
            d->binding_count > m->binding_count - d->binding_start ||
            !grid_valid || !group_valid ||
            (d->mode == INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS &&
             grid_product > INFERNO_METAL_MAX_THREADS / group_product)) {
            free(source_offsets);
            return false;
        }
        uint32_t prior_index = 0, prior_kind = 0;
        bool have_prior = false;
        for (uint32_t j = 0; j < d->binding_count; j++) {
            uint32_t bi = d->binding_start + j;
            const ImtlBatchBinding *b = &m->bindings[bi];
            bool ordered = !have_prior || b->index > prior_index ||
                           (b->index == prior_index && b->kind > prior_kind);
            bool duplicate =
                have_prior && b->index == prior_index &&
                ((b->kind <= INFERNO_METAL_BATCH_BINDING_INLINE &&
                  prior_kind <= INFERNO_METAL_BATCH_BINDING_INLINE) ||
                 b->kind == prior_kind);
            bool valid = b->index <= 30 && ordered && !duplicate;
            if (b->kind == INFERNO_METAL_BATCH_BINDING_BUFFER) {
                valid = valid && b->resource_id < m->buffer_count &&
                        !b->length && !(b->offset & 3) &&
                        b->offset < m->buffers[b->resource_id].length;
                if (valid) {
                    referenced[b->resource_id] = true;
                }
            } else if (b->kind == INFERNO_METAL_BATCH_BINDING_INLINE) {
                valid = valid && !b->resource_id && !b->offset && b->bytes &&
                        b->length && b->length <= 4096 &&
                        checkedAdd(&inline_size, b->length);
            } else if (b->kind == INFERNO_METAL_BATCH_BINDING_THREADGROUP) {
                valid = valid && !b->resource_id && !b->offset && !b->bytes &&
                        b->length && b->length <= 32768 && !(b->length & 15);
            } else {
                valid = false;
            }
            if (!valid) {
                free(source_offsets);
                return false;
            }
            prior_index = b->index;
            prior_kind = b->kind;
            have_prior = true;
        }
        expected_binding += d->binding_count;
    }
    if (expected_binding != m->binding_count ||
        source_size > INFERNO_METAL_BATCH_MAX_SOURCE ||
        inline_size > INFERNO_METAL_BATCH_MAX_INLINE ||
        images_size > INFERNO_METAL_BATCH_MAX_IMAGES) {
        free(source_offsets);
        return false;
    }
    for (uint32_t i = 0; i < m->buffer_count; i++) {
        if (!referenced[i]) {
            free(source_offsets);
            return false;
        }
    }
    size_t size = INFERNO_METAL_BATCH_HEADER_SIZE;
    if (!checkedAdd(&size, (size_t)m->pipeline_count *
                               INFERNO_METAL_BATCH_PIPELINE_RECORD_SIZE) ||
        !checkedAdd(&size, (size_t)m->buffer_count *
                               INFERNO_METAL_BATCH_BUFFER_RECORD_SIZE) ||
        !checkedAdd(&size, (size_t)m->dispatch_count *
                               INFERNO_METAL_BATCH_DISPATCH_RECORD_SIZE) ||
        !checkedAdd(&size, (size_t)m->binding_count *
                               INFERNO_METAL_BATCH_BINDING_RECORD_SIZE) ||
        !checkedAdd(&size, source_size) || !checkedAdd(&size, inline_size) ||
        !checkedAdd(&size, images_size) || size > INFERNO_METAL_MAX_BUFFER) {
        free(source_offsets);
        return false;
    }
    uint8_t *bytes = calloc(1, size);
    if (!bytes) {
        free(source_offsets);
        return false;
    }
    put32(bytes + INFERNO_METAL_BATCH_VERSION_OFFSET, INFERNO_METAL_VERSION);
    put32(bytes + INFERNO_METAL_BATCH_PIPELINE_COUNT_OFFSET, m->pipeline_count);
    put32(bytes + INFERNO_METAL_BATCH_BUFFER_COUNT_OFFSET, m->buffer_count);
    put32(bytes + INFERNO_METAL_BATCH_DISPATCH_COUNT_OFFSET, m->dispatch_count);
    put32(bytes + INFERNO_METAL_BATCH_BINDING_COUNT_OFFSET, m->binding_count);
    put32(bytes + INFERNO_METAL_BATCH_INLINE_SIZE_OFFSET, inline_size);
    put32(bytes + INFERNO_METAL_BATCH_SOURCE_SIZE_OFFSET, source_size);
    put32(bytes + INFERNO_METAL_BATCH_IMAGES_SIZE_OFFSET, images_size);
    size_t pipelines_offset = INFERNO_METAL_BATCH_HEADER_SIZE;
    size_t buffers_offset =
        pipelines_offset +
        (size_t)m->pipeline_count * INFERNO_METAL_BATCH_PIPELINE_RECORD_SIZE;
    size_t dispatches_offset =
        buffers_offset +
        (size_t)m->buffer_count * INFERNO_METAL_BATCH_BUFFER_RECORD_SIZE;
    size_t bindings_offset =
        dispatches_offset +
        (size_t)m->dispatch_count * INFERNO_METAL_BATCH_DISPATCH_RECORD_SIZE;
    size_t sources_offset =
        bindings_offset +
        (size_t)m->binding_count * INFERNO_METAL_BATCH_BINDING_RECORD_SIZE;
    size_t inline_offset = sources_offset + source_size;
    size_t images_offset = inline_offset + inline_size;
    size_t source_cursor = 0;
    for (uint32_t i = 0; i < m->pipeline_count; i++) {
        uint8_t *p = bytes + pipelines_offset +
                     (size_t)i * INFERNO_METAL_BATCH_PIPELINE_RECORD_SIZE;
        put32(p + INFERNO_METAL_BATCH_PIPELINE_SOURCE_OFFSET,
              source_offsets[i]);
        put32(p + INFERNO_METAL_BATCH_PIPELINE_SOURCE_SIZE_OFFSET,
              m->pipelines[i].source_size);
        memcpy(p + INFERNO_METAL_BATCH_PIPELINE_NAME_OFFSET,
               m->pipelines[i].function_name,
               strlen(m->pipelines[i].function_name));
        if (!i || source_offsets[i] != source_offsets[i - 1]) {
            memcpy(bytes + sources_offset + source_cursor,
                   m->pipelines[i].source, m->pipelines[i].source_size);
            source_cursor += m->pipelines[i].source_size;
        }
    }
    size_t image_cursor = 0;
    for (uint32_t i = 0; i < m->buffer_count; i++) {
        uint8_t *p = bytes + buffers_offset +
                     (size_t)i * INFERNO_METAL_BATCH_BUFFER_RECORD_SIZE;
        put32(p + INFERNO_METAL_BATCH_BUFFER_LENGTH_OFFSET,
              m->buffers[i].length);
        memcpy(bytes + images_offset + image_cursor, m->buffers[i].bytes,
               m->buffers[i].length);
        image_cursor += m->buffers[i].length;
    }
    for (uint32_t i = 0; i < m->dispatch_count; i++) {
        uint8_t *p = bytes + dispatches_offset +
                     (size_t)i * INFERNO_METAL_BATCH_DISPATCH_RECORD_SIZE;
        const ImtlBatchDispatch *d = &m->dispatches[i];
        put32(p + INFERNO_METAL_BATCH_DISPATCH_PIPELINE_OFFSET, d->pipeline_id);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_MODE_OFFSET, d->mode);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_BINDING_START_OFFSET,
              d->binding_start);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_BINDING_COUNT_OFFSET,
              d->binding_count);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_GRID_WIDTH_OFFSET,
              d->grid_width);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_GRID_HEIGHT_OFFSET,
              d->grid_height);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_GRID_DEPTH_OFFSET,
              d->grid_depth);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_GROUP_WIDTH_OFFSET,
              d->group_width);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_GROUP_HEIGHT_OFFSET,
              d->group_height);
        put32(p + INFERNO_METAL_BATCH_DISPATCH_GROUP_DEPTH_OFFSET,
              d->group_depth);
    }
    size_t inline_cursor = 0;
    for (uint32_t i = 0; i < m->binding_count; i++) {
        uint8_t *p = bytes + bindings_offset +
                     (size_t)i * INFERNO_METAL_BATCH_BINDING_RECORD_SIZE;
        const ImtlBatchBinding *b = &m->bindings[i];
        put32(p + INFERNO_METAL_BATCH_BINDING_KIND_OFFSET, b->kind);
        put32(p + INFERNO_METAL_BATCH_BINDING_INDEX_OFFSET, b->index);
        if (b->kind == INFERNO_METAL_BATCH_BINDING_BUFFER) {
            put32(p + INFERNO_METAL_BATCH_BINDING_RESOURCE_OFFSET,
                  b->resource_id);
            put64(p + INFERNO_METAL_BATCH_BINDING_OFFSET_OFFSET, b->offset);
        } else {
            if (b->kind == INFERNO_METAL_BATCH_BINDING_INLINE) {
                put32(p + INFERNO_METAL_BATCH_BINDING_RESOURCE_OFFSET,
                      inline_cursor);
                memcpy(bytes + inline_offset + inline_cursor, b->bytes,
                       b->length);
                inline_cursor += b->length;
            }
            put32(p + INFERNO_METAL_BATCH_BINDING_LENGTH_OFFSET, b->length);
        }
    }
    free(source_offsets);
    *out = bytes;
    *out_size = size;
    *out_images_size = images_size;
    return true;
}

void imtl_batch_builder_free(uint8_t *bytes)
{
    free(bytes);
}

static bool validDiagnostics(const ImtlBatchResult *r)
{
    bool no_error = r->flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR;
    bool warning = r->flags & INFERNO_METAL_BATCH_FLAG_WARNING;
    if (warning || (no_error &&
                    (r->error_code || r->error_domain_length ||
                     (r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED)))) {
        return false;
    }
    if (!no_error && !r->error_domain_length) {
        return false;
    }
    if ((r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED) &&
        r->error_domain_length < INFERNO_METAL_COMPILER_DOMAIN_SIZE - 4) {
        return false;
    }
    return !(r->flags & INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED) ||
           r->error_description_length >=
               INFERNO_METAL_COMPILER_DESCRIPTION_SIZE - 4;
}

bool imtl_batch_decode_result(const uint8_t *bytes, size_t size,
                              uint64_t sequence, uint32_t expected_buffers,
                              uint32_t expected_images, ImtlBatchResult *out)
{
    if (!bytes || !out || expected_buffers > INFERNO_METAL_BATCH_MAX_BUFFERS ||
        expected_images > INFERNO_METAL_BATCH_MAX_IMAGES ||
        size != INFERNO_METAL_BATCH_RESULT_SIZE + expected_images ||
        get32(bytes + INFERNO_METAL_BATCH_RESULT_VERSION_OFFSET) !=
            INFERNO_METAL_VERSION ||
        get32(bytes + INFERNO_METAL_BATCH_RESULT_OPCODE_OFFSET) !=
            INFERNO_METAL_BATCH ||
        get64(bytes + INFERNO_METAL_BATCH_RESULT_SEQUENCE_OFFSET) != sequence ||
        !allZero(bytes + INFERNO_METAL_BATCH_RESULT_RESERVED0_OFFSET,
                 INFERNO_METAL_BATCH_RESULT_RESERVED0_SIZE) ||
        !allZero(bytes + INFERNO_METAL_BATCH_RESULT_RESERVED1_OFFSET,
                 INFERNO_METAL_BATCH_RESULT_RESERVED1_SIZE)) {
        return false;
    }
    ImtlBatchResult r = {
        .sequence = sequence,
        .outcome = get32(bytes + INFERNO_METAL_BATCH_RESULT_OUTCOME_OFFSET),
        .phase = get32(bytes + INFERNO_METAL_BATCH_RESULT_PHASE_OFFSET),
        .flags = get32(bytes + INFERNO_METAL_BATCH_RESULT_FLAGS_OFFSET),
        .failed_record_kind =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_FAILED_KIND_OFFSET),
        .failed_record_index =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_FAILED_INDEX_OFFSET),
        .buffer_count =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_BUFFER_COUNT_OFFSET),
        .images_size =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_IMAGES_SIZE_OFFSET),
        .host_command_buffer_status =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_HOST_STATUS_OFFSET),
        .error_code =
            getSigned64(bytes + INFERNO_METAL_BATCH_RESULT_ERROR_CODE_OFFSET),
        .error_domain =
            (const char *)bytes + INFERNO_METAL_BATCH_RESULT_DOMAIN_OFFSET,
        .error_domain_length =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_DOMAIN_LENGTH_OFFSET),
        .error_description =
            (const char *)bytes + INFERNO_METAL_BATCH_RESULT_DESCRIPTION_OFFSET,
        .error_description_length =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_DESCRIPTION_LENGTH_OFFSET),
        .images = bytes + INFERNO_METAL_BATCH_RESULT_IMAGES_OFFSET,
    };
    if (r.outcome > INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST ||
        r.phase > INFERNO_METAL_BATCH_PHASE_EXECUTE ||
        r.flags & ~INFERNO_METAL_BATCH_FLAG_MASK ||
        r.failed_record_kind > INFERNO_METAL_BATCH_RECORD_BINDING ||
        !validString((const uint8_t *)r.error_domain, r.error_domain_length,
                     INFERNO_METAL_COMPILER_DOMAIN_SIZE) ||
        !validString((const uint8_t *)r.error_description,
                     r.error_description_length,
                     INFERNO_METAL_COMPILER_DESCRIPTION_SIZE) ||
        !validDiagnostics(&r)) {
        return false;
    }
    bool scheduled = r.flags & INFERNO_METAL_BATCH_FLAG_SCHEDULED;
    bool valid = false;
    switch (r.outcome) {
    case INFERNO_METAL_BATCH_OUTCOME_OK:
        valid =
            r.phase == INFERNO_METAL_BATCH_PHASE_EXECUTE && scheduled &&
            r.host_command_buffer_status ==
                INFERNO_METAL_HOST_COMMAND_BUFFER_COMPLETED &&
            r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_UNKNOWN &&
            r.failed_record_index == INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN &&
            r.buffer_count == expected_buffers &&
            r.images_size == expected_images &&
            r.flags == (INFERNO_METAL_BATCH_FLAG_NO_NSERROR |
                        INFERNO_METAL_BATCH_FLAG_SCHEDULED) &&
            !r.error_description_length;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_MALFORMED:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_PARSE && !scheduled &&
                !r.host_command_buffer_status && !r.buffer_count &&
                !r.images_size &&
                r.failed_record_kind != INFERNO_METAL_BATCH_RECORD_UNKNOWN &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED:
        valid = (r.phase == INFERNO_METAL_BATCH_PHASE_LIBRARY ||
                 r.phase == INFERNO_METAL_BATCH_PHASE_PIPELINE) &&
                r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_PIPELINE &&
                r.failed_record_index < INFERNO_METAL_BATCH_MAX_PIPELINES &&
                !scheduled && !r.host_command_buffer_status &&
                r.buffer_count == expected_buffers &&
                r.images_size == expected_images;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_NOT_FOUND:
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH:
    case INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_FUNCTION &&
                r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_PIPELINE &&
                r.failed_record_index < INFERNO_METAL_BATCH_MAX_PIPELINES &&
                !scheduled && !r.host_command_buffer_status &&
                r.buffer_count == expected_buffers &&
                r.images_size == expected_images &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_VALIDATE &&
                (r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_DISPATCH ||
                 r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_BINDING) &&
                ((r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_DISPATCH &&
                  r.failed_record_index < INFERNO_METAL_BATCH_MAX_DISPATCHES) ||
                 (r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_BINDING &&
                  r.failed_record_index < INFERNO_METAL_BATCH_MAX_BINDINGS)) &&
                !scheduled && !r.host_command_buffer_status &&
                r.buffer_count == expected_buffers &&
                r.images_size == expected_images &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_EXECUTION_FAILED:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_EXECUTE &&
                r.host_command_buffer_status ==
                    INFERNO_METAL_HOST_COMMAND_BUFFER_ERROR &&
                r.buffer_count == expected_buffers &&
                r.images_size == expected_images &&
                ((r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_UNKNOWN &&
                  r.failed_record_index ==
                      INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN) ||
                 (r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_DISPATCH &&
                  r.failed_record_index < INFERNO_METAL_BATCH_MAX_DISPATCHES));
        break;
    case INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_VALIDATE &&
                r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_HEADER &&
                r.failed_record_index == 0 && !scheduled &&
                !r.host_command_buffer_status &&
                r.buffer_count == expected_buffers &&
                r.images_size == expected_images &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    }
    if (!valid || (r.outcome != INFERNO_METAL_BATCH_OUTCOME_OK &&
                   !allZero(r.images, expected_images))) {
        return false;
    }
    *out = r;
    return true;
}

static uint32_t floatBits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t doubleBits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool finiteFloat(float value)
{
    return (floatBits(value) & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static bool finiteDouble(double value)
{
    return (doubleBits(value) & UINT64_C(0x7ff0000000000000)) !=
           UINT64_C(0x7ff0000000000000);
}

static bool validBatch5Name(const char *name)
{
    size_t size =
        name ? strnlen(name, INFERNO_METAL_RESOURCE_FUNCTION_NAME_SIZE) : 0;
    return size && size < INFERNO_METAL_RESOURCE_FUNCTION_NAME_SIZE &&
           validUtf8((const uint8_t *)name, size);
}

static bool validBatch5PixelFormat(uint32_t format)
{
    switch (format) {
    case INFERNO_METAL_RESOURCE_PIXEL_FORMAT_RGBA8_UNORM:
    case INFERNO_METAL_RESOURCE_PIXEL_FORMAT_RGBA8_UNORM_SRGB:
    case INFERNO_METAL_RESOURCE_PIXEL_FORMAT_RGBA8_SNORM:
    case INFERNO_METAL_RESOURCE_PIXEL_FORMAT_BGRA8_UNORM:
    case INFERNO_METAL_RESOURCE_PIXEL_FORMAT_BGRA8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

static bool validBatch5Library(const ImtlBatch5Library *library)
{
    if (!library->bytes || !library->size) {
        return false;
    }
    if (library->kind == INFERNO_METAL_RESOURCE_LIBRARY_SOURCE) {
        return library->size <= INFERNO_METAL_MAX_SOURCE &&
               validUtf8(library->bytes, library->size);
    }
    return library->kind == INFERNO_METAL_RESOURCE_LIBRARY_METALLIB &&
           library->size <= INFERNO_METAL_RESOURCE_MAX_METALLIB;
}

static bool sameBatch5Library(const ImtlBatch5Library *a,
                              const ImtlBatch5Library *b)
{
    return a->kind == b->kind && a->size == b->size &&
           !memcmp(a->bytes, b->bytes, a->size);
}

static bool validBatch5ComputePipeline(const ImtlBatch5ComputePipeline *p,
                                       uint32_t library_count)
{
    return p->library_id < library_count && validBatch5Name(p->function_name);
}

static bool sameBatch5ComputePipeline(const ImtlBatch5ComputePipeline *a,
                                      const ImtlBatch5ComputePipeline *b)
{
    return a->library_id == b->library_id &&
           !strcmp(a->function_name, b->function_name);
}

static bool validBatch5RenderPipeline(const ImtlBatch5RenderPipeline *p,
                                      uint32_t library_count)
{
    return p->vertex_library_id < library_count &&
           p->fragment_library_id < library_count &&
           validBatch5PixelFormat(p->color0_pixel_format) &&
           p->raster_sample_count == 1 && p->blending_enabled <= 1 &&
           p->source_rgb_blend_factor <= 18 &&
           p->destination_rgb_blend_factor <= 18 &&
           p->rgb_blend_operation <= 4 && p->source_alpha_blend_factor <= 18 &&
           p->destination_alpha_blend_factor <= 18 &&
           p->alpha_blend_operation <= 4 && p->write_mask <= 15 &&
           validBatch5Name(p->vertex_function_name) &&
           validBatch5Name(p->fragment_function_name);
}

static bool sameBatch5RenderPipeline(const ImtlBatch5RenderPipeline *a,
                                     const ImtlBatch5RenderPipeline *b)
{
    return a->vertex_library_id == b->vertex_library_id &&
           a->fragment_library_id == b->fragment_library_id &&
           a->color0_pixel_format == b->color0_pixel_format &&
           a->raster_sample_count == b->raster_sample_count &&
           a->blending_enabled == b->blending_enabled &&
           a->source_rgb_blend_factor == b->source_rgb_blend_factor &&
           a->destination_rgb_blend_factor == b->destination_rgb_blend_factor &&
           a->rgb_blend_operation == b->rgb_blend_operation &&
           a->source_alpha_blend_factor == b->source_alpha_blend_factor &&
           a->destination_alpha_blend_factor ==
               b->destination_alpha_blend_factor &&
           a->alpha_blend_operation == b->alpha_blend_operation &&
           a->write_mask == b->write_mask &&
           !strcmp(a->vertex_function_name, b->vertex_function_name) &&
           !strcmp(a->fragment_function_name, b->fragment_function_name);
}

static bool validBatch5Sampler(const ImtlBatch5Sampler *s)
{
    if (s->min_filter > INFERNO_METAL_RESOURCE_FILTER_LINEAR ||
        s->mag_filter > INFERNO_METAL_RESOURCE_FILTER_LINEAR ||
        s->mip_filter > INFERNO_METAL_RESOURCE_MIP_FILTER_LINEAR ||
        !s->max_anisotropy || s->max_anisotropy > 16 ||
        s->s_address_mode >
            INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR ||
        s->t_address_mode >
            INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR ||
        s->r_address_mode >
            INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR ||
        s->border_color > INFERNO_METAL_RESOURCE_BORDER_OPAQUE_WHITE ||
        s->reduction_mode !=
            INFERNO_METAL_RESOURCE_REDUCTION_WEIGHTED_AVERAGE ||
        s->normalized_coordinates > 1 || !finiteFloat(s->lod_min_clamp) ||
        !finiteFloat(s->lod_max_clamp) || s->lod_min_clamp < 0 ||
        s->lod_max_clamp < s->lod_min_clamp || s->lod_average > 1 ||
        floatBits(s->lod_bias) ||
        s->compare_function > INFERNO_METAL_RESOURCE_COMPARE_ALWAYS ||
        s->support_argument_buffers > 1) {
        return false;
    }
    return s->normalized_coordinates ||
           (s->s_address_mode == INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
            s->t_address_mode == INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
            s->r_address_mode == INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
            s->mip_filter == INFERNO_METAL_RESOURCE_MIP_FILTER_NOT_MIPMAPPED &&
            s->min_filter == s->mag_filter && s->max_anisotropy == 1);
}

static bool validBatch5DrawState(const ImtlBatch5Draw *d,
                                 uint32_t attachment_width,
                                 uint32_t attachment_height)
{
    if (d->primitive_type > INFERNO_METAL_RESOURCE_PRIMITIVE_TRIANGLE_STRIP ||
        !d->vertex_count || d->vertex_count > INFERNO_METAL_MAX_THREADS ||
        !d->instance_count || d->instance_count > INFERNO_METAL_MAX_THREADS ||
        d->vertex_count > INFERNO_METAL_MAX_THREADS / d->instance_count ||
        d->base_instance || d->cull_mode > INFERNO_METAL_RESOURCE_CULL_BACK ||
        d->winding > INFERNO_METAL_RESOURCE_WINDING_COUNTER_CLOCKWISE ||
        d->fill_mode > INFERNO_METAL_RESOURCE_FILL_MODE_LINES ||
        d->flags & ~INFERNO_METAL_RESOURCE_DRAW_FLAG_MASK) {
        return false;
    }
    if (d->flags & INFERNO_METAL_RESOURCE_DRAW_SCISSOR) {
        if (!d->scissor_width || !d->scissor_height ||
            d->scissor_x > attachment_width ||
            d->scissor_width > attachment_width - d->scissor_x ||
            d->scissor_y > attachment_height ||
            d->scissor_height > attachment_height - d->scissor_y) {
            return false;
        }
    } else if (d->scissor_x || d->scissor_y || d->scissor_width ||
               d->scissor_height) {
        return false;
    }
    if (d->flags & INFERNO_METAL_RESOURCE_DRAW_VIEWPORT) {
        if (!finiteDouble(d->viewport_origin_x) ||
            !finiteDouble(d->viewport_origin_y) ||
            !finiteDouble(d->viewport_width) ||
            !finiteDouble(d->viewport_height) ||
            !finiteDouble(d->viewport_znear) ||
            !finiteDouble(d->viewport_zfar) || d->viewport_origin_x < 0 ||
            d->viewport_origin_y < 0 || d->viewport_width < 0 ||
            d->viewport_height < 0 || d->viewport_origin_x > attachment_width ||
            d->viewport_width > attachment_width - d->viewport_origin_x ||
            d->viewport_origin_y > attachment_height ||
            d->viewport_height > attachment_height - d->viewport_origin_y ||
            d->viewport_znear < 0 || d->viewport_znear > d->viewport_zfar ||
            d->viewport_zfar > 1) {
            return false;
        }
    } else if (doubleBits(d->viewport_origin_x) ||
               doubleBits(d->viewport_origin_y) ||
               doubleBits(d->viewport_width) ||
               doubleBits(d->viewport_height) ||
               doubleBits(d->viewport_znear) || doubleBits(d->viewport_zfar)) {
        return false;
    }
    if (d->flags & INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR) {
        if (!finiteFloat(d->blend_red) || !finiteFloat(d->blend_green) ||
            !finiteFloat(d->blend_blue) || !finiteFloat(d->blend_alpha)) {
            return false;
        }
    } else if (floatBits(d->blend_red) || floatBits(d->blend_green) ||
               floatBits(d->blend_blue) || floatBits(d->blend_alpha)) {
        return false;
    }
    return true;
}

static bool
validBatch5Bindings(const ImtlBatch5Manifest *m, uint32_t start, uint32_t count,
                    uint32_t allowed_kinds, size_t *inline_size,
                    bool *referenced_buffers, bool *referenced_textures,
                    bool *referenced_samplers, bool *referenced_arguments,
                    uint32_t forbidden_texture, uint32_t compute_pipeline)
{
    if (start > m->binding_count || count > m->binding_count - start) {
        return false;
    }
    uint32_t prior_index = 0, prior_kind = 0;
    bool have_prior = false, have_slot_binding = false;
    for (uint32_t i = 0; i < count; i++) {
        const ImtlBatchBinding *b = &m->bindings[start + i];
        bool ordered = !have_prior || b->index > prior_index ||
                       (b->index == prior_index && b->kind > prior_kind);
        bool slot_binding = b->kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER ||
                            b->kind == INFERNO_METAL_RESOURCE_BINDING_INLINE ||
                            b->kind == INFERNO_METAL_RESOURCE_BINDING_ARGUMENT;
        if (!have_prior || b->index != prior_index) {
            have_slot_binding = false;
        }
        bool duplicate =
            have_prior && b->index == prior_index &&
            ((slot_binding && have_slot_binding) || b->kind == prior_kind);
        bool valid = b->kind >= INFERNO_METAL_RESOURCE_BINDING_BUFFER &&
                     b->kind <= INFERNO_METAL_RESOURCE_BINDING_ARGUMENT &&
                     (allowed_kinds & (1U << b->kind)) && ordered && !duplicate;
        valid = valid && (b->kind == INFERNO_METAL_RESOURCE_BINDING_SAMPLER ?
                              b->index <= 15 :
                              b->index <= 30);
        if (b->kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
            valid = valid && b->resource_id < m->buffer_count && !b->bytes &&
                    !b->length && !(b->offset & 3) &&
                    b->offset < m->buffers[b->resource_id].length;
            if (valid) {
                referenced_buffers[b->resource_id] = true;
            }
        } else if (b->kind == INFERNO_METAL_RESOURCE_BINDING_INLINE) {
            valid = valid && !b->resource_id && !b->offset && b->bytes &&
                    b->length &&
                    b->length <= INFERNO_METAL_RESOURCE_MAX_INLINE_BINDING &&
                    checkedAdd(inline_size, b->length) &&
                    *inline_size <= INFERNO_METAL_RESOURCE_MAX_INLINE;
        } else if (b->kind == INFERNO_METAL_RESOURCE_BINDING_THREADGROUP) {
            valid =
                valid && !b->resource_id && !b->offset && !b->bytes &&
                b->length &&
                b->length <= INFERNO_METAL_RESOURCE_MAX_THREADGROUP_MEMORY &&
                !(b->length & 15);
        } else if (b->kind == INFERNO_METAL_RESOURCE_BINDING_TEXTURE) {
            valid = valid && b->resource_id < m->texture_count && !b->offset &&
                    !b->bytes && !b->length &&
                    b->resource_id != forbidden_texture;
            if (valid) {
                referenced_textures[b->resource_id] = true;
            }
        } else if (b->kind == INFERNO_METAL_RESOURCE_BINDING_SAMPLER) {
            valid = valid && b->resource_id < m->sampler_count && !b->offset &&
                    !b->bytes && !b->length;
            if (valid) {
                referenced_samplers[b->resource_id] = true;
            }
        } else if (b->kind == INFERNO_METAL_RESOURCE_BINDING_ARGUMENT) {
            valid = valid && b->resource_id < m->argument_count && !b->offset &&
                    !b->bytes && !b->length &&
                    compute_pipeline < m->compute_pipeline_count;
            if (valid) {
                const ImtlBatch5Argument *argument =
                    &m->arguments[b->resource_id];
                const ImtlBatch5ComputePipeline *pipeline =
                    &m->compute_pipelines[compute_pipeline];
                valid =
                    argument->buffer_index == b->index &&
                    argument->library_id == pipeline->library_id &&
                    !strcmp(argument->function_name, pipeline->function_name);
            }
            if (valid) {
                referenced_arguments[b->resource_id] = true;
            }
        }
        if (!valid) {
            return false;
        }
        prior_index = b->index;
        prior_kind = b->kind;
        have_prior = true;
        have_slot_binding = have_slot_binding || slot_binding;
    }
    return true;
}

static bool validateBatch5Manifest(const ImtlBatch5Manifest *m,
                                   size_t *payload_size, size_t *inline_size,
                                   size_t *constants_size, size_t *images_size)
{
    if (!m || m->library_count > INFERNO_METAL_RESOURCE_MAX_LIBRARIES ||
        m->compute_pipeline_count >
            INFERNO_METAL_RESOURCE_MAX_COMPUTE_PIPELINES ||
        m->render_pipeline_count >
            INFERNO_METAL_RESOURCE_MAX_RENDER_PIPELINES ||
        m->buffer_count > INFERNO_METAL_RESOURCE_MAX_BUFFERS ||
        m->texture_count > INFERNO_METAL_RESOURCE_MAX_TEXTURES ||
        m->sampler_count > INFERNO_METAL_RESOURCE_MAX_SAMPLERS ||
        m->command_count > INFERNO_METAL_RESOURCE_MAX_COMMANDS ||
        m->draw_count > INFERNO_METAL_RESOURCE_MAX_DRAWS ||
        m->binding_count > INFERNO_METAL_RESOURCE_MAX_BINDINGS ||
        m->argument_count > INFERNO_METAL_RESOURCE_MAX_ARGUMENTS ||
        m->member_count > INFERNO_METAL_RESOURCE_MAX_MEMBERS ||
        m->declaration_count > INFERNO_METAL_RESOURCE_MAX_DECLARATIONS ||
        (m->library_count && !m->libraries) ||
        (m->compute_pipeline_count && !m->compute_pipelines) ||
        (m->render_pipeline_count && !m->render_pipelines) ||
        (m->buffer_count && !m->buffers) ||
        (m->texture_count && !m->textures) ||
        (m->sampler_count && !m->samplers) ||
        (m->command_count && !m->commands) || (m->draw_count && !m->draws) ||
        (m->binding_count && !m->bindings) ||
        (m->argument_count && !m->arguments) ||
        (m->member_count && !m->members) ||
        (m->declaration_count && !m->declarations)) {
        return false;
    }
    bool empty = !m->library_count && !m->compute_pipeline_count &&
                 !m->render_pipeline_count && !m->buffer_count &&
                 !m->texture_count && !m->sampler_count && !m->command_count &&
                 !m->draw_count && !m->binding_count && !m->argument_count &&
                 !m->member_count && !m->declaration_count;
    if (!empty && !m->command_count) {
        return false;
    }

    *payload_size = 0;
    *inline_size = 0;
    *constants_size = 0;
    *images_size = 0;
    bool referenced_libraries[INFERNO_METAL_RESOURCE_MAX_LIBRARIES] = { false };
    bool referenced_compute[INFERNO_METAL_RESOURCE_MAX_COMPUTE_PIPELINES] = {
        false
    };
    bool referenced_render[INFERNO_METAL_RESOURCE_MAX_RENDER_PIPELINES] = {
        false
    };
    bool referenced_buffers[INFERNO_METAL_RESOURCE_MAX_BUFFERS] = { false };
    bool referenced_textures[INFERNO_METAL_RESOURCE_MAX_TEXTURES] = { false };
    bool referenced_samplers[INFERNO_METAL_RESOURCE_MAX_SAMPLERS] = { false };
    bool referenced_draws[INFERNO_METAL_RESOURCE_MAX_DRAWS] = { false };
    bool referenced_arguments[INFERNO_METAL_RESOURCE_MAX_ARGUMENTS] = { false };

    for (uint32_t i = 0; i < m->library_count; i++) {
        if (!validBatch5Library(&m->libraries[i]) ||
            !checkedAdd(payload_size, m->libraries[i].size) ||
            *payload_size > INFERNO_METAL_RESOURCE_MAX_PAYLOAD) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (sameBatch5Library(&m->libraries[i], &m->libraries[j])) {
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < m->compute_pipeline_count; i++) {
        const ImtlBatch5ComputePipeline *pipeline = &m->compute_pipelines[i];
        if (!validBatch5ComputePipeline(pipeline, m->library_count)) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (sameBatch5ComputePipeline(pipeline, &m->compute_pipelines[j])) {
                return false;
            }
        }
        referenced_libraries[pipeline->library_id] = true;
    }
    for (uint32_t i = 0; i < m->render_pipeline_count; i++) {
        const ImtlBatch5RenderPipeline *pipeline = &m->render_pipelines[i];
        if (!validBatch5RenderPipeline(pipeline, m->library_count)) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (sameBatch5RenderPipeline(pipeline, &m->render_pipelines[j])) {
                return false;
            }
        }
        referenced_libraries[pipeline->vertex_library_id] = true;
        referenced_libraries[pipeline->fragment_library_id] = true;
    }
    for (uint32_t i = 0; i < m->buffer_count; i++) {
        if (!m->buffers[i].bytes || !m->buffers[i].length ||
            m->buffers[i].length > INFERNO_METAL_BATCH_MAX_BUFFER_LENGTH ||
            !checkedAdd(images_size, m->buffers[i].length) ||
            *images_size > INFERNO_METAL_RESOURCE_MAX_IMAGES) {
            return false;
        }
    }
    for (uint32_t i = 0; i < m->texture_count; i++) {
        const ImtlBatch5Texture *texture = &m->textures[i];
        if (!texture->bytes || !texture->width ||
            texture->width > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
            !texture->height ||
            texture->height > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
            !validBatch5PixelFormat(texture->pixel_format) || !texture->usage ||
            texture->usage & ~INFERNO_METAL_RESOURCE_TEXTURE_USAGE_MASK ||
            texture->flags & ~INFERNO_METAL_RESOURCE_TEXTURE_FLAG_MASK) {
            return false;
        }
        uint64_t length = (uint64_t)texture->width * texture->height *
                          INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL;
        if (length > INFERNO_METAL_RESOURCE_MAX_IMAGES ||
            !checkedAdd(images_size, (size_t)length) ||
            *images_size > INFERNO_METAL_RESOURCE_MAX_IMAGES) {
            return false;
        }
    }
    for (uint32_t i = 0; i < m->sampler_count; i++) {
        if (!validBatch5Sampler(&m->samplers[i])) {
            return false;
        }
    }

    uint32_t expected_member = 0;
    for (uint32_t i = 0; i < m->argument_count; i++) {
        const ImtlBatch5Argument *argument = &m->arguments[i];
        if (argument->library_id >= m->library_count ||
            argument->buffer_index > 30 ||
            argument->member_start != expected_member ||
            !argument->member_count ||
            argument->member_count >
                INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS ||
            argument->member_count > m->member_count - expected_member ||
            !argument->encoded_length ||
            argument->encoded_length >
                INFERNO_METAL_ARGUMENT_MAX_ENCODED_LENGTH ||
            !argument->alignment || argument->alignment > 4096 ||
            (argument->alignment & (argument->alignment - 1)) ||
            !validBatch5Name(argument->function_name)) {
            return false;
        }
        uint32_t prior_id = 0;
        for (uint32_t j = 0; j < argument->member_count; j++) {
            const ImtlBatch5ArgumentMember *member =
                &m->members[argument->member_start + j];
            if ((j && member->member_id <= prior_id) ||
                member->kind < INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER ||
                member->kind >
                    INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
                return false;
            }
            bool valid = false;
            switch (member->kind) {
            case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER:
                valid = member->resource < m->buffer_count &&
                        member->offset < m->buffers[member->resource].length &&
                        !member->constant_bytes && !member->constant_length;
                if (valid) {
                    referenced_buffers[member->resource] = true;
                }
                break;
            case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE:
                valid = member->resource < m->texture_count &&
                        !member->offset && !member->constant_bytes &&
                        !member->constant_length;
                if (valid) {
                    referenced_textures[member->resource] = true;
                }
                break;
            case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER:
                valid = member->resource < m->sampler_count &&
                        !member->offset && !member->constant_bytes &&
                        !member->constant_length &&
                        m->samplers[member->resource].support_argument_buffers;
                if (valid) {
                    referenced_samplers[member->resource] = true;
                }
                break;
            case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT:
                valid = !member->resource && !member->offset &&
                        member->constant_bytes && member->constant_length &&
                        member->constant_length <=
                            INFERNO_METAL_ARGUMENT_MAX_CONSTANT &&
                        checkedAdd(constants_size, member->constant_length) &&
                        *constants_size <= INFERNO_METAL_ARGUMENT_MAX_CONSTANTS;
                break;
            }
            if (!valid) {
                return false;
            }
            prior_id = member->member_id;
        }
        referenced_libraries[argument->library_id] = true;
        expected_member += argument->member_count;
    }
    if (expected_member != m->member_count) {
        return false;
    }

    uint32_t expected_binding = 0, expected_draw = 0, expected_declaration = 0;
    for (uint32_t i = 0; i < m->command_count; i++) {
        const ImtlBatch5Command *command = &m->commands[i];
        if (command->kind == INFERNO_METAL_RESOURCE_COMMAND_COMPUTE) {
            const ImtlBatch5ComputeCommand *compute = &command->value.compute;
            uint64_t grid_product = 0, group_product = 0;
            bool grid_valid = checkedProduct(
                compute->grid_width, compute->grid_height, compute->grid_depth,
                INFERNO_METAL_MAX_THREADS, &grid_product);
            bool group_valid =
                checkedProduct(compute->group_width, compute->group_height,
                               compute->group_depth, INFERNO_METAL_MAX_THREADS,
                               &group_product);
            if (compute->pipeline_id >= m->compute_pipeline_count ||
                (compute->mode != INFERNO_METAL_BATCH_DISPATCH_THREADS &&
                 compute->mode != INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS) ||
                compute->binding_start != expected_binding || !grid_valid ||
                !group_valid ||
                (compute->mode == INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS &&
                 grid_product > INFERNO_METAL_MAX_THREADS / group_product) ||
                !validBatch5Bindings(
                    m, compute->binding_start, compute->binding_count,
                    (1U << INFERNO_METAL_RESOURCE_BINDING_BUFFER) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_INLINE) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_THREADGROUP) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_TEXTURE) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_SAMPLER) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_ARGUMENT),
                    inline_size, referenced_buffers, referenced_textures,
                    referenced_samplers, referenced_arguments, UINT32_MAX,
                    compute->pipeline_id) ||
                compute->declaration_start != expected_declaration ||
                compute->declaration_count >
                    m->declaration_count - expected_declaration) {
                return false;
            }
            uint32_t prior_kind = 0, prior_resource = 0;
            for (uint32_t j = 0; j < compute->declaration_count; j++) {
                const ImtlBatch5ResourceDeclaration *declaration =
                    &m->declarations[compute->declaration_start + j];
                bool ordered = !j || declaration->resource_kind > prior_kind ||
                               (declaration->resource_kind == prior_kind &&
                                declaration->resource > prior_resource);
                bool valid =
                    ordered && declaration->usage &&
                    !(declaration->usage & ~INFERNO_METAL_RESOURCE_USAGE_MASK);
                if (declaration->resource_kind ==
                    INFERNO_METAL_RESOURCE_DECLARATION_BUFFER) {
                    valid = valid && declaration->resource < m->buffer_count;
                    if (valid) {
                        referenced_buffers[declaration->resource] = true;
                    }
                } else if (declaration->resource_kind ==
                           INFERNO_METAL_RESOURCE_DECLARATION_TEXTURE) {
                    valid = valid && declaration->resource < m->texture_count;
                    if (valid) {
                        referenced_textures[declaration->resource] = true;
                    }
                } else {
                    valid = false;
                }
                if (!valid) {
                    return false;
                }
                prior_kind = declaration->resource_kind;
                prior_resource = declaration->resource;
            }
            referenced_compute[compute->pipeline_id] = true;
            expected_binding += compute->binding_count;
            expected_declaration += compute->declaration_count;
        } else if (command->kind == INFERNO_METAL_RESOURCE_COMMAND_RENDER) {
            const ImtlBatch5RenderCommand *render = &command->value.render;
            if (render->texture_id >= m->texture_count ||
                !(m->textures[render->texture_id].usage &
                  INFERNO_METAL_RESOURCE_TEXTURE_USAGE_RENDER_TARGET) ||
                render->load_action > INFERNO_METAL_RESOURCE_LOAD_CLEAR ||
                render->store_action != INFERNO_METAL_RESOURCE_STORE_STORE ||
                render->draw_start != expected_draw ||
                render->draw_start > m->draw_count ||
                render->draw_count > m->draw_count - render->draw_start ||
                !finiteDouble(render->clear_red) ||
                !finiteDouble(render->clear_green) ||
                !finiteDouble(render->clear_blue) ||
                !finiteDouble(render->clear_alpha)) {
                return false;
            }
            referenced_textures[render->texture_id] = true;
            uint32_t render_kinds =
                (1U << INFERNO_METAL_RESOURCE_BINDING_BUFFER) |
                (1U << INFERNO_METAL_RESOURCE_BINDING_INLINE) |
                (1U << INFERNO_METAL_RESOURCE_BINDING_TEXTURE) |
                (1U << INFERNO_METAL_RESOURCE_BINDING_SAMPLER);
            for (uint32_t j = 0; j < render->draw_count; j++) {
                uint32_t draw_index = render->draw_start + j;
                const ImtlBatch5Draw *draw = &m->draws[draw_index];
                if (draw->render_pipeline_id >= m->render_pipeline_count ||
                    m->render_pipelines[draw->render_pipeline_id]
                            .color0_pixel_format !=
                        m->textures[render->texture_id].pixel_format ||
                    !validBatch5DrawState(
                        draw, m->textures[render->texture_id].width,
                        m->textures[render->texture_id].height) ||
                    draw->vertex_binding_start != expected_binding ||
                    !validBatch5Bindings(
                        m, draw->vertex_binding_start,
                        draw->vertex_binding_count, render_kinds, inline_size,
                        referenced_buffers, referenced_textures,
                        referenced_samplers, referenced_arguments,
                        render->texture_id, UINT32_MAX)) {
                    return false;
                }
                expected_binding += draw->vertex_binding_count;
                if (draw->fragment_binding_start != expected_binding ||
                    !validBatch5Bindings(
                        m, draw->fragment_binding_start,
                        draw->fragment_binding_count, render_kinds, inline_size,
                        referenced_buffers, referenced_textures,
                        referenced_samplers, referenced_arguments,
                        render->texture_id, UINT32_MAX)) {
                    return false;
                }
                expected_binding += draw->fragment_binding_count;
                referenced_render[draw->render_pipeline_id] = true;
                referenced_draws[draw_index] = true;
            }
            expected_draw += render->draw_count;
        } else {
            return false;
        }
    }
    if (expected_binding != m->binding_count ||
        expected_draw != m->draw_count ||
        expected_declaration != m->declaration_count ||
        *inline_size > INFERNO_METAL_RESOURCE_MAX_INLINE) {
        return false;
    }
#define REQUIRE_BATCH5_REFERENCED(count, referenced) \
    do {                                             \
        for (uint32_t i = 0; i < (count); i++) {     \
            if (!(referenced)[i]) {                  \
                return false;                        \
            }                                        \
        }                                            \
    } while (0)
    REQUIRE_BATCH5_REFERENCED(m->library_count, referenced_libraries);
    REQUIRE_BATCH5_REFERENCED(m->compute_pipeline_count, referenced_compute);
    REQUIRE_BATCH5_REFERENCED(m->render_pipeline_count, referenced_render);
    REQUIRE_BATCH5_REFERENCED(m->buffer_count, referenced_buffers);
    REQUIRE_BATCH5_REFERENCED(m->texture_count, referenced_textures);
    REQUIRE_BATCH5_REFERENCED(m->sampler_count, referenced_samplers);
    REQUIRE_BATCH5_REFERENCED(m->draw_count, referenced_draws);
    REQUIRE_BATCH5_REFERENCED(m->argument_count, referenced_arguments);
#undef REQUIRE_BATCH5_REFERENCED
    return true;
}

static void putBatch5ComputePipeline(uint8_t *record,
                                     const ImtlBatch5ComputePipeline *pipeline)
{
    put32(record + INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET,
          pipeline->library_id);
    memcpy(record + INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_NAME_OFFSET,
           pipeline->function_name, strlen(pipeline->function_name));
}

static void putBatch5RenderPipeline(uint8_t *record,
                                    const ImtlBatch5RenderPipeline *pipeline)
{
    put32(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_LIBRARY_OFFSET,
          pipeline->vertex_library_id);
    put32(record +
              INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_LIBRARY_OFFSET,
          pipeline->fragment_library_id);
    put32(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_PIXEL_FORMAT_OFFSET,
          pipeline->color0_pixel_format);
    put32(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SAMPLE_COUNT_OFFSET,
          pipeline->raster_sample_count);
    put32(record +
              INFERNO_METAL_RESOURCE_RENDER_PIPELINE_BLENDING_ENABLED_OFFSET,
          pipeline->blending_enabled);
    put32(record +
              INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SOURCE_RGB_FACTOR_OFFSET,
          pipeline->source_rgb_blend_factor);
    put32(
        record +
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_DESTINATION_RGB_FACTOR_OFFSET,
        pipeline->destination_rgb_blend_factor);
    put32(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RGB_OPERATION_OFFSET,
          pipeline->rgb_blend_operation);
    put32(record +
              INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SOURCE_ALPHA_FACTOR_OFFSET,
          pipeline->source_alpha_blend_factor);
    put32(
        record +
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_DESTINATION_ALPHA_FACTOR_OFFSET,
        pipeline->destination_alpha_blend_factor);
    put32(record +
              INFERNO_METAL_RESOURCE_RENDER_PIPELINE_ALPHA_OPERATION_OFFSET,
          pipeline->alpha_blend_operation);
    put32(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_WRITE_MASK_OFFSET,
          pipeline->write_mask);
    memcpy(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_NAME_OFFSET,
           pipeline->vertex_function_name,
           strlen(pipeline->vertex_function_name));
    memcpy(record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_NAME_OFFSET,
           pipeline->fragment_function_name,
           strlen(pipeline->fragment_function_name));
}

static void putBatch5Sampler(uint8_t *record, const ImtlBatch5Sampler *sampler)
{
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_MIN_FILTER_OFFSET,
          sampler->min_filter);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_MAG_FILTER_OFFSET,
          sampler->mag_filter);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_MIP_FILTER_OFFSET,
          sampler->mip_filter);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_MAX_ANISOTROPY_OFFSET,
          sampler->max_anisotropy);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_S_ADDRESS_MODE_OFFSET,
          sampler->s_address_mode);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_T_ADDRESS_MODE_OFFSET,
          sampler->t_address_mode);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_R_ADDRESS_MODE_OFFSET,
          sampler->r_address_mode);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_BORDER_COLOR_OFFSET,
          sampler->border_color);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_REDUCTION_MODE_OFFSET,
          sampler->reduction_mode);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_NORMALIZED_COORDINATES_OFFSET,
          sampler->normalized_coordinates);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_MIN_CLAMP_OFFSET,
          floatBits(sampler->lod_min_clamp));
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_MAX_CLAMP_OFFSET,
          floatBits(sampler->lod_max_clamp));
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_AVERAGE_OFFSET,
          sampler->lod_average);
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_BIAS_OFFSET,
          floatBits(sampler->lod_bias));
    put32(record + INFERNO_METAL_RESOURCE_SAMPLER_COMPARE_FUNCTION_OFFSET,
          sampler->compare_function);
    put32(record +
              INFERNO_METAL_RESOURCE_SAMPLER_SUPPORT_ARGUMENT_BUFFERS_OFFSET,
          sampler->support_argument_buffers);
}

bool imtl_batch5_builder_build(const ImtlBatch5Manifest *m, uint8_t **out,
                               size_t *out_size, uint32_t *out_images_size)
{
    if (out) {
        *out = NULL;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (out_images_size) {
        *out_images_size = 0;
    }
    if (!out || !out_size || !out_images_size) {
        return false;
    }
    size_t payload_size, inline_size, constants_size, images_size;
    if (!validateBatch5Manifest(m, &payload_size, &inline_size, &constants_size,
                                &images_size)) {
        return false;
    }
    size_t size = INFERNO_METAL_RESOURCE_HEADER_SIZE;
#define ADD_BATCH5_TABLE(count, stride) \
    checkedAdd(&size, (size_t)(count) * (stride))
    if (!ADD_BATCH5_TABLE(m->library_count,
                          INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(
            m->compute_pipeline_count,
            INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->render_pipeline_count,
                          INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->buffer_count,
                          INFERNO_METAL_RESOURCE_BUFFER_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->texture_count,
                          INFERNO_METAL_RESOURCE_TEXTURE_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->sampler_count,
                          INFERNO_METAL_RESOURCE_SAMPLER_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->command_count,
                          INFERNO_METAL_RESOURCE_COMMAND_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->draw_count,
                          INFERNO_METAL_RESOURCE_DRAW_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->binding_count,
                          INFERNO_METAL_RESOURCE_BINDING_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->argument_count,
                          INFERNO_METAL_RESOURCE_ARGUMENT_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->member_count,
                          INFERNO_METAL_RESOURCE_MEMBER_RECORD_SIZE) ||
        !ADD_BATCH5_TABLE(m->declaration_count,
                          INFERNO_METAL_RESOURCE_DECLARATION_RECORD_SIZE) ||
        !checkedAdd(&size, payload_size) || !checkedAdd(&size, inline_size) ||
        !checkedAdd(&size, constants_size) || !checkedAdd(&size, images_size) ||
        size > INFERNO_METAL_MAX_BUFFER) {
#undef ADD_BATCH5_TABLE
        return false;
    }
#undef ADD_BATCH5_TABLE
    uint8_t *bytes = calloc(1, size);
    if (!bytes) {
        return false;
    }
    put32(bytes + INFERNO_METAL_RESOURCE_VERSION_OFFSET,
          INFERNO_METAL_RESOURCE_VERSION);
    put32(bytes + INFERNO_METAL_RESOURCE_LIBRARY_COUNT_OFFSET,
          m->library_count);
    put32(bytes + INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_COUNT_OFFSET,
          m->compute_pipeline_count);
    put32(bytes + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_COUNT_OFFSET,
          m->render_pipeline_count);
    put32(bytes + INFERNO_METAL_RESOURCE_BUFFER_COUNT_OFFSET, m->buffer_count);
    put32(bytes + INFERNO_METAL_RESOURCE_TEXTURE_COUNT_OFFSET,
          m->texture_count);
    put32(bytes + INFERNO_METAL_RESOURCE_SAMPLER_COUNT_OFFSET,
          m->sampler_count);
    put32(bytes + INFERNO_METAL_RESOURCE_HEADER_COMMAND_COUNT_OFFSET,
          m->command_count);
    put32(bytes + INFERNO_METAL_RESOURCE_DRAW_COUNT_OFFSET, m->draw_count);
    put32(bytes + INFERNO_METAL_RESOURCE_BINDING_COUNT_OFFSET,
          m->binding_count);
    put32(bytes + INFERNO_METAL_RESOURCE_INLINE_SIZE_OFFSET,
          (uint32_t)inline_size);
    put32(bytes + INFERNO_METAL_RESOURCE_PAYLOAD_SIZE_OFFSET,
          (uint32_t)payload_size);
    put32(bytes + INFERNO_METAL_RESOURCE_IMAGES_SIZE_OFFSET,
          (uint32_t)images_size);
    put32(bytes + INFERNO_METAL_RESOURCE_ARGUMENT_COUNT_OFFSET,
          m->argument_count);
    put32(bytes + INFERNO_METAL_RESOURCE_MEMBER_COUNT_OFFSET, m->member_count);
    put32(bytes + INFERNO_METAL_RESOURCE_DECLARATION_COUNT_OFFSET,
          m->declaration_count);
    put32(bytes + INFERNO_METAL_RESOURCE_CONSTANTS_SIZE_OFFSET,
          (uint32_t)constants_size);

    size_t libraries_offset = INFERNO_METAL_RESOURCE_HEADER_SIZE;
    size_t compute_offset =
        libraries_offset +
        (size_t)m->library_count * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
    size_t render_offset =
        compute_offset +
        (size_t)m->compute_pipeline_count *
            INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE;
    size_t buffers_offset =
        render_offset + (size_t)m->render_pipeline_count *
                            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE;
    size_t textures_offset =
        buffers_offset +
        (size_t)m->buffer_count * INFERNO_METAL_RESOURCE_BUFFER_RECORD_SIZE;
    size_t samplers_offset =
        textures_offset +
        (size_t)m->texture_count * INFERNO_METAL_RESOURCE_TEXTURE_RECORD_SIZE;
    size_t commands_offset =
        samplers_offset +
        (size_t)m->sampler_count * INFERNO_METAL_RESOURCE_SAMPLER_RECORD_SIZE;
    size_t draws_offset =
        commands_offset +
        (size_t)m->command_count * INFERNO_METAL_RESOURCE_COMMAND_RECORD_SIZE;
    size_t bindings_offset =
        draws_offset +
        (size_t)m->draw_count * INFERNO_METAL_RESOURCE_DRAW_RECORD_SIZE;
    size_t arguments_offset =
        bindings_offset +
        (size_t)m->binding_count * INFERNO_METAL_RESOURCE_BINDING_RECORD_SIZE;
    size_t members_offset =
        arguments_offset +
        (size_t)m->argument_count * INFERNO_METAL_RESOURCE_ARGUMENT_RECORD_SIZE;
    size_t declarations_offset =
        members_offset +
        (size_t)m->member_count * INFERNO_METAL_RESOURCE_MEMBER_RECORD_SIZE;
    size_t payload_offset = declarations_offset +
                            (size_t)m->declaration_count *
                                INFERNO_METAL_RESOURCE_DECLARATION_RECORD_SIZE;
    size_t inline_offset = payload_offset + payload_size;
    size_t constants_offset = inline_offset + inline_size;
    size_t images_offset = constants_offset + constants_size;

    size_t payload_cursor = 0;
    for (uint32_t i = 0; i < m->library_count; i++) {
        uint8_t *record =
            bytes + libraries_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET,
              m->libraries[i].kind);
        put32(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET,
              (uint32_t)payload_cursor);
        put32(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET,
              (uint32_t)m->libraries[i].size);
        memcpy(bytes + payload_offset + payload_cursor, m->libraries[i].bytes,
               m->libraries[i].size);
        payload_cursor += m->libraries[i].size;
    }
    for (uint32_t i = 0; i < m->compute_pipeline_count; i++) {
        putBatch5ComputePipeline(
            bytes + compute_offset +
                (size_t)i * INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE,
            &m->compute_pipelines[i]);
    }
    for (uint32_t i = 0; i < m->render_pipeline_count; i++) {
        putBatch5RenderPipeline(
            bytes + render_offset +
                (size_t)i * INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE,
            &m->render_pipelines[i]);
    }
    size_t image_cursor = 0;
    for (uint32_t i = 0; i < m->buffer_count; i++) {
        uint8_t *record = bytes + buffers_offset +
                          (size_t)i * INFERNO_METAL_RESOURCE_BUFFER_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_BUFFER_LENGTH_OFFSET,
              (uint32_t)m->buffers[i].length);
        memcpy(bytes + images_offset + image_cursor, m->buffers[i].bytes,
               m->buffers[i].length);
        image_cursor += m->buffers[i].length;
    }
    for (uint32_t i = 0; i < m->texture_count; i++) {
        const ImtlBatch5Texture *texture = &m->textures[i];
        uint8_t *record =
            bytes + textures_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_TEXTURE_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_TEXTURE_WIDTH_OFFSET,
              texture->width);
        put32(record + INFERNO_METAL_RESOURCE_TEXTURE_HEIGHT_OFFSET,
              texture->height);
        put32(record + INFERNO_METAL_RESOURCE_TEXTURE_PIXEL_FORMAT_OFFSET,
              texture->pixel_format);
        put32(record + INFERNO_METAL_RESOURCE_TEXTURE_USAGE_OFFSET,
              texture->usage);
        put32(record + INFERNO_METAL_RESOURCE_TEXTURE_FLAGS_OFFSET,
              texture->flags);
        size_t length = (size_t)texture->width * texture->height *
                        INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL;
        memcpy(bytes + images_offset + image_cursor, texture->bytes, length);
        image_cursor += length;
    }
    for (uint32_t i = 0; i < m->sampler_count; i++) {
        putBatch5Sampler(bytes + samplers_offset +
                             (size_t)i *
                                 INFERNO_METAL_RESOURCE_SAMPLER_RECORD_SIZE,
                         &m->samplers[i]);
    }
    for (uint32_t i = 0; i < m->command_count; i++) {
        uint8_t *record =
            bytes + commands_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_COMMAND_RECORD_SIZE;
        const ImtlBatch5Command *command = &m->commands[i];
        put32(record + INFERNO_METAL_RESOURCE_COMMAND_KIND_OFFSET,
              command->kind);
        if (command->kind == INFERNO_METAL_RESOURCE_COMMAND_COMPUTE) {
            const ImtlBatch5ComputeCommand *compute = &command->value.compute;
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_RESOURCE_OFFSET,
                  compute->pipeline_id);
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_ACTION_OFFSET,
                  compute->mode);
            put32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COUNT_OR_ACTION_OFFSET,
                  compute->binding_start);
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_START_OFFSET,
                  compute->binding_count);
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_COUNT_OFFSET,
                  compute->grid_width);
            put32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_HEIGHT_OFFSET,
                  compute->grid_height);
            put32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_DEPTH_OFFSET,
                  compute->grid_depth);
            put32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_WIDTH_OFFSET,
                  compute->group_width);
            put32(
                record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_HEIGHT_OFFSET,
                compute->group_height);
            put32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_DEPTH_OFFSET,
                  compute->group_depth);
            put32(
                record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_START_OFFSET,
                compute->declaration_start);
            put32(
                record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_COUNT_OFFSET,
                compute->declaration_count);
        } else {
            const ImtlBatch5RenderCommand *render = &command->value.render;
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_RESOURCE_OFFSET,
                  render->texture_id);
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_ACTION_OFFSET,
                  render->load_action);
            put32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COUNT_OR_ACTION_OFFSET,
                  render->store_action);
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_START_OFFSET,
                  render->draw_start);
            put32(record + INFERNO_METAL_RESOURCE_COMMAND_COUNT_OFFSET,
                  render->draw_count);
            put64(record +
                      INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_RED_OFFSET,
                  doubleBits(render->clear_red));
            put64(record +
                      INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_GREEN_OFFSET,
                  doubleBits(render->clear_green));
            put64(record +
                      INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_BLUE_OFFSET,
                  doubleBits(render->clear_blue));
            put64(record +
                      INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_ALPHA_OFFSET,
                  doubleBits(render->clear_alpha));
        }
    }
    for (uint32_t i = 0; i < m->draw_count; i++) {
        const ImtlBatch5Draw *draw = &m->draws[i];
        uint8_t *record = bytes + draws_offset +
                          (size_t)i * INFERNO_METAL_RESOURCE_DRAW_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_DRAW_PIPELINE_OFFSET,
              draw->render_pipeline_id);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_PRIMITIVE_TYPE_OFFSET,
              draw->primitive_type);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_START_OFFSET,
              draw->vertex_start);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_COUNT_OFFSET,
              draw->vertex_count);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_INSTANCE_COUNT_OFFSET,
              draw->instance_count);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_BASE_INSTANCE_OFFSET,
              draw->base_instance);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_START_OFFSET,
              draw->vertex_binding_start);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_COUNT_OFFSET,
              draw->vertex_binding_count);
        put32(record +
                  INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_START_OFFSET,
              draw->fragment_binding_start);
        put32(record +
                  INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_COUNT_OFFSET,
              draw->fragment_binding_count);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_CULL_MODE_OFFSET,
              draw->cull_mode);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_WINDING_OFFSET,
              draw->winding);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_FILL_MODE_OFFSET,
              draw->fill_mode);
        put32(record + INFERNO_METAL_RESOURCE_DRAW_FLAGS_OFFSET, draw->flags);
        if (draw->flags & INFERNO_METAL_RESOURCE_DRAW_SCISSOR) {
            uint8_t *scissor =
                record + INFERNO_METAL_RESOURCE_DRAW_SCISSOR_OFFSET;
            put32(scissor, draw->scissor_x);
            put32(scissor + 4, draw->scissor_y);
            put32(scissor + 8, draw->scissor_width);
            put32(scissor + 12, draw->scissor_height);
        }
        if (draw->flags & INFERNO_METAL_RESOURCE_DRAW_VIEWPORT) {
            uint8_t *viewport =
                record + INFERNO_METAL_RESOURCE_DRAW_VIEWPORT_OFFSET;
            put64(viewport, doubleBits(draw->viewport_origin_x));
            put64(viewport + 8, doubleBits(draw->viewport_origin_y));
            put64(viewport + 16, doubleBits(draw->viewport_width));
            put64(viewport + 24, doubleBits(draw->viewport_height));
            put64(viewport + 32, doubleBits(draw->viewport_znear));
            put64(viewport + 40, doubleBits(draw->viewport_zfar));
        }
        if (draw->flags & INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR) {
            uint8_t *blend =
                record + INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR_OFFSET;
            put32(blend, floatBits(draw->blend_red));
            put32(blend + 4, floatBits(draw->blend_green));
            put32(blend + 8, floatBits(draw->blend_blue));
            put32(blend + 12, floatBits(draw->blend_alpha));
        }
    }
    size_t inline_cursor = 0;
    for (uint32_t i = 0; i < m->binding_count; i++) {
        const ImtlBatchBinding *binding = &m->bindings[i];
        uint8_t *record =
            bytes + bindings_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_BINDING_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET,
              binding->kind);
        put32(record + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET,
              binding->index);
        if (binding->kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
            put32(record + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET,
                  binding->resource_id);
            put64(record + INFERNO_METAL_RESOURCE_BINDING_OFFSET_OFFSET,
                  binding->offset);
        } else if (binding->kind == INFERNO_METAL_RESOURCE_BINDING_INLINE) {
            put32(record + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET,
                  (uint32_t)inline_cursor);
            put32(record + INFERNO_METAL_RESOURCE_BINDING_LENGTH_OFFSET,
                  (uint32_t)binding->length);
            memcpy(bytes + inline_offset + inline_cursor, binding->bytes,
                   binding->length);
            inline_cursor += binding->length;
        } else if (binding->kind ==
                   INFERNO_METAL_RESOURCE_BINDING_THREADGROUP) {
            put32(record + INFERNO_METAL_RESOURCE_BINDING_LENGTH_OFFSET,
                  (uint32_t)binding->length);
        } else {
            put32(record + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET,
                  binding->resource_id);
        }
    }
    for (uint32_t i = 0; i < m->argument_count; i++) {
        const ImtlBatch5Argument *argument = &m->arguments[i];
        uint8_t *record =
            bytes + arguments_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_ARGUMENT_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_ARGUMENT_LIBRARY_OFFSET,
              argument->library_id);
        put32(record + INFERNO_METAL_RESOURCE_ARGUMENT_BUFFER_INDEX_OFFSET,
              argument->buffer_index);
        put32(record + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_START_OFFSET,
              argument->member_start);
        put32(record + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_COUNT_OFFSET,
              argument->member_count);
        put32(record + INFERNO_METAL_RESOURCE_ARGUMENT_ENCODED_LENGTH_OFFSET,
              argument->encoded_length);
        put32(record + INFERNO_METAL_RESOURCE_ARGUMENT_ALIGNMENT_OFFSET,
              argument->alignment);
        memcpy(record + INFERNO_METAL_RESOURCE_ARGUMENT_NAME_OFFSET,
               argument->function_name, strlen(argument->function_name));
    }
    size_t constants_cursor = 0;
    for (uint32_t i = 0; i < m->member_count; i++) {
        const ImtlBatch5ArgumentMember *member = &m->members[i];
        uint8_t *record = bytes + members_offset +
                          (size_t)i * INFERNO_METAL_RESOURCE_MEMBER_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_MEMBER_KIND_OFFSET, member->kind);
        put32(record + INFERNO_METAL_RESOURCE_MEMBER_ID_OFFSET,
              member->member_id);
        if (member->kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
            put32(record + INFERNO_METAL_RESOURCE_MEMBER_RESOURCE_OFFSET,
                  (uint32_t)constants_cursor);
            put32(record + INFERNO_METAL_RESOURCE_MEMBER_LENGTH_OFFSET,
                  member->constant_length);
            memcpy(bytes + constants_offset + constants_cursor,
                   member->constant_bytes, member->constant_length);
            constants_cursor += member->constant_length;
        } else {
            put32(record + INFERNO_METAL_RESOURCE_MEMBER_RESOURCE_OFFSET,
                  member->resource);
            put64(record + INFERNO_METAL_RESOURCE_MEMBER_OFFSET_OFFSET,
                  member->offset);
        }
    }
    for (uint32_t i = 0; i < m->declaration_count; i++) {
        const ImtlBatch5ResourceDeclaration *declaration = &m->declarations[i];
        uint8_t *record =
            bytes + declarations_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_DECLARATION_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_KIND_OFFSET,
              declaration->resource_kind);
        put32(record + INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_OFFSET,
              declaration->resource);
        put32(record + INFERNO_METAL_RESOURCE_DECLARATION_USAGE_OFFSET,
              declaration->usage);
    }
    *out = bytes;
    *out_size = size;
    *out_images_size = (uint32_t)images_size;
    return true;
}

bool imtl_typed_query_builder_build(uint32_t opcode,
                                    const ImtlTypedQueryManifest *m,
                                    uint8_t **out, size_t *out_size)
{
    if (out) {
        *out = NULL;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (!m || !out || !out_size || !m->libraries ||
        (opcode != INFERNO_METAL_QUERY_LIBRARY_TYPED &&
         opcode != INFERNO_METAL_QUERY_PIPELINE_TYPED &&
         opcode != INFERNO_METAL_QUERY_RENDER_PIPELINE &&
         opcode != INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED &&
         opcode != INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) ||
        (opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE ?
             (m->library_count < 1 || m->library_count > 2) :
             m->library_count != 1) ||
        ((opcode == INFERNO_METAL_QUERY_LIBRARY_TYPED) &&
         (m->compute_pipeline || m->render_pipeline)) ||
        ((opcode == INFERNO_METAL_QUERY_PIPELINE_TYPED ||
          opcode == INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED ||
          opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) &&
         (!m->compute_pipeline || m->render_pipeline)) ||
        (opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE &&
         (m->compute_pipeline || !m->render_pipeline)) ||
        (opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ?
             m->argument_buffer_index > 30 :
             m->argument_buffer_index != 0)) {
        return false;
    }

    size_t payload_size = 0;
    bool referenced[INFERNO_METAL_RESOURCE_MAX_LIBRARIES] = { false };
    for (uint32_t i = 0; i < m->library_count; i++) {
        if (!validBatch5Library(&m->libraries[i]) ||
            !checkedAdd(&payload_size, m->libraries[i].size) ||
            payload_size > INFERNO_METAL_RESOURCE_MAX_PAYLOAD) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            if (sameBatch5Library(&m->libraries[i], &m->libraries[j])) {
                return false;
            }
        }
    }
    size_t pipeline_size = 0;
    if (m->compute_pipeline) {
        if (!validBatch5ComputePipeline(m->compute_pipeline,
                                        m->library_count)) {
            return false;
        }
        referenced[m->compute_pipeline->library_id] = true;
        pipeline_size = INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE;
    } else if (m->render_pipeline) {
        if (!validBatch5RenderPipeline(m->render_pipeline, m->library_count)) {
            return false;
        }
        referenced[m->render_pipeline->vertex_library_id] = true;
        referenced[m->render_pipeline->fragment_library_id] = true;
        pipeline_size = INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE;
    } else {
        referenced[0] = true;
    }
    for (uint32_t i = 0; i < m->library_count; i++) {
        if (!referenced[i]) {
            return false;
        }
    }

    size_t size = INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE;
    if (!checkedAdd(&size, (size_t)m->library_count *
                               INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE) ||
        !checkedAdd(&size, pipeline_size) || !checkedAdd(&size, payload_size) ||
        size > INFERNO_METAL_MAX_BUFFER) {
        return false;
    }
    uint8_t *bytes = calloc(1, size);
    if (!bytes) {
        return false;
    }
    put32(bytes + INFERNO_METAL_RESOURCE_QUERY_VERSION_OFFSET,
          INFERNO_METAL_RESOURCE_VERSION);
    put32(bytes + INFERNO_METAL_RESOURCE_QUERY_LIBRARY_COUNT_OFFSET,
          m->library_count);
    put32(bytes + INFERNO_METAL_RESOURCE_QUERY_PAYLOAD_SIZE_OFFSET,
          (uint32_t)payload_size);
    if (opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) {
        put32(bytes + INFERNO_METAL_RESOURCE_QUERY_ARGUMENT_BUFFER_INDEX_OFFSET,
              m->argument_buffer_index);
    }
    size_t libraries_offset = INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE;
    size_t pipeline_offset =
        libraries_offset +
        (size_t)m->library_count * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
    size_t payload_offset = pipeline_offset + pipeline_size;
    size_t payload_cursor = 0;
    for (uint32_t i = 0; i < m->library_count; i++) {
        uint8_t *record =
            bytes + libraries_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
        put32(record + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET,
              m->libraries[i].kind);
        put32(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET,
              (uint32_t)payload_cursor);
        put32(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET,
              (uint32_t)m->libraries[i].size);
        memcpy(bytes + payload_offset + payload_cursor, m->libraries[i].bytes,
               m->libraries[i].size);
        payload_cursor += m->libraries[i].size;
    }
    if (m->compute_pipeline) {
        putBatch5ComputePipeline(bytes + pipeline_offset, m->compute_pipeline);
    } else if (m->render_pipeline) {
        putBatch5RenderPipeline(bytes + pipeline_offset, m->render_pipeline);
    }
    *out = bytes;
    *out_size = size;
    return true;
}

static bool validBatch5Diagnostics(const ImtlBatch5Result *r)
{
    bool no_error = r->flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR;
    bool warning = r->flags & INFERNO_METAL_BATCH_FLAG_WARNING;
    if (warning || (no_error &&
                    (r->error_code || r->error_domain_length ||
                     (r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED)))) {
        return false;
    }
    if (!no_error && !r->error_domain_length) {
        return false;
    }
    if ((r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED) &&
        r->error_domain_length < INFERNO_METAL_COMPILER_DOMAIN_SIZE - 4) {
        return false;
    }
    return !(r->flags & INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED) ||
           r->error_description_length >=
               INFERNO_METAL_COMPILER_DESCRIPTION_SIZE - 4;
}

static bool validBatch5RecordIndex(uint32_t kind, uint32_t index)
{
    switch (kind) {
    case INFERNO_METAL_BATCH_RECORD_HEADER:
        return index == 0;
    case INFERNO_METAL_BATCH_RECORD_PIPELINE:
        return index < INFERNO_METAL_RESOURCE_MAX_COMPUTE_PIPELINES;
    case INFERNO_METAL_BATCH_RECORD_BUFFER:
        return index < INFERNO_METAL_RESOURCE_MAX_BUFFERS;
    case INFERNO_METAL_BATCH_RECORD_BINDING:
        return index < INFERNO_METAL_RESOURCE_MAX_BINDINGS;
    case INFERNO_METAL_RESOURCE_RECORD_LIBRARY:
        return index < INFERNO_METAL_RESOURCE_MAX_LIBRARIES;
    case INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE:
        return index < INFERNO_METAL_RESOURCE_MAX_RENDER_PIPELINES;
    case INFERNO_METAL_RESOURCE_RECORD_TEXTURE:
        return index < INFERNO_METAL_RESOURCE_MAX_TEXTURES;
    case INFERNO_METAL_RESOURCE_RECORD_SAMPLER:
        return index < INFERNO_METAL_RESOURCE_MAX_SAMPLERS;
    case INFERNO_METAL_RESOURCE_RECORD_COMMAND:
        return index < INFERNO_METAL_RESOURCE_MAX_COMMANDS;
    case INFERNO_METAL_RESOURCE_RECORD_DRAW:
        return index < INFERNO_METAL_RESOURCE_MAX_DRAWS;
    case INFERNO_METAL_RESOURCE_RECORD_ARGUMENT:
        return index < INFERNO_METAL_RESOURCE_MAX_ARGUMENTS;
    case INFERNO_METAL_RESOURCE_RECORD_MEMBER:
        return index < INFERNO_METAL_RESOURCE_MAX_MEMBERS;
    case INFERNO_METAL_RESOURCE_RECORD_DECLARATION:
        return index < INFERNO_METAL_RESOURCE_MAX_DECLARATIONS;
    default:
        return false;
    }
}

bool imtl_batch5_decode_result(const uint8_t *bytes, size_t size,
                               uint64_t sequence, uint32_t expected_buffers,
                               uint32_t expected_textures,
                               uint32_t expected_images, ImtlBatch5Result *out)
{
    if (!bytes || !out ||
        expected_buffers > INFERNO_METAL_RESOURCE_MAX_BUFFERS ||
        expected_textures > INFERNO_METAL_RESOURCE_MAX_TEXTURES ||
        expected_images > INFERNO_METAL_RESOURCE_MAX_IMAGES ||
        size != INFERNO_METAL_RESOURCE_RESULT_SIZE + expected_images ||
        get32(bytes + INFERNO_METAL_BATCH_RESULT_VERSION_OFFSET) !=
            INFERNO_METAL_RESOURCE_VERSION ||
        get32(bytes + INFERNO_METAL_BATCH_RESULT_OPCODE_OFFSET) !=
            INFERNO_METAL_BATCH_RESOURCES ||
        get64(bytes + INFERNO_METAL_BATCH_RESULT_SEQUENCE_OFFSET) != sequence ||
        !allZero(bytes + INFERNO_METAL_RESOURCE_RESULT_RESERVED0_OFFSET,
                 INFERNO_METAL_RESOURCE_RESULT_RESERVED0_SIZE) ||
        !allZero(bytes + INFERNO_METAL_BATCH_RESULT_RESERVED1_OFFSET,
                 INFERNO_METAL_BATCH_RESULT_RESERVED1_SIZE)) {
        return false;
    }
    ImtlBatch5Result r = {
        .sequence = sequence,
        .outcome = get32(bytes + INFERNO_METAL_BATCH_RESULT_OUTCOME_OFFSET),
        .phase = get32(bytes + INFERNO_METAL_BATCH_RESULT_PHASE_OFFSET),
        .flags = get32(bytes + INFERNO_METAL_BATCH_RESULT_FLAGS_OFFSET),
        .failed_record_kind =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_FAILED_KIND_OFFSET),
        .failed_record_index =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_FAILED_INDEX_OFFSET),
        .buffer_count =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_BUFFER_COUNT_OFFSET),
        .texture_count =
            get32(bytes + INFERNO_METAL_RESOURCE_RESULT_TEXTURE_COUNT_OFFSET),
        .images_size =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_IMAGES_SIZE_OFFSET),
        .host_command_buffer_status =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_HOST_STATUS_OFFSET),
        .error_code =
            getSigned64(bytes + INFERNO_METAL_BATCH_RESULT_ERROR_CODE_OFFSET),
        .error_domain =
            (const char *)bytes + INFERNO_METAL_BATCH_RESULT_DOMAIN_OFFSET,
        .error_domain_length =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_DOMAIN_LENGTH_OFFSET),
        .error_description =
            (const char *)bytes + INFERNO_METAL_BATCH_RESULT_DESCRIPTION_OFFSET,
        .error_description_length =
            get32(bytes + INFERNO_METAL_BATCH_RESULT_DESCRIPTION_LENGTH_OFFSET),
        .images = bytes + INFERNO_METAL_BATCH_RESULT_IMAGES_OFFSET,
    };
    if (r.outcome > INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED ||
        r.phase > INFERNO_METAL_RESOURCE_PHASE_RESOURCE ||
        r.flags & ~INFERNO_METAL_BATCH_FLAG_MASK ||
        r.failed_record_kind > INFERNO_METAL_RESOURCE_RECORD_DECLARATION ||
        !validString((const uint8_t *)r.error_domain, r.error_domain_length,
                     INFERNO_METAL_COMPILER_DOMAIN_SIZE) ||
        !validString((const uint8_t *)r.error_description,
                     r.error_description_length,
                     INFERNO_METAL_COMPILER_DESCRIPTION_SIZE) ||
        !validBatch5Diagnostics(&r)) {
        return false;
    }
    bool scheduled = r.flags & INFERNO_METAL_BATCH_FLAG_SCHEDULED;
    bool typed_counts = r.buffer_count == expected_buffers &&
                        r.texture_count == expected_textures &&
                        r.images_size == expected_images;
    bool valid = false;
    switch (r.outcome) {
    case INFERNO_METAL_BATCH_OUTCOME_OK:
        valid =
            r.phase == INFERNO_METAL_BATCH_PHASE_EXECUTE && scheduled &&
            r.host_command_buffer_status ==
                INFERNO_METAL_HOST_COMMAND_BUFFER_COMPLETED &&
            r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_UNKNOWN &&
            r.failed_record_index == INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN &&
            typed_counts &&
            r.flags == (INFERNO_METAL_BATCH_FLAG_NO_NSERROR |
                        INFERNO_METAL_BATCH_FLAG_SCHEDULED) &&
            !r.error_description_length;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_MALFORMED:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_PARSE && !scheduled &&
                !r.host_command_buffer_status && !r.buffer_count &&
                !r.texture_count && !r.images_size &&
                validBatch5RecordIndex(r.failed_record_kind,
                                       r.failed_record_index) &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED:
        valid =
            !scheduled && !r.host_command_buffer_status && typed_counts &&
            ((r.phase == INFERNO_METAL_BATCH_PHASE_LIBRARY &&
              r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_LIBRARY) ||
             (r.phase == INFERNO_METAL_BATCH_PHASE_PIPELINE &&
              (r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_PIPELINE ||
               r.failed_record_kind ==
                   INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE))) &&
            validBatch5RecordIndex(r.failed_record_kind, r.failed_record_index);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_NOT_FOUND:
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH:
    case INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_FUNCTION && !scheduled &&
                !r.host_command_buffer_status && typed_counts &&
                (r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_PIPELINE ||
                 r.failed_record_kind ==
                     INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE) &&
                validBatch5RecordIndex(r.failed_record_kind,
                                       r.failed_record_index) &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH:
        valid =
            r.phase == INFERNO_METAL_BATCH_PHASE_VALIDATE && !scheduled &&
            !r.host_command_buffer_status && typed_counts &&
            (r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_COMMAND ||
             r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_DRAW ||
             r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_BINDING) &&
            validBatch5RecordIndex(r.failed_record_kind,
                                   r.failed_record_index) &&
            (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_BATCH_OUTCOME_EXECUTION_FAILED:
        valid =
            r.phase == INFERNO_METAL_BATCH_PHASE_EXECUTE &&
            r.host_command_buffer_status ==
                INFERNO_METAL_HOST_COMMAND_BUFFER_ERROR &&
            typed_counts &&
            ((r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_UNKNOWN &&
              r.failed_record_index ==
                  INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN) ||
             (r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_COMMAND &&
              validBatch5RecordIndex(r.failed_record_kind,
                                     r.failed_record_index)));
        break;
    case INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_VALIDATE && !scheduled &&
                !r.host_command_buffer_status && typed_counts &&
                r.failed_record_kind == INFERNO_METAL_BATCH_RECORD_HEADER &&
                r.failed_record_index == 0 &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE:
        valid = r.phase == INFERNO_METAL_BATCH_PHASE_VALIDATE && !scheduled &&
                !r.host_command_buffer_status && typed_counts &&
                validBatch5RecordIndex(r.failed_record_kind,
                                       r.failed_record_index) &&
                (r.flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR);
        break;
    case INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED:
        valid =
            r.phase == INFERNO_METAL_RESOURCE_PHASE_RESOURCE && !scheduled &&
            !r.host_command_buffer_status && typed_counts &&
            (r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_TEXTURE ||
             r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_SAMPLER ||
             r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_COMMAND ||
             r.failed_record_kind == INFERNO_METAL_RESOURCE_RECORD_ARGUMENT) &&
            validBatch5RecordIndex(r.failed_record_kind, r.failed_record_index);
        break;
    }
    if (!valid || (r.outcome != INFERNO_METAL_BATCH_OUTCOME_OK &&
                   !allZero(r.images, expected_images))) {
        return false;
    }
    *out = r;
    return true;
}
