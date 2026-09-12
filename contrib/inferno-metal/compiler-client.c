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

#include "compiler-client.h"
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
    uint32_t value = 0;

    for (unsigned i = 0; i < 4; i++) {
        value |= (uint32_t)p[i] << (i * 8);
    }
    return value;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static bool allZero(const uint8_t *p, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

static bool validUtf8(const uint8_t *p, size_t size)
{
    size_t i = 0;

    while (i < size) {
        uint32_t value;
        unsigned extra;

        if (p[i] < 0x80) {
            i++;
            continue;
        }
        if (p[i] >= 0xc2 && p[i] <= 0xdf) {
            value = p[i] & 0x1f;
            extra = 1;
        } else if (p[i] >= 0xe0 && p[i] <= 0xef) {
            value = p[i] & 0x0f;
            extra = 2;
        } else if (p[i] >= 0xf0 && p[i] <= 0xf4) {
            value = p[i] & 0x07;
            extra = 3;
        } else {
            return false;
        }
        if (extra > size - i - 1) {
            return false;
        }
        for (unsigned j = 1; j <= extra; j++) {
            if ((p[i + j] & 0xc0) != 0x80) {
                return false;
            }
            value = (value << 6) | (p[i + j] & 0x3f);
        }
        if ((extra == 2 && value < 0x800) || (extra == 3 && value < 0x10000) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

static bool knownFunctionType(uint32_t type)
{
    switch (type) {
    case INFERNO_METAL_FUNCTION_TYPE_VERTEX:
    case INFERNO_METAL_FUNCTION_TYPE_FRAGMENT:
    case INFERNO_METAL_FUNCTION_TYPE_KERNEL:
    case INFERNO_METAL_FUNCTION_TYPE_VISIBLE:
    case INFERNO_METAL_FUNCTION_TYPE_INTERSECTION:
    case INFERNO_METAL_FUNCTION_TYPE_MESH:
    case INFERNO_METAL_FUNCTION_TYPE_OBJECT:
        return true;
    default:
        return false;
    }
}

static bool validString(const uint8_t *p, uint32_t length, uint32_t capacity)
{
    return length < capacity && p[length] == 0 && !memchr(p, 0, length) &&
           validUtf8(p, length) &&
           allZero(p + length + 1, capacity - length - 1);
}

size_t imtl_compiler_output_size(uint32_t function_capacity)
{
    if (function_capacity > INFERNO_METAL_COMPILER_MAX_FUNCTIONS) {
        return 0;
    }
    return INFERNO_METAL_COMPILER_MIN_OUTPUT +
           (size_t)function_capacity *
               INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
}

static IOReturn validateBuilder(ImtlUserClient *client, uint32_t opcode,
                                const void *source, size_t source_size,
                                uint32_t output_size)
{
    const ImtlUserCaps *caps = imtl_user_client_caps(client);

    if (!client || !source || !source_size ||
        source_size > INFERNO_METAL_MAX_SOURCE) {
        return kIOReturnBadArgument;
    }
    if (!validUtf8(source, source_size)) {
        return kIOReturnUnsupported;
    }
    if (!caps || !(caps->opcode_mask & (1U << opcode)) ||
        source_size > caps->max_source_size ||
        output_size > caps->max_output_size ||
        caps->max_output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
        source_size >
            caps->max_request_size - INFERNO_METAL_USER_SUBMIT_HEADER_SIZE) {
        return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

IOReturn imtl_compiler_submit_library(ImtlUserClient *client, uint64_t sequence,
                                      const void *source, size_t source_size,
                                      uint32_t function_capacity)
{
    size_t output_size = imtl_compiler_output_size(function_capacity);
    if (!output_size) {
        return kIOReturnBadArgument;
    }
    IOReturn result = validateBuilder(client, INFERNO_METAL_QUERY_LIBRARY,
                                      source, source_size, output_size);
    if (result != kIOReturnSuccess) {
        return result;
    }
    ImtlUserSubmit request = {
        .opcode = INFERNO_METAL_QUERY_LIBRARY,
        .sequence = sequence,
        .output_size = (uint32_t)output_size,
        .width = 1,
        .height = 1,
        .depth = 1,
        .options = INFERNO_METAL_OPTIONS_DEFAULT,
        .source = source,
        .source_size = source_size,
    };
    return imtl_user_client_submit(client, &request);
}

IOReturn imtl_compiler_submit_pipeline(ImtlUserClient *client,
                                       uint64_t sequence, const void *source,
                                       size_t source_size,
                                       const char *kernel_name)
{
    if (!kernel_name) {
        return kIOReturnBadArgument;
    }
    size_t name_size = strnlen(kernel_name, 64);
    if (!name_size || name_size == 64 ||
        !validUtf8((const uint8_t *)kernel_name, name_size)) {
        return kIOReturnUnsupported;
    }
    IOReturn result =
        validateBuilder(client, INFERNO_METAL_QUERY_PIPELINE, source,
                        source_size, INFERNO_METAL_COMPILER_MIN_OUTPUT);
    if (result != kIOReturnSuccess) {
        return result;
    }
    ImtlUserSubmit request = {
        .opcode = INFERNO_METAL_QUERY_PIPELINE,
        .sequence = sequence,
        .output_size = INFERNO_METAL_COMPILER_MIN_OUTPUT,
        .width = 1,
        .height = 1,
        .depth = 1,
        .options = INFERNO_METAL_OPTIONS_DEFAULT,
        .source = source,
        .source_size = source_size,
    };
    memcpy(request.function, kernel_name, name_size + 1);
    return imtl_user_client_submit(client, &request);
}

static bool validCombination(const ImtlCompilerResult *r)
{
    bool library = r->opcode == INFERNO_METAL_QUERY_LIBRARY;
    bool no_error = r->flags & INFERNO_METAL_COMPILER_FLAG_NO_NSERROR;
    bool warning = r->flags & INFERNO_METAL_COMPILER_FLAG_WARNING;

    if (warning &&
        (r->outcome != INFERNO_METAL_COMPILER_OUTCOME_OK || no_error)) {
        return false;
    }
    if (no_error) {
        if (r->error_code || r->error_domain_length ||
            (r->flags & INFERNO_METAL_COMPILER_FLAG_DOMAIN_TRUNCATED)) {
            return false;
        }
    } else if (!r->error_domain_length) {
        return false;
    }
    if ((r->flags & INFERNO_METAL_COMPILER_FLAG_DOMAIN_TRUNCATED) &&
        r->error_domain_length < INFERNO_METAL_COMPILER_DOMAIN_SIZE - 4) {
        return false;
    }
    if ((r->flags & INFERNO_METAL_COMPILER_FLAG_DESCRIPTION_TRUNCATED) &&
        r->error_description_length !=
            INFERNO_METAL_COMPILER_DESCRIPTION_SIZE - 1 &&
        r->error_description_length <
            INFERNO_METAL_COMPILER_DESCRIPTION_SIZE - 4) {
        return false;
    }

    if (r->outcome == INFERNO_METAL_COMPILER_OUTCOME_OK) {
        if (no_error) {
            if (r->error_description_length ||
                r->flags != INFERNO_METAL_COMPILER_FLAG_NO_NSERROR) {
                return false;
            }
        } else if (!warning) {
            return false;
        }
    }

    switch (r->outcome) {
    case INFERNO_METAL_COMPILER_OUTCOME_OK:
        return library ?
                   r->phase == INFERNO_METAL_COMPILER_PHASE_INVENTORY &&
                       r->function_type == 0 &&
                       r->max_total_threads_per_threadgroup == 0 &&
                       r->thread_execution_width == 0 &&
                       r->static_threadgroup_memory_length == 0 :
                   r->phase == INFERNO_METAL_COMPILER_PHASE_PIPELINE &&
                       r->function_count == 0 &&
                       r->function_type == INFERNO_METAL_FUNCTION_TYPE_KERNEL &&
                       r->max_total_threads_per_threadgroup != 0 &&
                       r->thread_execution_width != 0;
    case INFERNO_METAL_COMPILER_OUTCOME_COMPILE_FAILED:
        return r->function_count == 0 && r->function_type == 0 &&
               r->max_total_threads_per_threadgroup == 0 &&
               r->thread_execution_width == 0 &&
               r->static_threadgroup_memory_length == 0 &&
               (r->phase == INFERNO_METAL_COMPILER_PHASE_LIBRARY ||
                (!library &&
                 r->phase == INFERNO_METAL_COMPILER_PHASE_PIPELINE));
    case INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_NOT_FOUND:
        return !library && no_error &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_FUNCTION &&
               r->function_count == 0 && r->function_type == 0 &&
               r->max_total_threads_per_threadgroup == 0 &&
               r->thread_execution_width == 0 &&
               r->static_threadgroup_memory_length == 0;
    case INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_TYPE_MISMATCH:
        return !library && no_error &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_FUNCTION &&
               r->function_count == 0 && knownFunctionType(r->function_type) &&
               r->function_type != INFERNO_METAL_FUNCTION_TYPE_KERNEL &&
               r->max_total_threads_per_threadgroup == 0 &&
               r->thread_execution_width == 0 &&
               r->static_threadgroup_memory_length == 0;
    case INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED:
        return library && no_error &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_INVENTORY &&
               r->function_type == 0 &&
               r->max_total_threads_per_threadgroup == 0 &&
               r->thread_execution_width == 0 &&
               r->static_threadgroup_memory_length == 0;
    case INFERNO_METAL_COMPILER_OUTCOME_OUTPUT_TOO_SMALL:
        return library && no_error &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_INVENTORY &&
               r->function_count > r->function_capacity &&
               r->function_type == 0 &&
               r->max_total_threads_per_threadgroup == 0 &&
               r->thread_execution_width == 0 &&
               r->static_threadgroup_memory_length == 0;
    default:
        return false;
    }
}

bool imtl_compiler_decode_result(const uint8_t *bytes, size_t size,
                                 uint32_t expected_opcode,
                                 uint64_t expected_sequence,
                                 ImtlCompilerResult *out)
{
    if (!bytes || !out ||
        (expected_opcode != INFERNO_METAL_QUERY_LIBRARY &&
         expected_opcode != INFERNO_METAL_QUERY_PIPELINE) ||
        size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
        size > INFERNO_METAL_COMPILER_MAX_OUTPUT) {
        return false;
    }
    ImtlCompilerResult r = {
        .opcode = get32(bytes + INFERNO_METAL_COMPILER_OPCODE_OFFSET),
        .sequence = get64(bytes + INFERNO_METAL_COMPILER_SEQUENCE_OFFSET),
        .outcome = get32(bytes + INFERNO_METAL_COMPILER_OUTCOME_OFFSET),
        .phase = get32(bytes + INFERNO_METAL_COMPILER_PHASE_OFFSET),
        .flags = get32(bytes + INFERNO_METAL_COMPILER_FLAGS_OFFSET),
        .function_count =
            get32(bytes + INFERNO_METAL_COMPILER_FUNCTION_COUNT_OFFSET),
        .function_type =
            get32(bytes + INFERNO_METAL_COMPILER_FUNCTION_TYPE_OFFSET),
        .max_total_threads_per_threadgroup =
            get32(bytes + INFERNO_METAL_COMPILER_MAX_TOTAL_THREADS_OFFSET),
        .thread_execution_width =
            get32(bytes + INFERNO_METAL_COMPILER_THREAD_EXECUTION_WIDTH_OFFSET),
        .static_threadgroup_memory_length = get32(
            bytes + INFERNO_METAL_COMPILER_STATIC_THREADGROUP_MEMORY_OFFSET),
        .error_domain_length =
            get32(bytes + INFERNO_METAL_COMPILER_DOMAIN_LENGTH_OFFSET),
        .error_description_length =
            get32(bytes + INFERNO_METAL_COMPILER_DESCRIPTION_LENGTH_OFFSET),
        .error_domain =
            (const char *)bytes + INFERNO_METAL_COMPILER_DOMAIN_OFFSET,
        .error_description =
            (const char *)bytes + INFERNO_METAL_COMPILER_DESCRIPTION_OFFSET,
        .function_records = bytes + INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET,
        .function_capacity =
            (uint32_t)((size - INFERNO_METAL_COMPILER_MIN_OUTPUT) /
                       INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE),
    };
    uint64_t error_bits =
        get64(bytes + INFERNO_METAL_COMPILER_ERROR_CODE_OFFSET);
    memcpy(&r.error_code, &error_bits, sizeof(r.error_code));

    if (get32(bytes + INFERNO_METAL_COMPILER_VERSION_OFFSET) !=
            INFERNO_METAL_VERSION ||
        r.opcode != expected_opcode || r.sequence != expected_sequence ||
        r.flags & ~INFERNO_METAL_COMPILER_FLAG_MASK ||
        !allZero(bytes + INFERNO_METAL_COMPILER_RESERVED_OFFSET,
                 INFERNO_METAL_COMPILER_RESERVED_SIZE) ||
        !validString(bytes + INFERNO_METAL_COMPILER_DOMAIN_OFFSET,
                     r.error_domain_length,
                     INFERNO_METAL_COMPILER_DOMAIN_SIZE) ||
        !validString(bytes + INFERNO_METAL_COMPILER_DESCRIPTION_OFFSET,
                     r.error_description_length,
                     INFERNO_METAL_COMPILER_DESCRIPTION_SIZE) ||
        !validCombination(&r)) {
        return false;
    }

    uint32_t records = r.outcome == INFERNO_METAL_COMPILER_OUTCOME_OK &&
                               r.opcode == INFERNO_METAL_QUERY_LIBRARY ?
                           r.function_count :
                           0;
    if (records > r.function_capacity ||
        records > INFERNO_METAL_COMPILER_MAX_FUNCTIONS) {
        return false;
    }
    for (uint32_t i = 0; i < records; i++) {
        const uint8_t *record =
            r.function_records +
            (size_t)i * INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
        uint32_t type =
            get32(record + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET);
        uint32_t length =
            get32(record + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET);
        const uint8_t *name =
            record + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET;

        if (!knownFunctionType(type) || !length ||
            !validString(name, length,
                         INFERNO_METAL_COMPILER_FUNCTION_NAME_SIZE)) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *previous =
                r.function_records +
                (size_t)j * INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
            uint32_t previous_length = get32(
                previous + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET);

            if (length == previous_length &&
                !memcmp(name,
                        previous + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
                        length)) {
                return false;
            }
        }
    }
    size_t used = (size_t)records * INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
    size_t available = size - INFERNO_METAL_COMPILER_MIN_OUTPUT;
    if (!allZero(r.function_records + used, available - used)) {
        return false;
    }
    *out = r;
    return true;
}

bool imtl_compiler_function_at(const ImtlCompilerResult *result, uint32_t index,
                               ImtlCompilerFunction *out)
{
    if (!result || !out || result->opcode != INFERNO_METAL_QUERY_LIBRARY ||
        result->outcome != INFERNO_METAL_COMPILER_OUTCOME_OK ||
        index >= result->function_count || index >= result->function_capacity ||
        !result->function_records) {
        return false;
    }
    const uint8_t *record =
        result->function_records +
        (size_t)index * INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
    ImtlCompilerFunction function = {
        .type = get32(record + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET),
        .name =
            (const char *)record + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
        .name_length =
            get32(record + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET),
    };
    *out = function;
    return true;
}
