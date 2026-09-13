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

static bool finite32(uint32_t bits)
{
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

static bool finite64(uint64_t bits)
{
    return (bits & UINT64_C(0x7ff0000000000000)) !=
           UINT64_C(0x7ff0000000000000);
}

static float getFloat32(const uint8_t *p)
{
    uint32_t bits = get32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double getFloat64(const uint8_t *p)
{
    uint64_t bits = get64(p);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool validName(const uint8_t *name)
{
    size_t size =
        strnlen((const char *)name, INFERNO_METAL_RESOURCE_FUNCTION_NAME_SIZE);
    return size && size < INFERNO_METAL_RESOURCE_FUNCTION_NAME_SIZE &&
           validUtf8(name, size) &&
           allZero(name + size + 1,
                   INFERNO_METAL_RESOURCE_FUNCTION_NAME_SIZE - size - 1);
}

static bool validPixelFormat(uint32_t format)
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

const uint8_t *
inferno_metal_resource_batch_library(const InfernoMetalResourceBatchView *view,
                                     uint32_t index)
{
    return view->bytes + view->libraries_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
}

const uint8_t *inferno_metal_resource_batch_compute_pipeline(
    const InfernoMetalResourceBatchView *view, uint32_t index)
{
    return view->bytes + view->compute_pipelines_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE;
}

const uint8_t *inferno_metal_resource_batch_render_pipeline(
    const InfernoMetalResourceBatchView *view, uint32_t index)
{
    return view->bytes + view->render_pipelines_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_buffer(const InfernoMetalResourceBatchView *view,
                                    uint32_t index)
{
    return view->bytes + view->buffers_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_BUFFER_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_texture(const InfernoMetalResourceBatchView *view,
                                     uint32_t index)
{
    return view->bytes + view->textures_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_TEXTURE_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_sampler(const InfernoMetalResourceBatchView *view,
                                     uint32_t index)
{
    return view->bytes + view->samplers_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_SAMPLER_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_command(const InfernoMetalResourceBatchView *view,
                                     uint32_t index)
{
    return view->bytes + view->commands_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_COMMAND_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_draw(const InfernoMetalResourceBatchView *view,
                                  uint32_t index)
{
    return view->bytes + view->draws_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_DRAW_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_binding(const InfernoMetalResourceBatchView *view,
                                     uint32_t index)
{
    return view->bytes + view->bindings_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_BINDING_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_argument(const InfernoMetalResourceBatchView *view,
                                      uint32_t index)
{
    return view->bytes + view->arguments_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_ARGUMENT_RECORD_SIZE;
}

const uint8_t *
inferno_metal_resource_batch_member(const InfernoMetalResourceBatchView *view,
                                    uint32_t index)
{
    return view->bytes + view->members_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_MEMBER_RECORD_SIZE;
}

const uint8_t *inferno_metal_resource_batch_declaration(
    const InfernoMetalResourceBatchView *view, uint32_t index)
{
    return view->bytes + view->declarations_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_DECLARATION_RECORD_SIZE;
}

const uint8_t *
inferno_metal_typed_query_library(const InfernoMetalTypedQueryView *view,
                                  uint32_t index)
{
    return view->bytes + view->libraries_offset +
           (size_t)index * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
}

const uint8_t *
inferno_metal_typed_query_pipeline(const InfernoMetalTypedQueryView *view)
{
    return view->pipeline_size ? view->bytes + view->pipeline_offset : NULL;
}

static bool validResourceLibraries(const uint8_t *bytes,
                                   size_t libraries_offset,
                                   uint32_t library_count,
                                   size_t payload_offset, uint32_t payload_size,
                                   InfernoMetalBatchParseError *error)
{
    uint32_t expected_offset = 0;
    for (uint32_t i = 0; i < library_count; i++) {
        const uint8_t *record =
            bytes + libraries_offset +
            (size_t)i * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
        uint32_t kind =
            get32(record + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET);
        uint32_t offset =
            get32(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET);
        uint32_t size =
            get32(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET);
        uint32_t limit = kind == INFERNO_METAL_RESOURCE_LIBRARY_SOURCE ?
                             INFERNO_METAL_MAX_SOURCE :
                             INFERNO_METAL_RESOURCE_MAX_METALLIB;
        if ((kind != INFERNO_METAL_RESOURCE_LIBRARY_SOURCE &&
             kind != INFERNO_METAL_RESOURCE_LIBRARY_METALLIB) ||
            offset != expected_offset || !size || size > limit ||
            offset > payload_size || size > payload_size - offset ||
            get32(record + INFERNO_METAL_RESOURCE_LIBRARY_FLAGS_OFFSET) ||
            !allZero(record + INFERNO_METAL_RESOURCE_LIBRARY_RESERVED_OFFSET,
                     INFERNO_METAL_RESOURCE_LIBRARY_RESERVED_SIZE) ||
            (kind == INFERNO_METAL_RESOURCE_LIBRARY_SOURCE &&
             !validUtf8(bytes + payload_offset + offset, size))) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_LIBRARY, i);
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *prior =
                bytes + libraries_offset +
                (size_t)j * INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE;
            uint32_t prior_kind =
                get32(prior + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET);
            uint32_t prior_offset =
                get32(prior + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET);
            uint32_t prior_size = get32(
                prior + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET);
            if (kind == prior_kind && size == prior_size &&
                !memcmp(bytes + payload_offset + offset,
                        bytes + payload_offset + prior_offset, size)) {
                return fail(error, INFERNO_METAL_RESOURCE_RECORD_LIBRARY, i);
            }
        }
        expected_offset += size;
    }
    if (expected_offset != payload_size) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_HEADER, 0);
    }
    return true;
}

static bool validComputePipeline(const uint8_t *record, uint32_t library_count)
{
    return get32(record +
                 INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET) <
               library_count &&
           !get32(record +
                  INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_FLAGS_OFFSET) &&
           allZero(record +
                       INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RESERVED_OFFSET,
                   INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RESERVED_SIZE) &&
           validName(record +
                     INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_NAME_OFFSET);
}

static bool validRenderPipeline(const uint8_t *record, uint32_t library_count)
{
    return get32(record +
                 INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_LIBRARY_OFFSET) <
               library_count &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_LIBRARY_OFFSET) <
               library_count &&
           validPixelFormat(get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_PIXEL_FORMAT_OFFSET)) &&
           get32(record +
                 INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SAMPLE_COUNT_OFFSET) ==
               1 &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_BLENDING_ENABLED_OFFSET) <=
               1 &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SOURCE_RGB_FACTOR_OFFSET) <=
               18 &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_DESTINATION_RGB_FACTOR_OFFSET) <=
               18 &&
           get32(record +
                 INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RGB_OPERATION_OFFSET) <=
               4 &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SOURCE_ALPHA_FACTOR_OFFSET) <=
               18 &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_DESTINATION_ALPHA_FACTOR_OFFSET) <=
               18 &&
           get32(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_ALPHA_OPERATION_OFFSET) <=
               4 &&
           get32(record +
                 INFERNO_METAL_RESOURCE_RENDER_PIPELINE_WRITE_MASK_OFFSET) <=
               15 &&
           !get32(record +
                  INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FLAGS_OFFSET) &&
           allZero(record +
                       INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RESERVED_OFFSET,
                   INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RESERVED_SIZE) &&
           validName(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_NAME_OFFSET) &&
           validName(
               record +
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_NAME_OFFSET);
}

