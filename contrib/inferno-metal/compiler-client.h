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
} ImtlCompilerFunction;

/* All string and record views are borrowed from the immutable caller-owned
 * bytes passed to the decoder. They remain valid only while those bytes remain
 * unchanged and alive. The caller serializes decode/copy with handle use and
 * ACKs only after it has copied every view it needs.
 */
typedef struct ImtlCompilerResult {
    uint32_t opcode;
    uint64_t sequence;
    uint32_t outcome;
    uint32_t phase;
    uint32_t flags;
    uint32_t function_count;
    uint32_t function_type;
    uint32_t max_total_threads_per_threadgroup;
    uint32_t thread_execution_width;
    uint32_t static_threadgroup_memory_length;
    int64_t error_code;
    const char *error_domain;
    uint32_t error_domain_length;
    const char *error_description;
    uint32_t error_description_length;
    const uint8_t *function_records;
    uint32_t function_capacity;
} ImtlCompilerResult;

/* Capacity zero is a valid size probe. Invalid capacities return zero. */
size_t imtl_compiler_output_size(uint32_t function_capacity);

IOReturn imtl_compiler_submit_library(ImtlUserClient *client, uint64_t sequence,
                                      const void *source, size_t source_size,
                                      uint32_t function_capacity);
IOReturn imtl_compiler_submit_pipeline(ImtlUserClient *client,
                                       uint64_t sequence, const void *source,
                                       size_t source_size,
                                       const char *kernel_name);

/* Validates the complete supplied envelope before publishing out. Failure
 * leaves out unchanged. expected_opcode must name one of the two query ops.
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

#ifdef __cplusplus
}
#endif
#endif
