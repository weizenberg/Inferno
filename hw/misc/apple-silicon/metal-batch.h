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

#ifndef HW_APPLE_METAL_BATCH_H
#define HW_APPLE_METAL_BATCH_H

#include "standard-headers/inferno/metal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct InfernoMetalBatchView {
    const uint8_t *bytes;
    size_t size;
    uint32_t pipeline_count;
    uint32_t buffer_count;
    uint32_t dispatch_count;
    uint32_t binding_count;
    uint32_t inline_size;
    uint32_t source_size;
    uint32_t images_size;
    size_t pipelines_offset;
    size_t buffers_offset;
    size_t dispatches_offset;
    size_t bindings_offset;
    size_t source_offset;
    size_t inline_offset;
    size_t images_offset;
} InfernoMetalBatchView;

typedef struct InfernoMetalBatchParseError {
    uint32_t record_kind;
    uint32_t record_index;
} InfernoMetalBatchParseError;

bool inferno_metal_batch_parse(const void *bytes, size_t size,
                               size_t output_size, InfernoMetalBatchView *view,
                               InfernoMetalBatchParseError *error);
const uint8_t *inferno_metal_batch_pipeline(const InfernoMetalBatchView *view,
                                            uint32_t index);
const uint8_t *inferno_metal_batch_buffer(const InfernoMetalBatchView *view,
                                          uint32_t index);
const uint8_t *inferno_metal_batch_dispatch(const InfernoMetalBatchView *view,
                                            uint32_t index);
const uint8_t *inferno_metal_batch_binding(const InfernoMetalBatchView *view,
                                           uint32_t index);

#endif