static bool validSampler(const uint8_t *record)
{
    uint32_t min_filter =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_MIN_FILTER_OFFSET);
    uint32_t mag_filter =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_MAG_FILTER_OFFSET);
    uint32_t mip_filter =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_MIP_FILTER_OFFSET);
    uint32_t anisotropy =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_MAX_ANISOTROPY_OFFSET);
    uint32_t s_mode =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_S_ADDRESS_MODE_OFFSET);
    uint32_t t_mode =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_T_ADDRESS_MODE_OFFSET);
    uint32_t r_mode =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_R_ADDRESS_MODE_OFFSET);
    uint32_t normalized = get32(
        record + INFERNO_METAL_RESOURCE_SAMPLER_NORMALIZED_COORDINATES_OFFSET);
    uint32_t min_bits =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_MIN_CLAMP_OFFSET);
    uint32_t max_bits =
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_MAX_CLAMP_OFFSET);
    if (min_filter > INFERNO_METAL_RESOURCE_FILTER_LINEAR ||
        mag_filter > INFERNO_METAL_RESOURCE_FILTER_LINEAR ||
        mip_filter > INFERNO_METAL_RESOURCE_MIP_FILTER_LINEAR || !anisotropy ||
        anisotropy > 16 ||
        s_mode > INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR ||
        t_mode > INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR ||
        r_mode > INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR ||
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_BORDER_COLOR_OFFSET) >
            INFERNO_METAL_RESOURCE_BORDER_OPAQUE_WHITE ||
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_REDUCTION_MODE_OFFSET) !=
            INFERNO_METAL_RESOURCE_REDUCTION_WEIGHTED_AVERAGE ||
        normalized > 1 || !finite32(min_bits) || !finite32(max_bits) ||
        getFloat32(record +
                   INFERNO_METAL_RESOURCE_SAMPLER_LOD_MIN_CLAMP_OFFSET) < 0 ||
        getFloat32(record +
                   INFERNO_METAL_RESOURCE_SAMPLER_LOD_MAX_CLAMP_OFFSET) <
            getFloat32(record +
                       INFERNO_METAL_RESOURCE_SAMPLER_LOD_MIN_CLAMP_OFFSET) ||
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_AVERAGE_OFFSET) > 1 ||
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_BIAS_OFFSET) ||
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_COMPARE_FUNCTION_OFFSET) >
            INFERNO_METAL_RESOURCE_COMPARE_ALWAYS ||
        get32(record +
              INFERNO_METAL_RESOURCE_SAMPLER_SUPPORT_ARGUMENT_BUFFERS_OFFSET) >
            1 ||
        get32(record + INFERNO_METAL_RESOURCE_SAMPLER_FLAGS_OFFSET) ||
        !allZero(record + INFERNO_METAL_RESOURCE_SAMPLER_RESERVED_OFFSET,
                 INFERNO_METAL_RESOURCE_SAMPLER_RESERVED_SIZE)) {
        return false;
    }
    return normalized ||
           (s_mode == INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
            t_mode == INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
            r_mode == INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
            mip_filter == INFERNO_METAL_RESOURCE_MIP_FILTER_NOT_MIPMAPPED &&
            min_filter == mag_filter && anisotropy == 1);
}

static bool validResourceBindings(
    const InfernoMetalResourceBatchView *view, uint32_t start, uint32_t count,
    uint32_t allowed_kinds, uint32_t *expected_inline, bool *referenced_buffers,
    bool *referenced_textures, bool *referenced_samplers,
    bool *referenced_arguments, uint32_t forbidden_texture,
    uint32_t compute_pipeline, InfernoMetalBatchParseError *error)
{
    uint32_t prior_index = 0, prior_kind = 0;
    bool have_prior = false;
    bool have_slot_binding = false;
    if (start > view->binding_count || count > view->binding_count - start) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t record_index = start + i;
        const uint8_t *record =
            inferno_metal_resource_batch_binding(view, record_index);
        uint32_t kind =
            get32(record + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET);
        uint32_t index =
            get32(record + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET);
        uint32_t resource =
            get32(record + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET);
        uint32_t length =
            get32(record + INFERNO_METAL_RESOURCE_BINDING_LENGTH_OFFSET);
        uint64_t offset =
            get64(record + INFERNO_METAL_RESOURCE_BINDING_OFFSET_OFFSET);
        bool ordered = !have_prior || index > prior_index ||
                       (index == prior_index && kind > prior_kind);
        bool same_index = have_prior && index == prior_index;
        if (!same_index) {
            have_slot_binding = false;
        }
        bool slot_binding = kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER ||
                            kind == INFERNO_METAL_RESOURCE_BINDING_INLINE ||
                            kind == INFERNO_METAL_RESOURCE_BINDING_ARGUMENT;
        bool duplicate = same_index && ((slot_binding && have_slot_binding) ||
                                        kind == prior_kind);
        bool valid =
            kind >= INFERNO_METAL_RESOURCE_BINDING_BUFFER &&
            kind <= INFERNO_METAL_RESOURCE_BINDING_ARGUMENT &&
            (allowed_kinds & (1U << kind)) && ordered && !duplicate &&
            !get64(record + INFERNO_METAL_RESOURCE_BINDING_RESERVED_OFFSET);
        if (kind == INFERNO_METAL_RESOURCE_BINDING_SAMPLER) {
            valid = valid && index <= 15;
        } else {
            valid = valid && index <= 30;
        }
        if (kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
            valid = valid && resource < view->buffer_count && !length &&
                    !(offset & 3);
            if (valid) {
                uint32_t buffer_length =
                    get32(inferno_metal_resource_batch_buffer(view, resource) +
                          INFERNO_METAL_RESOURCE_BUFFER_LENGTH_OFFSET);
                valid = offset < buffer_length;
            }
            if (valid) {
                referenced_buffers[resource] = true;
            }
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_INLINE) {
            valid = valid && !offset && length &&
                    length <= INFERNO_METAL_RESOURCE_MAX_INLINE_BINDING &&
                    resource == *expected_inline &&
                    resource <= view->inline_size &&
                    length <= view->inline_size - resource;
            if (valid) {
                *expected_inline += length;
            }
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_THREADGROUP) {
            valid = valid && !resource && !offset && length &&
                    length <= INFERNO_METAL_RESOURCE_MAX_THREADGROUP_MEMORY &&
                    !(length & 15);
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_TEXTURE) {
            valid = valid && resource < view->texture_count && !length &&
                    !offset && resource != forbidden_texture;
            if (valid) {
                referenced_textures[resource] = true;
            }
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_SAMPLER) {
            valid =
                valid && resource < view->sampler_count && !length && !offset;
            if (valid) {
                referenced_samplers[resource] = true;
            }
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_ARGUMENT) {
            valid = valid && resource < view->argument_count && !length &&
                    !offset && compute_pipeline < view->compute_pipeline_count;
            if (valid) {
                const uint8_t *argument =
                    inferno_metal_resource_batch_argument(view, resource);
                const uint8_t *pipeline =
                    inferno_metal_resource_batch_compute_pipeline(
                        view, compute_pipeline);
                uint32_t library = get32(
                    pipeline +
                    INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET);
                valid =
                    get32(
                        argument +
                        INFERNO_METAL_RESOURCE_ARGUMENT_BUFFER_INDEX_OFFSET) ==
                        index &&
                    get32(argument +
                          INFERNO_METAL_RESOURCE_ARGUMENT_LIBRARY_OFFSET) ==
                        library &&
                    !memcmp(
                        argument + INFERNO_METAL_RESOURCE_ARGUMENT_NAME_OFFSET,
                        pipeline +
                            INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_NAME_OFFSET,
                        INFERNO_METAL_RESOURCE_FUNCTION_NAME_SIZE);
            }
            if (valid) {
                referenced_arguments[resource] = true;
            }
        }
        if (!valid) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_BINDING,
                        record_index);
        }
        prior_index = index;
        prior_kind = kind;
        have_prior = true;
        have_slot_binding |= slot_binding;
    }
    return true;
}

