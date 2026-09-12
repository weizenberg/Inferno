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

#ifdef __cplusplus
}
#endif
#endif
