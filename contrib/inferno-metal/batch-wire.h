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

#ifndef INFERNO_METAL_BATCH_WIRE_H
#define INFERNO_METAL_BATCH_WIRE_H

#include "standard-headers/inferno/metal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImtlBatchPipeline {
    const void *source;
    size_t source_size;
    const char *function_name;
} ImtlBatchPipeline;

typedef struct ImtlBatchBuffer {
    const void *bytes;
    size_t length;
} ImtlBatchBuffer;

typedef struct ImtlBatchBinding {
    uint32_t kind;
    uint32_t index;
    uint32_t resource_id;
    uint64_t offset;
    const void *bytes;
    size_t length;
} ImtlBatchBinding;

typedef struct ImtlBatchDispatch {
    uint32_t pipeline_id;
    uint32_t mode;
    uint32_t binding_start;
    uint32_t binding_count;
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t grid_depth;
    uint32_t group_width;
    uint32_t group_height;
    uint32_t group_depth;
} ImtlBatchDispatch;

typedef struct ImtlBatchManifest {
    const ImtlBatchPipeline *pipelines;
    uint32_t pipeline_count;
    const ImtlBatchBuffer *buffers;
    uint32_t buffer_count;
    const ImtlBatchDispatch *dispatches;
    uint32_t dispatch_count;
    const ImtlBatchBinding *bindings;
    uint32_t binding_count;
} ImtlBatchManifest;

typedef struct ImtlBatchResult {
    uint64_t sequence;
    uint32_t outcome;
    uint32_t phase;
    uint32_t flags;
    uint32_t failed_record_kind;
    uint32_t failed_record_index;
    uint32_t buffer_count;
    uint32_t images_size;
    uint32_t host_command_buffer_status;
    int64_t error_code;
    const char *error_domain;
    uint32_t error_domain_length;
    const char *error_description;
    uint32_t error_description_length;
    const uint8_t *images;
} ImtlBatchResult;

typedef struct ImtlBatch5Library {
    uint32_t kind;
    const void *bytes;
    size_t size;
} ImtlBatch5Library;

typedef struct ImtlBatch5ComputePipeline {
    uint32_t library_id;
    const char *function_name;
} ImtlBatch5ComputePipeline;

typedef struct ImtlBatch5RenderPipeline {
    uint32_t vertex_library_id;
    uint32_t fragment_library_id;
    uint32_t color0_pixel_format;
    uint32_t raster_sample_count;
    uint32_t blending_enabled;
    uint32_t source_rgb_blend_factor;
    uint32_t destination_rgb_blend_factor;
    uint32_t rgb_blend_operation;
    uint32_t source_alpha_blend_factor;
    uint32_t destination_alpha_blend_factor;
    uint32_t alpha_blend_operation;
    uint32_t write_mask;
    const char *vertex_function_name;
    const char *fragment_function_name;
} ImtlBatch5RenderPipeline;

typedef struct ImtlBatch5Texture {
    const void *bytes;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t usage;
    uint32_t flags;
} ImtlBatch5Texture;

typedef struct ImtlBatch5Sampler {
    uint32_t min_filter;
    uint32_t mag_filter;
    uint32_t mip_filter;
    uint32_t max_anisotropy;
    uint32_t s_address_mode;
    uint32_t t_address_mode;
    uint32_t r_address_mode;
    uint32_t border_color;
    uint32_t reduction_mode;
    uint32_t normalized_coordinates;
    float lod_min_clamp;
    float lod_max_clamp;
    uint32_t lod_average;
    float lod_bias;
    uint32_t compare_function;
    uint32_t support_argument_buffers;
} ImtlBatch5Sampler;

typedef struct ImtlBatch5ComputeCommand {
    uint32_t pipeline_id;
    uint32_t mode;
    uint32_t binding_start;
    uint32_t binding_count;
    uint32_t grid_width;
    uint32_t grid_height;
    uint32_t grid_depth;
    uint32_t group_width;
    uint32_t group_height;
    uint32_t group_depth;
} ImtlBatch5ComputeCommand;

typedef struct ImtlBatch5RenderCommand {
    uint32_t texture_id;
    uint32_t load_action;
    uint32_t store_action;
    uint32_t draw_start;
    uint32_t draw_count;
    double clear_red;
    double clear_green;
    double clear_blue;
    double clear_alpha;
} ImtlBatch5RenderCommand;

typedef struct ImtlBatch5Command {
    uint32_t kind;
    union {
        ImtlBatch5ComputeCommand compute;
        ImtlBatch5RenderCommand render;
    } value;
} ImtlBatch5Command;