static bool validDrawState(const uint8_t *record, uint32_t attachment_width,
                           uint32_t attachment_height)
{
    uint32_t flags = get32(record + INFERNO_METAL_RESOURCE_DRAW_FLAGS_OFFSET);
    if (get32(record + INFERNO_METAL_RESOURCE_DRAW_PRIMITIVE_TYPE_OFFSET) >
            INFERNO_METAL_RESOURCE_PRIMITIVE_TRIANGLE_STRIP ||
        !get32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_COUNT_OFFSET) ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_COUNT_OFFSET) >
            INFERNO_METAL_MAX_THREADS ||
        !get32(record + INFERNO_METAL_RESOURCE_DRAW_INSTANCE_COUNT_OFFSET) ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_INSTANCE_COUNT_OFFSET) >
            INFERNO_METAL_MAX_THREADS ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_VERTEX_COUNT_OFFSET) >
            INFERNO_METAL_MAX_THREADS /
                get32(record +
                      INFERNO_METAL_RESOURCE_DRAW_INSTANCE_COUNT_OFFSET) ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_BASE_INSTANCE_OFFSET) ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_CULL_MODE_OFFSET) >
            INFERNO_METAL_RESOURCE_CULL_BACK ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_WINDING_OFFSET) >
            INFERNO_METAL_RESOURCE_WINDING_COUNTER_CLOCKWISE ||
        get32(record + INFERNO_METAL_RESOURCE_DRAW_FILL_MODE_OFFSET) >
            INFERNO_METAL_RESOURCE_FILL_MODE_LINES ||
        flags & ~INFERNO_METAL_RESOURCE_DRAW_FLAG_MASK ||
        !allZero(record + INFERNO_METAL_RESOURCE_DRAW_RESERVED_OFFSET,
                 INFERNO_METAL_RESOURCE_DRAW_RESERVED_SIZE)) {
        return false;
    }
    const uint8_t *scissor =
        record + INFERNO_METAL_RESOURCE_DRAW_SCISSOR_OFFSET;
    if (flags & INFERNO_METAL_RESOURCE_DRAW_SCISSOR) {
        uint32_t x = get32(scissor), y = get32(scissor + 4);
        uint32_t width = get32(scissor + 8), height = get32(scissor + 12);
        if (!width || !height || x > attachment_width ||
            width > attachment_width - x || y > attachment_height ||
            height > attachment_height - y) {
            return false;
        }
    } else if (!allZero(scissor, 16)) {
        return false;
    }
    const uint8_t *viewport =
        record + INFERNO_METAL_RESOURCE_DRAW_VIEWPORT_OFFSET;
    if (flags & INFERNO_METAL_RESOURCE_DRAW_VIEWPORT) {
        for (unsigned i = 0; i < 6; i++) {
            if (!finite64(get64(viewport + i * 8))) {
                return false;
            }
        }
        double x = getFloat64(viewport), y = getFloat64(viewport + 8);
        double width = getFloat64(viewport + 16);
        double height = getFloat64(viewport + 24);
        double znear = getFloat64(viewport + 32);
        double zfar = getFloat64(viewport + 40);
        if (x < 0 || y < 0 || width < 0 || height < 0 || x > attachment_width ||
            width > attachment_width - x || y > attachment_height ||
            height > attachment_height - y || znear < 0 || znear > zfar ||
            zfar > 1) {
            return false;
        }
    } else if (!allZero(viewport, 48)) {
        return false;
    }
    const uint8_t *blend =
        record + INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR_OFFSET;
    if (flags & INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR) {
        for (unsigned i = 0; i < 4; i++) {
            if (!finite32(get32(blend + i * 4))) {
                return false;
            }
        }
    } else if (!allZero(blend, 16)) {
        return false;
    }
    return true;
}

