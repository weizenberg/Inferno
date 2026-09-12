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
    if (warning ||
        (no_error &&
         (r->error_code || r->error_domain_length ||
          r->error_description_length ||
          (r->flags & (INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED |
                       INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED))))) {
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
