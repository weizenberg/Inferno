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

#include "qemu/osdep.h"
#include "metal-batch.h"

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
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

static bool addRegion(size_t *offset, uint32_t count, uint32_t stride,
                      size_t limit)
{
    uint64_t end = (uint64_t)*offset + (uint64_t)count * stride;
    if (end > limit || end > SIZE_MAX) {
        return false;
    }
    *offset = (size_t)end;
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
            code = a & 0x07;
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

static bool fail(InfernoMetalBatchParseError *error, uint32_t kind,
                 uint32_t index)
{
    if (error) {
        error->record_kind = kind;
        error->record_index = index;
    }
    return false;
}

const uint8_t *inferno_metal_batch_pipeline(const InfernoMetalBatchView *view,
                                            uint32_t index)
{
    return view->bytes + view->pipelines_offset +
           (size_t)index * INFERNO_METAL_BATCH_PIPELINE_RECORD_SIZE;
}

const uint8_t *inferno_metal_batch_buffer(const InfernoMetalBatchView *view,
                                          uint32_t index)
{
    return view->bytes + view->buffers_offset +
           (size_t)index * INFERNO_METAL_BATCH_BUFFER_RECORD_SIZE;
}

const uint8_t *inferno_metal_batch_dispatch(const InfernoMetalBatchView *view,
                                            uint32_t index)
{
    return view->bytes + view->dispatches_offset +
           (size_t)index * INFERNO_METAL_BATCH_DISPATCH_RECORD_SIZE;
}

const uint8_t *inferno_metal_batch_binding(const InfernoMetalBatchView *view,
                                           uint32_t index)
{
    return view->bytes + view->bindings_offset +
           (size_t)index * INFERNO_METAL_BATCH_BINDING_RECORD_SIZE;
}

bool inferno_metal_batch_parse(const void *opaque, size_t size,
                               size_t output_size, InfernoMetalBatchView *out,
                               InfernoMetalBatchParseError *error)
{
    const uint8_t *bytes = opaque;
    InfernoMetalBatchView view = { 0 };
    if (error) {
        *error = (InfernoMetalBatchParseError){
            .record_kind = INFERNO_METAL_BATCH_RECORD_HEADER,
            .record_index = 0,
        };
    }
    if (!bytes || !out || size < INFERNO_METAL_BATCH_HEADER_SIZE ||
        size > INFERNO_METAL_MAX_BUFFER ||
        output_size > INFERNO_METAL_MAX_BUFFER ||
        get32(bytes + INFERNO_METAL_BATCH_VERSION_OFFSET) !=
            INFERNO_METAL_VERSION ||
        get32(bytes + INFERNO_METAL_BATCH_FLAGS_OFFSET) ||
        !allZero(bytes + INFERNO_METAL_BATCH_RESERVED_OFFSET,
                 INFERNO_METAL_BATCH_RESERVED_SIZE)) {
        return false;
    }
    view = (InfernoMetalBatchView){
        .bytes = bytes,
        .size = size,
        .pipeline_count =
            get32(bytes + INFERNO_METAL_BATCH_PIPELINE_COUNT_OFFSET),
        .buffer_count = get32(bytes + INFERNO_METAL_BATCH_BUFFER_COUNT_OFFSET),
        .dispatch_count =
            get32(bytes + INFERNO_METAL_BATCH_DISPATCH_COUNT_OFFSET),
        .binding_count =
            get32(bytes + INFERNO_METAL_BATCH_BINDING_COUNT_OFFSET),
        .inline_size = get32(bytes + INFERNO_METAL_BATCH_INLINE_SIZE_OFFSET),
        .source_size = get32(bytes + INFERNO_METAL_BATCH_SOURCE_SIZE_OFFSET),
        .images_size = get32(bytes + INFERNO_METAL_BATCH_IMAGES_SIZE_OFFSET),
        .pipelines_offset = INFERNO_METAL_BATCH_HEADER_SIZE,
    };
    bool empty = !view.pipeline_count && !view.buffer_count &&
                 !view.dispatch_count && !view.binding_count &&
                 !view.inline_size && !view.source_size && !view.images_size;
    if (view.pipeline_count > INFERNO_METAL_BATCH_MAX_PIPELINES ||
        view.buffer_count > INFERNO_METAL_BATCH_MAX_BUFFERS ||
        view.dispatch_count > INFERNO_METAL_BATCH_MAX_DISPATCHES ||
        view.binding_count > INFERNO_METAL_BATCH_MAX_BINDINGS ||
        view.inline_size > INFERNO_METAL_BATCH_MAX_INLINE ||
        view.source_size > INFERNO_METAL_BATCH_MAX_SOURCE ||
        view.images_size > INFERNO_METAL_BATCH_MAX_IMAGES ||
        (!empty &&
         (!view.pipeline_count || !view.dispatch_count || !view.source_size)) ||
        (!view.dispatch_count && view.binding_count) ||
        output_size != INFERNO_METAL_BATCH_RESULT_SIZE + view.images_size) {
        return false;
    }
    size_t offset = view.pipelines_offset;
    if (!addRegion(&offset, view.pipeline_count,
                   INFERNO_METAL_BATCH_PIPELINE_RECORD_SIZE, size)) {
        return false;
    }
    view.buffers_offset = offset;
    if (!addRegion(&offset, view.buffer_count,
                   INFERNO_METAL_BATCH_BUFFER_RECORD_SIZE, size)) {
        return false;
    }
    view.dispatches_offset = offset;
    if (!addRegion(&offset, view.dispatch_count,
                   INFERNO_METAL_BATCH_DISPATCH_RECORD_SIZE, size)) {
        return false;
    }
    view.bindings_offset = offset;
    if (!addRegion(&offset, view.binding_count,
                   INFERNO_METAL_BATCH_BINDING_RECORD_SIZE, size)) {
        return false;
    }
    view.source_offset = offset;
    if (!addRegion(&offset, view.source_size, 1, size)) {
        return false;
    }
    view.inline_offset = offset;
    if (!addRegion(&offset, view.inline_size, 1, size)) {
        return false;
    }
    view.images_offset = offset;
    if (!addRegion(&offset, view.images_size, 1, size) || offset != size) {
        return false;
    }

    uint32_t previous_source_offset = 0, previous_source_size = 0;
    for (uint32_t i = 0; i < view.pipeline_count; i++) {
        const uint8_t *p = inferno_metal_batch_pipeline(&view, i);
        uint32_t source_offset =
            get32(p + INFERNO_METAL_BATCH_PIPELINE_SOURCE_OFFSET);
        uint32_t source_size =
            get32(p + INFERNO_METAL_BATCH_PIPELINE_SOURCE_SIZE_OFFSET);
        const uint8_t *name = p + INFERNO_METAL_BATCH_PIPELINE_NAME_OFFSET;
        size_t name_size = strnlen((const char *)name, 64);
        bool canonical = i == 0 ? source_offset == 0 :
                                  (source_offset == previous_source_offset &&
                                   source_size == previous_source_size) ||
                                      source_offset == previous_source_offset +
                                                           previous_source_size;
        if (!source_size || source_size > INFERNO_METAL_MAX_SOURCE ||
            source_offset > view.source_size ||
            source_size > view.source_size - source_offset || !canonical ||
            !name_size || name_size == 64 ||
            !allZero(name + name_size + 1, 63 - name_size) ||
            get32(p + INFERNO_METAL_BATCH_PIPELINE_FLAGS_OFFSET) ||
            get32(p + INFERNO_METAL_BATCH_PIPELINE_RESERVED_OFFSET) ||
            !validUtf8(bytes + view.source_offset + source_offset,
                       source_size)) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_PIPELINE, i);
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *q = inferno_metal_batch_pipeline(&view, j);
            uint32_t qo = get32(q + INFERNO_METAL_BATCH_PIPELINE_SOURCE_OFFSET);
            uint32_t qs =
                get32(q + INFERNO_METAL_BATCH_PIPELINE_SOURCE_SIZE_OFFSET);
            if (source_size == qs &&
                !memcmp(bytes + view.source_offset + source_offset,
                        bytes + view.source_offset + qo, source_size) &&
                !strcmp((const char *)name,
                        (const char *)q +
                            INFERNO_METAL_BATCH_PIPELINE_NAME_OFFSET)) {
                return fail(error, INFERNO_METAL_BATCH_RECORD_PIPELINE, i);
            }
        }
        previous_source_offset = source_offset;
        previous_source_size = source_size;
    }
    if (view.pipeline_count &&
        previous_source_offset + previous_source_size != view.source_size) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_PIPELINE,
                    view.pipeline_count - 1);
    }

    uint64_t image_sum = 0;
    bool referenced[INFERNO_METAL_BATCH_MAX_BUFFERS] = { false };
    for (uint32_t i = 0; i < view.buffer_count; i++) {
        const uint8_t *p = inferno_metal_batch_buffer(&view, i);
        uint32_t length = get32(p + INFERNO_METAL_BATCH_BUFFER_LENGTH_OFFSET);
        if (!length || length > INFERNO_METAL_BATCH_MAX_BUFFER_LENGTH ||
            get32(p + INFERNO_METAL_BATCH_BUFFER_FLAGS_OFFSET) ||
            get64(p + INFERNO_METAL_BATCH_BUFFER_RESERVED_OFFSET)) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_BUFFER, i);
        }
        image_sum += length;
    }
    if (image_sum != view.images_size) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_HEADER, 0);
    }

    uint32_t expected_binding = 0;
    uint32_t expected_inline = 0;
    for (uint32_t i = 0; i < view.dispatch_count; i++) {
        const uint8_t *p = inferno_metal_batch_dispatch(&view, i);
        uint32_t pipeline =
            get32(p + INFERNO_METAL_BATCH_DISPATCH_PIPELINE_OFFSET);
        uint32_t mode = get32(p + INFERNO_METAL_BATCH_DISPATCH_MODE_OFFSET);
        uint32_t start =
            get32(p + INFERNO_METAL_BATCH_DISPATCH_BINDING_START_OFFSET);
        uint32_t count =
            get32(p + INFERNO_METAL_BATCH_DISPATCH_BINDING_COUNT_OFFSET);
        uint32_t dimensions[6] = {
            get32(p + INFERNO_METAL_BATCH_DISPATCH_GRID_WIDTH_OFFSET),
            get32(p + INFERNO_METAL_BATCH_DISPATCH_GRID_HEIGHT_OFFSET),
            get32(p + INFERNO_METAL_BATCH_DISPATCH_GRID_DEPTH_OFFSET),
            get32(p + INFERNO_METAL_BATCH_DISPATCH_GROUP_WIDTH_OFFSET),
            get32(p + INFERNO_METAL_BATCH_DISPATCH_GROUP_HEIGHT_OFFSET),
            get32(p + INFERNO_METAL_BATCH_DISPATCH_GROUP_DEPTH_OFFSET),
        };
        uint64_t grid_product = 0, group_product = 0;
        bool grid_valid =
            checkedProduct(dimensions[0], dimensions[1], dimensions[2],
                           INFERNO_METAL_MAX_THREADS, &grid_product);
        bool group_valid =
            checkedProduct(dimensions[3], dimensions[4], dimensions[5],
                           INFERNO_METAL_MAX_THREADS, &group_product);
        if (pipeline >= view.pipeline_count ||
            (mode != INFERNO_METAL_BATCH_DISPATCH_THREADS &&
             mode != INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS) ||
            start != expected_binding || start > view.binding_count ||
            count > view.binding_count - start || !grid_valid || !group_valid ||
            (mode == INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS &&
             grid_product > INFERNO_METAL_MAX_THREADS / group_product) ||
            get32(p + INFERNO_METAL_BATCH_DISPATCH_FLAGS_OFFSET) ||
            !allZero(p + INFERNO_METAL_BATCH_DISPATCH_RESERVED_OFFSET,
                     INFERNO_METAL_BATCH_DISPATCH_RESERVED_SIZE)) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_DISPATCH, i);
        }
        uint32_t prior_index = 0, prior_kind = 0;
        bool have_prior = false;
        for (uint32_t j = 0; j < count; j++) {
            uint32_t record_index = start + j;
            const uint8_t *b = inferno_metal_batch_binding(&view, record_index);
            uint32_t kind = get32(b + INFERNO_METAL_BATCH_BINDING_KIND_OFFSET);
            uint32_t index =
                get32(b + INFERNO_METAL_BATCH_BINDING_INDEX_OFFSET);
            uint32_t resource =
                get32(b + INFERNO_METAL_BATCH_BINDING_RESOURCE_OFFSET);
            uint32_t length =
                get32(b + INFERNO_METAL_BATCH_BINDING_LENGTH_OFFSET);
            uint64_t binding_offset =
                get64(b + INFERNO_METAL_BATCH_BINDING_OFFSET_OFFSET);
            bool order = !have_prior || index > prior_index ||
                         (index == prior_index && kind > prior_kind);
            bool duplicate =
                have_prior && index == prior_index &&
                ((kind <= INFERNO_METAL_BATCH_BINDING_INLINE &&
                  prior_kind <= INFERNO_METAL_BATCH_BINDING_INLINE) ||
                 kind == prior_kind);
            bool valid =
                index <= 30 && order && !duplicate &&
                !get64(b + INFERNO_METAL_BATCH_BINDING_RESERVED_OFFSET);
            if (kind == INFERNO_METAL_BATCH_BINDING_BUFFER) {
                if (resource >= view.buffer_count || length ||
                    (binding_offset & 3)) {
                    valid = false;
                } else {
                    uint32_t buffer_length =
                        get32(inferno_metal_batch_buffer(&view, resource) +
                              INFERNO_METAL_BATCH_BUFFER_LENGTH_OFFSET);
                    if (binding_offset >= buffer_length) {
                        valid = false;
                    }
                    referenced[resource] = true;
                }
            } else if (kind == INFERNO_METAL_BATCH_BINDING_INLINE) {
                if (!length || length > 4096 || binding_offset ||
                    resource != expected_inline ||
                    length > view.inline_size - expected_inline) {
                    valid = false;
                } else {
                    expected_inline += length;
                }
            } else if (kind == INFERNO_METAL_BATCH_BINDING_THREADGROUP) {
                if (resource || !length || length > 32768 || (length & 15) ||
                    binding_offset) {
                    valid = false;
                }
            } else {
                valid = false;
            }
            if (!valid) {
                return fail(error, INFERNO_METAL_BATCH_RECORD_BINDING,
                            record_index);
            }
            prior_index = index;
            prior_kind = kind;
            have_prior = true;
        }
        expected_binding += count;
    }
    if (expected_binding != view.binding_count ||
        expected_inline != view.inline_size) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_HEADER, 0);
    }
    for (uint32_t i = 0; i < view.buffer_count; i++) {
        if (!referenced[i]) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_BUFFER, i);
        }
    }
    *out = view;
    return true;
}