bool inferno_metal_resource_batch_parse(const void *opaque, size_t size,
                                        size_t output_size,
                                        InfernoMetalResourceBatchView *out,
                                        InfernoMetalBatchParseError *error)
{
    const uint8_t *bytes = opaque;
    InfernoMetalResourceBatchView view = { 0 };
    if (error) {
        *error = (InfernoMetalBatchParseError){
            .record_kind = INFERNO_METAL_BATCH_RECORD_HEADER,
            .record_index = 0,
        };
    }
    if (!bytes || !out || size < INFERNO_METAL_RESOURCE_HEADER_SIZE ||
        size > INFERNO_METAL_MAX_BUFFER ||
        output_size > INFERNO_METAL_MAX_BUFFER ||
        get32(bytes + INFERNO_METAL_RESOURCE_VERSION_OFFSET) !=
            INFERNO_METAL_RESOURCE_VERSION ||
        get32(bytes + INFERNO_METAL_RESOURCE_FLAGS_OFFSET) ||
        !allZero(bytes + INFERNO_METAL_RESOURCE_RESERVED_OFFSET,
                 INFERNO_METAL_RESOURCE_RESERVED_SIZE)) {
        return false;
    }
    view = (InfernoMetalResourceBatchView){
        .bytes = bytes,
        .size = size,
        .library_count =
            get32(bytes + INFERNO_METAL_RESOURCE_LIBRARY_COUNT_OFFSET),
        .compute_pipeline_count =
            get32(bytes + INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_COUNT_OFFSET),
        .render_pipeline_count =
            get32(bytes + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_COUNT_OFFSET),
        .buffer_count =
            get32(bytes + INFERNO_METAL_RESOURCE_BUFFER_COUNT_OFFSET),
        .texture_count =
            get32(bytes + INFERNO_METAL_RESOURCE_TEXTURE_COUNT_OFFSET),
        .sampler_count =
            get32(bytes + INFERNO_METAL_RESOURCE_SAMPLER_COUNT_OFFSET),
        .command_count =
            get32(bytes + INFERNO_METAL_RESOURCE_HEADER_COMMAND_COUNT_OFFSET),
        .draw_count = get32(bytes + INFERNO_METAL_RESOURCE_DRAW_COUNT_OFFSET),
        .binding_count =
            get32(bytes + INFERNO_METAL_RESOURCE_BINDING_COUNT_OFFSET),
        .argument_count =
            get32(bytes + INFERNO_METAL_RESOURCE_ARGUMENT_COUNT_OFFSET),
        .member_count =
            get32(bytes + INFERNO_METAL_RESOURCE_MEMBER_COUNT_OFFSET),
        .declaration_count =
            get32(bytes + INFERNO_METAL_RESOURCE_DECLARATION_COUNT_OFFSET),
        .inline_size = get32(bytes + INFERNO_METAL_RESOURCE_INLINE_SIZE_OFFSET),
        .payload_size =
            get32(bytes + INFERNO_METAL_RESOURCE_PAYLOAD_SIZE_OFFSET),
        .constants_size =
            get32(bytes + INFERNO_METAL_RESOURCE_CONSTANTS_SIZE_OFFSET),
        .images_size = get32(bytes + INFERNO_METAL_RESOURCE_IMAGES_SIZE_OFFSET),
        .libraries_offset = INFERNO_METAL_RESOURCE_HEADER_SIZE,
    };
    bool empty =
        !view.library_count && !view.compute_pipeline_count &&
        !view.render_pipeline_count && !view.buffer_count &&
        !view.texture_count && !view.sampler_count && !view.command_count &&
        !view.draw_count && !view.binding_count && !view.argument_count &&
        !view.member_count && !view.declaration_count && !view.inline_size &&
        !view.payload_size && !view.constants_size && !view.images_size;
    if (view.library_count > INFERNO_METAL_RESOURCE_MAX_LIBRARIES ||
        view.compute_pipeline_count >
            INFERNO_METAL_RESOURCE_MAX_COMPUTE_PIPELINES ||
        view.render_pipeline_count >
            INFERNO_METAL_RESOURCE_MAX_RENDER_PIPELINES ||
        view.buffer_count > INFERNO_METAL_RESOURCE_MAX_BUFFERS ||
        view.texture_count > INFERNO_METAL_RESOURCE_MAX_TEXTURES ||
        view.sampler_count > INFERNO_METAL_RESOURCE_MAX_SAMPLERS ||
        view.command_count > INFERNO_METAL_RESOURCE_MAX_COMMANDS ||
        view.draw_count > INFERNO_METAL_RESOURCE_MAX_DRAWS ||
        view.binding_count > INFERNO_METAL_RESOURCE_MAX_BINDINGS ||
        view.argument_count > INFERNO_METAL_RESOURCE_MAX_ARGUMENTS ||
        view.member_count > INFERNO_METAL_RESOURCE_MAX_MEMBERS ||
        view.declaration_count > INFERNO_METAL_RESOURCE_MAX_DECLARATIONS ||
        view.inline_size > INFERNO_METAL_RESOURCE_MAX_INLINE ||
        view.payload_size > INFERNO_METAL_RESOURCE_MAX_PAYLOAD ||
        view.constants_size > INFERNO_METAL_ARGUMENT_MAX_CONSTANTS ||
        view.images_size > INFERNO_METAL_RESOURCE_MAX_IMAGES ||
        (!empty && !view.command_count) ||
        output_size != INFERNO_METAL_RESOURCE_RESULT_SIZE + view.images_size) {
        return false;
    }

    size_t offset = view.libraries_offset;
    if (!addRegion(&offset, view.library_count,
                   INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE, size)) {
        return false;
    }
    view.compute_pipelines_offset = offset;
    if (!addRegion(&offset, view.compute_pipeline_count,
                   INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE, size)) {
        return false;
    }
    view.render_pipelines_offset = offset;
    if (!addRegion(&offset, view.render_pipeline_count,
                   INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE, size)) {
        return false;
    }
    view.buffers_offset = offset;
    if (!addRegion(&offset, view.buffer_count,
                   INFERNO_METAL_RESOURCE_BUFFER_RECORD_SIZE, size)) {
        return false;
    }
    view.textures_offset = offset;
    if (!addRegion(&offset, view.texture_count,
                   INFERNO_METAL_RESOURCE_TEXTURE_RECORD_SIZE, size)) {
        return false;
    }
    view.samplers_offset = offset;
    if (!addRegion(&offset, view.sampler_count,
                   INFERNO_METAL_RESOURCE_SAMPLER_RECORD_SIZE, size)) {
        return false;
    }
    view.commands_offset = offset;
    if (!addRegion(&offset, view.command_count,
                   INFERNO_METAL_RESOURCE_COMMAND_RECORD_SIZE, size)) {
        return false;
    }
    view.draws_offset = offset;
    if (!addRegion(&offset, view.draw_count,
                   INFERNO_METAL_RESOURCE_DRAW_RECORD_SIZE, size)) {
        return false;
    }
    view.bindings_offset = offset;
    if (!addRegion(&offset, view.binding_count,
                   INFERNO_METAL_RESOURCE_BINDING_RECORD_SIZE, size)) {
        return false;
    }
    view.arguments_offset = offset;
    if (!addRegion(&offset, view.argument_count,
                   INFERNO_METAL_RESOURCE_ARGUMENT_RECORD_SIZE, size)) {
        return false;
    }
    view.members_offset = offset;
    if (!addRegion(&offset, view.member_count,
                   INFERNO_METAL_RESOURCE_MEMBER_RECORD_SIZE, size)) {
        return false;
    }
    view.declarations_offset = offset;
    if (!addRegion(&offset, view.declaration_count,
                   INFERNO_METAL_RESOURCE_DECLARATION_RECORD_SIZE, size)) {
        return false;
    }
    view.payload_offset = offset;
    if (!addRegion(&offset, view.payload_size, 1, size)) {
        return false;
    }
    view.inline_offset = offset;
    if (!addRegion(&offset, view.inline_size, 1, size)) {
        return false;
    }
    view.constants_offset = offset;
    if (!addRegion(&offset, view.constants_size, 1, size)) {
        return false;
    }
    view.images_offset = offset;
    if (!addRegion(&offset, view.images_size, 1, size) || offset != size) {
        return false;
    }

    if (!validResourceLibraries(bytes, view.libraries_offset,
                                view.library_count, view.payload_offset,
                                view.payload_size, error)) {
        return false;
    }

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

    for (uint32_t i = 0; i < view.compute_pipeline_count; i++) {
        const uint8_t *record =
            inferno_metal_resource_batch_compute_pipeline(&view, i);
        if (!validComputePipeline(record, view.library_count)) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_PIPELINE, i);
        }
        for (uint32_t j = 0; j < i; j++) {
            if (!memcmp(record,
                        inferno_metal_resource_batch_compute_pipeline(&view, j),
                        INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE)) {
                return fail(error, INFERNO_METAL_BATCH_RECORD_PIPELINE, i);
            }
        }
        referenced_libraries[get32(
            record + INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET)] =
            true;
    }
    for (uint32_t i = 0; i < view.render_pipeline_count; i++) {
        const uint8_t *record =
            inferno_metal_resource_batch_render_pipeline(&view, i);
        if (!validRenderPipeline(record, view.library_count)) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE,
                        i);
        }
        for (uint32_t j = 0; j < i; j++) {
            if (!memcmp(record,
                        inferno_metal_resource_batch_render_pipeline(&view, j),
                        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE)) {
                return fail(error,
                            INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE, i);
            }
        }
        referenced_libraries[get32(
            record +
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_LIBRARY_OFFSET)] =
            true;
        referenced_libraries[get32(
            record +
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_LIBRARY_OFFSET)] =
            true;
    }

    uint64_t image_sum = 0;
    for (uint32_t i = 0; i < view.buffer_count; i++) {
        const uint8_t *record = inferno_metal_resource_batch_buffer(&view, i);
        uint32_t length =
            get32(record + INFERNO_METAL_RESOURCE_BUFFER_LENGTH_OFFSET);
        if (!length || length > INFERNO_METAL_BATCH_MAX_BUFFER_LENGTH ||
            get32(record + INFERNO_METAL_RESOURCE_BUFFER_FLAGS_OFFSET) ||
            !allZero(record + INFERNO_METAL_RESOURCE_BUFFER_RESERVED_OFFSET,
                     INFERNO_METAL_RESOURCE_BUFFER_RESERVED_SIZE)) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_BUFFER, i);
        }
        image_sum += length;
    }
    for (uint32_t i = 0; i < view.texture_count; i++) {
        const uint8_t *record = inferno_metal_resource_batch_texture(&view, i);
        uint32_t width =
            get32(record + INFERNO_METAL_RESOURCE_TEXTURE_WIDTH_OFFSET);
        uint32_t height =
            get32(record + INFERNO_METAL_RESOURCE_TEXTURE_HEIGHT_OFFSET);
        uint32_t usage =
            get32(record + INFERNO_METAL_RESOURCE_TEXTURE_USAGE_OFFSET);
        if (!width || width > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
            !height || height > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
            !validPixelFormat(get32(
                record + INFERNO_METAL_RESOURCE_TEXTURE_PIXEL_FORMAT_OFFSET)) ||
            !usage || usage & ~INFERNO_METAL_RESOURCE_TEXTURE_USAGE_MASK ||
            get32(record + INFERNO_METAL_RESOURCE_TEXTURE_FLAGS_OFFSET) &
                ~INFERNO_METAL_RESOURCE_TEXTURE_FLAG_MASK ||
            !allZero(record + INFERNO_METAL_RESOURCE_TEXTURE_RESERVED_OFFSET,
                     INFERNO_METAL_RESOURCE_TEXTURE_RESERVED_SIZE)) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_TEXTURE, i);
        }
        uint64_t texture_size = (uint64_t)width * height *
                                INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL;
        if (texture_size > INFERNO_METAL_RESOURCE_MAX_IMAGES ||
            image_sum > INFERNO_METAL_RESOURCE_MAX_IMAGES - texture_size) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_TEXTURE, i);
        }
        image_sum += texture_size;
    }
    if (image_sum != view.images_size) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_HEADER, 0);
    }
    for (uint32_t i = 0; i < view.sampler_count; i++) {
        if (!validSampler(inferno_metal_resource_batch_sampler(&view, i))) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_SAMPLER, i);
        }
    }

    uint32_t expected_member = 0;
    uint32_t expected_constant = 0;
    for (uint32_t i = 0; i < view.argument_count; i++) {
        const uint8_t *argument =
            inferno_metal_resource_batch_argument(&view, i);
        uint32_t member_start = get32(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_START_OFFSET);
        uint32_t member_count = get32(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_COUNT_OFFSET);
        uint32_t encoded_length = get32(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_ENCODED_LENGTH_OFFSET);
        uint32_t alignment =
            get32(argument + INFERNO_METAL_RESOURCE_ARGUMENT_ALIGNMENT_OFFSET);
        if (get32(argument + INFERNO_METAL_RESOURCE_ARGUMENT_LIBRARY_OFFSET) >=
                view.library_count ||
            get32(argument +
                  INFERNO_METAL_RESOURCE_ARGUMENT_BUFFER_INDEX_OFFSET) > 30 ||
            member_start != expected_member || !member_count ||
            member_count > INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS ||
            expected_member > view.member_count ||
            member_count > view.member_count - expected_member ||
            !encoded_length ||
            encoded_length > INFERNO_METAL_ARGUMENT_MAX_ENCODED_LENGTH ||
            !alignment || alignment > 4096 || (alignment & (alignment - 1)) ||
            get32(argument + INFERNO_METAL_RESOURCE_ARGUMENT_FLAGS_OFFSET) ||
            !allZero(argument + INFERNO_METAL_RESOURCE_ARGUMENT_RESERVED_OFFSET,
                     INFERNO_METAL_RESOURCE_ARGUMENT_RESERVED_SIZE) ||
            !validName(argument +
                       INFERNO_METAL_RESOURCE_ARGUMENT_NAME_OFFSET)) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, i);
        }
        uint32_t prior_id = 0;
        for (uint32_t j = 0; j < member_count; j++) {
            uint32_t member_index = member_start + j;
            const uint8_t *member =
                inferno_metal_resource_batch_member(&view, member_index);
            uint32_t kind =
                get32(member + INFERNO_METAL_RESOURCE_MEMBER_KIND_OFFSET);
            uint32_t id =
                get32(member + INFERNO_METAL_RESOURCE_MEMBER_ID_OFFSET);
            uint32_t resource =
                get32(member + INFERNO_METAL_RESOURCE_MEMBER_RESOURCE_OFFSET);
            uint32_t length =
                get32(member + INFERNO_METAL_RESOURCE_MEMBER_LENGTH_OFFSET);
            uint64_t member_offset =
                get64(member + INFERNO_METAL_RESOURCE_MEMBER_OFFSET_OFFSET);
            bool valid =
                (!j || id > prior_id) &&
                !get64(member + INFERNO_METAL_RESOURCE_MEMBER_RESERVED_OFFSET);
            if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER) {
                valid = valid && resource < view.buffer_count && !length;
                if (valid) {
                    uint32_t buffer_length = get32(
                        inferno_metal_resource_batch_buffer(&view, resource) +
                        INFERNO_METAL_RESOURCE_BUFFER_LENGTH_OFFSET);
                    valid = member_offset < buffer_length;
                }
                if (valid) {
                    referenced_buffers[resource] = true;
                }
            } else if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE) {
                valid = valid && resource < view.texture_count && !length &&
                        !member_offset;
                if (valid) {
                    referenced_textures[resource] = true;
                }
            } else if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER) {
                valid =
                    valid && resource < view.sampler_count && !length &&
                    !member_offset &&
                    get32(
                        inferno_metal_resource_batch_sampler(&view, resource) +
                        INFERNO_METAL_RESOURCE_SAMPLER_SUPPORT_ARGUMENT_BUFFERS_OFFSET);
                if (valid) {
                    referenced_samplers[resource] = true;
                }
            } else if (kind ==
                       INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
                valid = valid && resource == expected_constant && length &&
                        length <= INFERNO_METAL_ARGUMENT_MAX_CONSTANT &&
                        !member_offset && resource <= view.constants_size &&
                        length <= view.constants_size - resource;
                if (valid) {
                    expected_constant += length;
                }
            } else {
                valid = false;
            }
            if (!valid) {
                return fail(error, INFERNO_METAL_RESOURCE_RECORD_MEMBER,
                            member_index);
            }
            prior_id = id;
        }
        referenced_libraries[get32(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_LIBRARY_OFFSET)] = true;
        expected_member += member_count;
    }
    if (expected_member != view.member_count ||
        expected_constant != view.constants_size) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_HEADER, 0);
    }

    uint32_t expected_binding = 0, expected_draw = 0, expected_inline = 0;
    uint32_t expected_declaration = 0;
    for (uint32_t i = 0; i < view.command_count; i++) {
        const uint8_t *record = inferno_metal_resource_batch_command(&view, i);
        uint32_t kind =
            get32(record + INFERNO_METAL_RESOURCE_COMMAND_KIND_OFFSET);
        if (get32(record + INFERNO_METAL_RESOURCE_COMMAND_FLAGS_OFFSET)) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_COMMAND, i);
        }
        if (kind == INFERNO_METAL_RESOURCE_COMMAND_COMPUTE) {
            uint32_t pipeline =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_RESOURCE_OFFSET);
            uint32_t mode =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_ACTION_OFFSET);
            uint32_t start = get32(
                record + INFERNO_METAL_RESOURCE_COMMAND_COUNT_OR_ACTION_OFFSET);
            uint32_t count =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_START_OFFSET);
            uint32_t declaration_start = get32(
                record +
                INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_START_OFFSET);
            uint32_t declaration_count = get32(
                record +
                INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_COUNT_OFFSET);
            uint32_t dimensions[6] = {
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_COUNT_OFFSET),
                get32(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_HEIGHT_OFFSET),
                get32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_DEPTH_OFFSET),
                get32(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_WIDTH_OFFSET),
                get32(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_HEIGHT_OFFSET),
                get32(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_DEPTH_OFFSET),
            };
            uint64_t grid_product = 0, group_product = 0;
            bool grid_valid =
                checkedProduct(dimensions[0], dimensions[1], dimensions[2],
                               INFERNO_METAL_MAX_THREADS, &grid_product);
            bool group_valid =
                checkedProduct(dimensions[3], dimensions[4], dimensions[5],
                               INFERNO_METAL_MAX_THREADS, &group_product);
            if (pipeline >= view.compute_pipeline_count ||
                (mode != INFERNO_METAL_BATCH_DISPATCH_THREADS &&
                 mode != INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS) ||
                start != expected_binding || start > view.binding_count ||
                count > view.binding_count - start || !grid_valid ||
                !group_valid ||
                (mode == INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS &&
                 grid_product > INFERNO_METAL_MAX_THREADS / group_product) ||
                !allZero(
                    record +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_RESERVED_OFFSET,
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_RESERVED_SIZE)) {
                return fail(error, INFERNO_METAL_RESOURCE_RECORD_COMMAND, i);
            }
            if (!validResourceBindings(
                    &view, start, count,
                    (1U << INFERNO_METAL_RESOURCE_BINDING_BUFFER) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_INLINE) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_THREADGROUP) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_TEXTURE) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_SAMPLER) |
                        (1U << INFERNO_METAL_RESOURCE_BINDING_ARGUMENT),
                    &expected_inline, referenced_buffers, referenced_textures,
                    referenced_samplers, referenced_arguments, UINT32_MAX,
                    pipeline, error) ||
                declaration_start != expected_declaration ||
                declaration_start > view.declaration_count ||
                declaration_count >
                    view.declaration_count - declaration_start) {
                return false;
            }
            uint32_t prior_resource_kind = 0, prior_resource = 0;
            for (uint32_t j = 0; j < declaration_count; j++) {
                uint32_t declaration_index = declaration_start + j;
                const uint8_t *declaration =
                    inferno_metal_resource_batch_declaration(&view,
                                                             declaration_index);
                uint32_t resource_kind = get32(
                    declaration +
                    INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_KIND_OFFSET);
                uint32_t resource =
                    get32(declaration +
                          INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_OFFSET);
                uint32_t usage =
                    get32(declaration +
                          INFERNO_METAL_RESOURCE_DECLARATION_USAGE_OFFSET);
                bool ordered = !j || resource_kind > prior_resource_kind ||
                               (resource_kind == prior_resource_kind &&
                                resource > prior_resource);
                bool valid =
                    ordered && usage &&
                    !(usage & ~INFERNO_METAL_RESOURCE_USAGE_MASK) &&
                    !get32(declaration +
                           INFERNO_METAL_RESOURCE_DECLARATION_RESERVED_OFFSET);
                if (resource_kind ==
                    INFERNO_METAL_RESOURCE_DECLARATION_BUFFER) {
                    valid = valid && resource < view.buffer_count;
                    if (valid) {
                        referenced_buffers[resource] = true;
                    }
                } else if (resource_kind ==
                           INFERNO_METAL_RESOURCE_DECLARATION_TEXTURE) {
                    valid = valid && resource < view.texture_count;
                    if (valid) {
                        referenced_textures[resource] = true;
                    }
                } else {
                    valid = false;
                }
                if (!valid) {
                    return fail(error,
                                INFERNO_METAL_RESOURCE_RECORD_DECLARATION,
                                declaration_index);
                }
                prior_resource_kind = resource_kind;
                prior_resource = resource;
            }
            referenced_compute[pipeline] = true;
            expected_binding += count;
            expected_declaration += declaration_count;
        } else if (kind == INFERNO_METAL_RESOURCE_COMMAND_RENDER) {
            uint32_t texture =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_RESOURCE_OFFSET);
            uint32_t load =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_ACTION_OFFSET);
            uint32_t store = get32(
                record + INFERNO_METAL_RESOURCE_COMMAND_COUNT_OR_ACTION_OFFSET);
            uint32_t draw_start =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_START_OFFSET);
            uint32_t draw_count =
                get32(record + INFERNO_METAL_RESOURCE_COMMAND_COUNT_OFFSET);
            if (texture >= view.texture_count ||
                !(get32(inferno_metal_resource_batch_texture(&view, texture) +
                        INFERNO_METAL_RESOURCE_TEXTURE_USAGE_OFFSET) &
                  INFERNO_METAL_RESOURCE_TEXTURE_USAGE_RENDER_TARGET) ||
                load > INFERNO_METAL_RESOURCE_LOAD_CLEAR ||
                store != INFERNO_METAL_RESOURCE_STORE_STORE ||
                draw_start != expected_draw || draw_start > view.draw_count ||
                draw_count > view.draw_count - draw_start ||
                get32(record +
                      INFERNO_METAL_RESOURCE_COMMAND_RENDER_UNUSED_OFFSET) ||
                !finite64(get64(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_RED_OFFSET)) ||
                !finite64(get64(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_GREEN_OFFSET)) ||
                !finite64(get64(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_BLUE_OFFSET)) ||
                !finite64(get64(
                    record +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_ALPHA_OFFSET)) ||
                !allZero(
                    record +
                        INFERNO_METAL_RESOURCE_COMMAND_RENDER_RESERVED_OFFSET,
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_RESERVED_SIZE)) {
                return fail(error, INFERNO_METAL_RESOURCE_RECORD_COMMAND, i);
            }
            referenced_textures[texture] = true;
            const uint8_t *texture_record =
                inferno_metal_resource_batch_texture(&view, texture);
            uint32_t attachment_width = get32(
                texture_record + INFERNO_METAL_RESOURCE_TEXTURE_WIDTH_OFFSET);
            uint32_t attachment_height = get32(
                texture_record + INFERNO_METAL_RESOURCE_TEXTURE_HEIGHT_OFFSET);
            for (uint32_t j = 0; j < draw_count; j++) {
                uint32_t draw_index = draw_start + j;
                const uint8_t *draw =
                    inferno_metal_resource_batch_draw(&view, draw_index);
                uint32_t pipeline =
                    get32(draw + INFERNO_METAL_RESOURCE_DRAW_PIPELINE_OFFSET);
                const uint8_t *pipeline_record =
                    pipeline < view.render_pipeline_count ?
                        inferno_metal_resource_batch_render_pipeline(&view,
                                                                     pipeline) :
                        NULL;
                uint32_t vertex_start = get32(
                    draw +
                    INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_START_OFFSET);
                uint32_t vertex_count = get32(
                    draw +
                    INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_COUNT_OFFSET);
                uint32_t fragment_start = get32(
                    draw +
                    INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_START_OFFSET);
                uint32_t fragment_count = get32(
                    draw +
                    INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_COUNT_OFFSET);
                uint32_t render_kinds =
                    (1U << INFERNO_METAL_RESOURCE_BINDING_BUFFER) |
                    (1U << INFERNO_METAL_RESOURCE_BINDING_INLINE) |
                    (1U << INFERNO_METAL_RESOURCE_BINDING_TEXTURE) |
                    (1U << INFERNO_METAL_RESOURCE_BINDING_SAMPLER);
                if (!pipeline_record ||
                    get32(
                        pipeline_record +
                        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_PIXEL_FORMAT_OFFSET) !=
                        get32(
                            texture_record +
                            INFERNO_METAL_RESOURCE_TEXTURE_PIXEL_FORMAT_OFFSET) ||
                    !validDrawState(draw, attachment_width,
                                    attachment_height) ||
                    vertex_start != expected_binding ||
                    vertex_start > view.binding_count ||
                    vertex_count > view.binding_count - vertex_start) {
                    return fail(error, INFERNO_METAL_RESOURCE_RECORD_DRAW,
                                draw_index);
                }
                if (!validResourceBindings(
                        &view, vertex_start, vertex_count, render_kinds,
                        &expected_inline, referenced_buffers,
                        referenced_textures, referenced_samplers, NULL, texture,
                        UINT32_MAX, error)) {
                    return false;
                }
                expected_binding += vertex_count;
                if (fragment_start != expected_binding ||
                    fragment_start > view.binding_count ||
                    fragment_count > view.binding_count - fragment_start) {
                    return fail(error, INFERNO_METAL_RESOURCE_RECORD_DRAW,
                                draw_index);
                }
                if (!validResourceBindings(
                        &view, fragment_start, fragment_count, render_kinds,
                        &expected_inline, referenced_buffers,
                        referenced_textures, referenced_samplers, NULL, texture,
                        UINT32_MAX, error)) {
                    return false;
                }
                expected_binding += fragment_count;
                referenced_render[pipeline] = true;
                referenced_draws[draw_index] = true;
            }
            expected_draw += draw_count;
        } else {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_COMMAND, i);
        }
    }
    if (expected_binding != view.binding_count ||
        expected_draw != view.draw_count ||
        expected_inline != view.inline_size ||
        expected_declaration != view.declaration_count) {
        return fail(error, INFERNO_METAL_BATCH_RECORD_HEADER, 0);
    }