typedef struct ImtlBatch5Draw {
    uint32_t render_pipeline_id;
    uint32_t primitive_type;
    uint32_t vertex_start;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t base_instance;
    uint32_t vertex_binding_start;
    uint32_t vertex_binding_count;
    uint32_t fragment_binding_start;
    uint32_t fragment_binding_count;
    uint32_t cull_mode;
    uint32_t winding;
    uint32_t fill_mode;
    uint32_t flags;
    uint32_t scissor_x;
    uint32_t scissor_y;
    uint32_t scissor_width;
    uint32_t scissor_height;
    double viewport_origin_x;
    double viewport_origin_y;
    double viewport_width;
    double viewport_height;
    double viewport_znear;
    double viewport_zfar;
    float blend_red;
    float blend_green;
    float blend_blue;
    float blend_alpha;
} ImtlBatch5Draw;

typedef struct ImtlBatch5Manifest {
    const ImtlBatch5Library *libraries;
    uint32_t library_count;
    const ImtlBatch5ComputePipeline *compute_pipelines;
    uint32_t compute_pipeline_count;
    const ImtlBatch5RenderPipeline *render_pipelines;
    uint32_t render_pipeline_count;
    const ImtlBatchBuffer *buffers;
    uint32_t buffer_count;
    const ImtlBatch5Texture *textures;
    uint32_t texture_count;
    const ImtlBatch5Sampler *samplers;
    uint32_t sampler_count;
    const ImtlBatch5Command *commands;
    uint32_t command_count;
    const ImtlBatch5Draw *draws;
    uint32_t draw_count;
    const ImtlBatchBinding *bindings;
    uint32_t binding_count;
} ImtlBatch5Manifest;

typedef struct ImtlTypedQueryManifest {
    const ImtlBatch5Library *libraries;
    uint32_t library_count;
    const ImtlBatch5ComputePipeline *compute_pipeline;
    const ImtlBatch5RenderPipeline *render_pipeline;
} ImtlTypedQueryManifest;

typedef struct ImtlBatch5Result {
    uint64_t sequence;
    uint32_t outcome;
    uint32_t phase;
    uint32_t flags;
    uint32_t failed_record_kind;
    uint32_t failed_record_index;
    uint32_t buffer_count;
    uint32_t texture_count;
    uint32_t images_size;
    uint32_t host_command_buffer_status;
    int64_t error_code;
    const char *error_domain;
    uint32_t error_domain_length;
    const char *error_description;
    uint32_t error_description_length;
    const uint8_t *images;
} ImtlBatch5Result;

/* Allocates one canonical manifest. Equal adjacent source blobs share one
 * source range; all other pipeline sources are emitted contiguously. The
 * caller owns *bytes and releases it with imtl_batch_builder_free().
 */
bool imtl_batch_builder_build(const ImtlBatchManifest *manifest,
                              uint8_t **bytes, size_t *size,
                              uint32_t *images_size);
void imtl_batch_builder_free(uint8_t *bytes);

/* Validates every byte before publishing out. Typed failures for a valid
 * manifest echo expected_buffer_count/expected_images_size and carry a zero
 * image region. MALFORMED carries zero counts. OK uses UNKNOWN/UINT32_MAX.
 */
bool imtl_batch_decode_result(const uint8_t *bytes, size_t size,
                              uint64_t expected_sequence,
                              uint32_t expected_buffer_count,
                              uint32_t expected_images_size,
                              ImtlBatchResult *out);

/* Builds an immutable copied version-5 resource manifest. Raw payload, inline,
 * and image bytes are packed without padding in their canonical region order.
 */
bool imtl_batch5_builder_build(const ImtlBatch5Manifest *manifest,
                               uint8_t **bytes, size_t *size,
                               uint32_t *images_size);

/* Builds one typed-query input for opcodes 9 through 12. */
bool imtl_typed_query_builder_build(uint32_t opcode,
                                    const ImtlTypedQueryManifest *manifest,
                                    uint8_t **bytes, size_t *size);

/* Validates the complete version-5 resource result before publishing out. */
bool imtl_batch5_decode_result(const uint8_t *bytes, size_t size,
                               uint64_t expected_sequence,
                               uint32_t expected_buffer_count,
                               uint32_t expected_texture_count,
                               uint32_t expected_images_size,
                               ImtlBatch5Result *out);

#ifdef __cplusplus
}
#endif
#endif
