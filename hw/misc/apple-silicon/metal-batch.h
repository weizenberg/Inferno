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

typedef struct InfernoMetalResourceBatchView {
    const uint8_t *bytes;
    size_t size;
    uint32_t library_count;
    uint32_t compute_pipeline_count;
    uint32_t render_pipeline_count;
    uint32_t buffer_count;
    uint32_t texture_count;
    uint32_t sampler_count;
    uint32_t command_count;
    uint32_t draw_count;
    uint32_t binding_count;
    uint32_t argument_count;
    uint32_t member_count;
    uint32_t declaration_count;
    uint32_t inline_size;
    uint32_t payload_size;
    uint32_t constants_size;
    uint32_t images_size;
    size_t libraries_offset;
    size_t compute_pipelines_offset;
    size_t render_pipelines_offset;
    size_t buffers_offset;
    size_t textures_offset;
    size_t samplers_offset;
    size_t commands_offset;
    size_t draws_offset;
    size_t bindings_offset;
    size_t arguments_offset;
    size_t members_offset;
    size_t declarations_offset;
    size_t payload_offset;
    size_t inline_offset;
    size_t constants_offset;
    size_t images_offset;
} InfernoMetalResourceBatchView;

typedef struct InfernoMetalTypedQueryView {
    const uint8_t *bytes;
    size_t size;
    uint32_t opcode;
    uint32_t library_count;
    uint32_t payload_size;
    uint32_t argument_buffer_index;
    size_t libraries_offset;
    size_t pipeline_offset;
    size_t pipeline_size;
    size_t payload_offset;
} InfernoMetalTypedQueryView;

bool inferno_metal_resource_batch_parse(const void *bytes, size_t size,
                                        size_t output_size,
                                        InfernoMetalResourceBatchView *view,
                                        InfernoMetalBatchParseError *error);
const uint8_t *
inferno_metal_resource_batch_library(const InfernoMetalResourceBatchView *view,
                                     uint32_t index);
const uint8_t *inferno_metal_resource_batch_compute_pipeline(
    const InfernoMetalResourceBatchView *view, uint32_t index);
const uint8_t *inferno_metal_resource_batch_render_pipeline(
    const InfernoMetalResourceBatchView *view, uint32_t index);
const uint8_t *
inferno_metal_resource_batch_buffer(const InfernoMetalResourceBatchView *view,
                                    uint32_t index);
const uint8_t *
inferno_metal_resource_batch_texture(const InfernoMetalResourceBatchView *view,
                                     uint32_t index);
const uint8_t *
inferno_metal_resource_batch_sampler(const InfernoMetalResourceBatchView *view,
                                     uint32_t index);
const uint8_t *
inferno_metal_resource_batch_command(const InfernoMetalResourceBatchView *view,
                                     uint32_t index);
const uint8_t *
inferno_metal_resource_batch_draw(const InfernoMetalResourceBatchView *view,
                                  uint32_t index);
const uint8_t *
inferno_metal_resource_batch_binding(const InfernoMetalResourceBatchView *view,
                                     uint32_t index);
const uint8_t *
inferno_metal_resource_batch_argument(const InfernoMetalResourceBatchView *view,
                                      uint32_t index);
const uint8_t *
inferno_metal_resource_batch_member(const InfernoMetalResourceBatchView *view,
                                    uint32_t index);
const uint8_t *inferno_metal_resource_batch_declaration(
    const InfernoMetalResourceBatchView *view, uint32_t index);

bool inferno_metal_typed_query_parse(uint32_t opcode, const void *bytes,
                                     size_t size,
                                     InfernoMetalTypedQueryView *view,
                                     InfernoMetalBatchParseError *error);
const uint8_t *
inferno_metal_typed_query_library(const InfernoMetalTypedQueryView *view,
                                  uint32_t index);
const uint8_t *
inferno_metal_typed_query_pipeline(const InfernoMetalTypedQueryView *view);

#endif