#define REQUIRE_REFERENCED(count, referenced, kind) \
    do {                                            \
        for (uint32_t i = 0; i < (count); i++) {    \
            if (!(referenced)[i]) {                 \
                return fail(error, (kind), i);      \
            }                                       \
        }                                           \
    } while (0)
    REQUIRE_REFERENCED(view.library_count, referenced_libraries,
                       INFERNO_METAL_RESOURCE_RECORD_LIBRARY);
    REQUIRE_REFERENCED(view.compute_pipeline_count, referenced_compute,
                       INFERNO_METAL_BATCH_RECORD_PIPELINE);
    REQUIRE_REFERENCED(view.render_pipeline_count, referenced_render,
                       INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE);
    REQUIRE_REFERENCED(view.buffer_count, referenced_buffers,
                       INFERNO_METAL_BATCH_RECORD_BUFFER);
    REQUIRE_REFERENCED(view.texture_count, referenced_textures,
                       INFERNO_METAL_RESOURCE_RECORD_TEXTURE);
    REQUIRE_REFERENCED(view.sampler_count, referenced_samplers,
                       INFERNO_METAL_RESOURCE_RECORD_SAMPLER);
    REQUIRE_REFERENCED(view.draw_count, referenced_draws,
                       INFERNO_METAL_RESOURCE_RECORD_DRAW);
    REQUIRE_REFERENCED(view.argument_count, referenced_arguments,
                       INFERNO_METAL_RESOURCE_RECORD_ARGUMENT);
