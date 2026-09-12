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

#ifndef INFERNO_METAL_COMPILER_CLIENT_H
#define INFERNO_METAL_COMPILER_CLIENT_H

#include "user-client.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImtlCompilerFunction {
    uint32_t type;
    const char *name;
    uint32_t name_length;
    uint32_t patch_type;
    uint32_t flags;
    int64_t patch_control_point_count;
    uint64_t options;
    uint32_t constant_count;
    uint32_t vertex_attribute_count;
    uint32_t stage_input_attribute_count;
    const uint8_t *metadata_records;
} ImtlCompilerFunction;

typedef struct ImtlCompilerMetadata {
    uint32_t kind;
    uint32_t data_type;
    uint64_t index;
    uint32_t flags;
    const char *name;
    uint32_t name_length;
} ImtlCompilerMetadata;

typedef struct ImtlCompilerSize {
    uint64_t width;
    uint64_t height;
    uint64_t depth;
} ImtlCompilerSize;

typedef struct ImtlCompilerRenderPipeline {
    uint64_t allocated_size;
    uint64_t imageblock_sample_length;
    uint64_t max_total_threads_per_threadgroup;
    uint64_t max_total_threads_per_object_threadgroup;
    uint64_t max_total_threads_per_mesh_threadgroup;
    uint64_t object_thread_execution_width;
    uint64_t mesh_thread_execution_width;
    uint64_t max_total_threadgroups_per_mesh_grid;
    int64_t shader_validation;
    ImtlCompilerSize required_threads_per_tile_threadgroup;
    ImtlCompilerSize required_threads_per_object_threadgroup;
    ImtlCompilerSize required_threads_per_mesh_threadgroup;
    uint32_t flags;
} ImtlCompilerRenderPipeline;

/* All string and record views are borrowed from the immutable caller-owned
 * bytes passed to the decoder. They remain valid only while those bytes remain
 * unchanged and alive. Kernel ACK does not invalidate a private reply buffer;
 * the buffer owner determines the lifetime of every decoded view. stage is
 * meaningful only for an opcode-11 function failure. render_pipeline is
 * meaningful only for an opcode-11 success; the library fields are meaningful
 * only for a successful library query.
 */
typedef struct ImtlCompilerResult {
    uint32_t opcode;
    uint64_t sequence;
    uint32_t outcome;
    uint32_t phase;
    uint32_t flags;
    uint32_t stage;
    uint32_t function_count;
    uint32_t function_type;
    uint32_t max_total_threads_per_threadgroup;
    uint32_t thread_execution_width;
    uint32_t static_threadgroup_memory_length;
    uint32_t required_output_size;
    uint32_t metadata_record_count;
    int64_t error_code;
    const char *error_domain;
    uint32_t error_domain_length;
    const char *error_description;
    uint32_t error_description_length;
    uint64_t allocated_size;
    uint64_t required_threads_width;
    uint64_t required_threads_height;
    uint64_t required_threads_depth;
    int64_t shader_validation;
    uint64_t imageblock_memory_length;
    uint32_t pipeline_flags;
    uint32_t library_type;
    uint32_t library_flags;
    const char *library_install_name;
    uint32_t library_install_name_length;
    ImtlCompilerRenderPipeline render_pipeline;
    const uint8_t *function_records;
    size_t function_records_size;
} ImtlCompilerResult;

/* Zero capacities are a valid size probe. Invalid capacities/overflow return
 * zero. The result is the exact complete-inventory envelope size.
 */
size_t imtl_compiler_output_size(uint32_t function_capacity,
                                 uint32_t metadata_capacity);

IOReturn imtl_compiler_submit_library(ImtlUserClient *client, uint64_t sequence,
                                      const void *source, size_t source_size,
                                      uint32_t output_size);
IOReturn imtl_compiler_submit_pipeline(ImtlUserClient *client,
                                       uint64_t sequence, const void *source,
                                       size_t source_size,
                                       const char *kernel_name);
IOReturn imtl_compiler_submit_imageblock(ImtlUserClient *client,
                                         uint64_t sequence, const void *source,
                                         size_t source_size,
                                         const char *kernel_name,
                                         uint32_t width, uint32_t height,
                                         uint32_t depth);

/* manifest must be an immutable canonical allocation returned by
 * imtl_typed_query_builder_build(). It is borrowed for this synchronous call.
 */
IOReturn imtl_compiler_submit_typed_query(ImtlUserClient *client,
                                          uint64_t sequence, uint32_t opcode,
                                          const void *manifest,
                                          size_t manifest_size,
                                          uint32_t output_size, uint32_t width,
                                          uint32_t height, uint32_t depth);

/* Validates the complete supplied envelope before publishing out. Failure
 * leaves out unchanged. expected_opcode must name a compiler query opcode.
 */
bool imtl_compiler_decode_result(const uint8_t *bytes, size_t size,
                                 uint32_t expected_opcode,
                                 uint64_t expected_sequence,
                                 ImtlCompilerResult *out);

/* Returns one already-validated borrowed inventory record. Failure leaves out
 * unchanged. Only successful library results have accessible records.
 */
bool imtl_compiler_function_at(const ImtlCompilerResult *result, uint32_t index,
                               ImtlCompilerFunction *out);
bool imtl_compiler_metadata_at(const ImtlCompilerFunction *function,
                               uint32_t index, ImtlCompilerMetadata *out);

#ifdef __cplusplus
}
#endif
#endif