#undef REQUIRE_REFERENCED
    *out = view;
    return true;
}

bool inferno_metal_typed_query_parse(uint32_t opcode, const void *opaque,
                                     size_t size,
                                     InfernoMetalTypedQueryView *out,
                                     InfernoMetalBatchParseError *error)
{
    const uint8_t *bytes = opaque;
    InfernoMetalTypedQueryView view = { 0 };
    if (error) {
        *error = (InfernoMetalBatchParseError){
            .record_kind = INFERNO_METAL_BATCH_RECORD_HEADER,
            .record_index = 0,
        };
    }
    if (!bytes || !out || size < INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE ||
        size > INFERNO_METAL_MAX_BUFFER ||
        (opcode != INFERNO_METAL_QUERY_LIBRARY_TYPED &&
         opcode != INFERNO_METAL_QUERY_PIPELINE_TYPED &&
         opcode != INFERNO_METAL_QUERY_RENDER_PIPELINE &&
         opcode != INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED &&
         opcode != INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) ||
        get32(bytes + INFERNO_METAL_RESOURCE_QUERY_VERSION_OFFSET) !=
            INFERNO_METAL_RESOURCE_VERSION ||
        get32(bytes + INFERNO_METAL_RESOURCE_QUERY_FLAGS_OFFSET) ||
        (opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ?
             (get32(bytes +
                    INFERNO_METAL_RESOURCE_QUERY_ARGUMENT_BUFFER_INDEX_OFFSET) >
                  30 ||
              !allZero(
                  bytes + INFERNO_METAL_RESOURCE_QUERY_ARGUMENT_RESERVED_OFFSET,
                  INFERNO_METAL_RESOURCE_QUERY_ARGUMENT_RESERVED_SIZE)) :
             !allZero(bytes + INFERNO_METAL_RESOURCE_QUERY_RESERVED_OFFSET,
                      INFERNO_METAL_RESOURCE_QUERY_RESERVED_SIZE))) {
        return false;
    }
    view = (InfernoMetalTypedQueryView){
        .bytes = bytes,
        .size = size,
        .opcode = opcode,
        .library_count =
            get32(bytes + INFERNO_METAL_RESOURCE_QUERY_LIBRARY_COUNT_OFFSET),
        .payload_size =
            get32(bytes + INFERNO_METAL_RESOURCE_QUERY_PAYLOAD_SIZE_OFFSET),
        .argument_buffer_index =
            opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ?
                get32(
                    bytes +
                    INFERNO_METAL_RESOURCE_QUERY_ARGUMENT_BUFFER_INDEX_OFFSET) :
                0,
        .libraries_offset = INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE,
    };
    uint32_t required_libraries =
        opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE ? 0 : 1;
    if (view.library_count > INFERNO_METAL_RESOURCE_MAX_LIBRARIES ||
        view.payload_size > INFERNO_METAL_RESOURCE_MAX_PAYLOAD ||
        (required_libraries && view.library_count != required_libraries) ||
        (!required_libraries &&
         (view.library_count < 1 || view.library_count > 2))) {
        return false;
    }
    size_t offset = view.libraries_offset;
    if (!addRegion(&offset, view.library_count,
                   INFERNO_METAL_RESOURCE_LIBRARY_RECORD_SIZE, size)) {
        return false;
    }
    view.pipeline_offset = offset;
    if (opcode == INFERNO_METAL_QUERY_PIPELINE_TYPED ||
        opcode == INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED ||
        opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) {
        view.pipeline_size =
            INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE;
    } else if (opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE) {
        view.pipeline_size = INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE;
    }
    if (!addRegion(&offset, 1, view.pipeline_size, size)) {
        return false;
    }
    view.payload_offset = offset;
    if (!addRegion(&offset, view.payload_size, 1, size) || offset != size ||
        !validResourceLibraries(bytes, view.libraries_offset,
                                view.library_count, view.payload_offset,
                                view.payload_size, error)) {
        return false;
    }

    bool referenced[INFERNO_METAL_RESOURCE_MAX_LIBRARIES] = { false };
    const uint8_t *pipeline = inferno_metal_typed_query_pipeline(&view);
    if (view.pipeline_size ==
        INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_RECORD_SIZE) {
        if (!validComputePipeline(pipeline, view.library_count)) {
            return fail(error, INFERNO_METAL_BATCH_RECORD_PIPELINE, 0);
        }
        referenced[get32(
            pipeline +
            INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET)] = true;
    } else if (view.pipeline_size ==
               INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RECORD_SIZE) {
        if (!validRenderPipeline(pipeline, view.library_count)) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE,
                        0);
        }
        referenced[get32(
            pipeline +
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_LIBRARY_OFFSET)] =
            true;
        referenced[get32(
            pipeline +
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_LIBRARY_OFFSET)] =
            true;
    } else {
        referenced[0] = true;
    }
    for (uint32_t i = 0; i < view.library_count; i++) {
        if (!referenced[i]) {
            return fail(error, INFERNO_METAL_RESOURCE_RECORD_LIBRARY, i);
        }
    }
    *out = view;
    return true;
}
