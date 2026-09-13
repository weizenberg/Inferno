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

static int64_t getSigned64(const uint8_t *p)
{
    uint64_t bits = get64(p);
    int64_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
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

static bool validString(const uint8_t *p, uint32_t length, uint32_t capacity)
{
    return length < capacity && p[length] == 0 && !memchr(p, 0, length) &&
           validUtf8(p, length) &&
           allZero(p + length + 1, capacity - length - 1);
}

static bool knownFunctionType(uint32_t type)
{
    return type == INFERNO_METAL_FUNCTION_TYPE_VERTEX ||
           type == INFERNO_METAL_FUNCTION_TYPE_FRAGMENT ||
           type == INFERNO_METAL_FUNCTION_TYPE_KERNEL ||
           type == INFERNO_METAL_FUNCTION_TYPE_VISIBLE ||
           type == INFERNO_METAL_FUNCTION_TYPE_INTERSECTION ||
           type == INFERNO_METAL_FUNCTION_TYPE_MESH ||
           type == INFERNO_METAL_FUNCTION_TYPE_OBJECT;
}

static bool knownDataType(uint32_t type)
{
    return type <= 56 || (type >= 58 && type <= 60) ||
           (type >= 62 && type <= 88) || (type >= 115 && type <= 118) ||
           (type >= 121 && type <= 124) || (type >= 139 && type <= 140);
}

static bool libraryOpcode(uint32_t opcode)
{
    return opcode == INFERNO_METAL_QUERY_LIBRARY ||
           opcode == INFERNO_METAL_QUERY_LIBRARY_TYPED;
}

static bool imageblockOpcode(uint32_t opcode)
{
    return opcode == INFERNO_METAL_QUERY_IMAGEBLOCK ||
           opcode == INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED;
}

static bool typedOpcode(uint32_t opcode)
{
    switch (opcode) {
    case INFERNO_METAL_QUERY_LIBRARY_TYPED:
    case INFERNO_METAL_QUERY_PIPELINE_TYPED:
    case INFERNO_METAL_QUERY_RENDER_PIPELINE:
    case INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED:
    case INFERNO_METAL_QUERY_ARGUMENT_LAYOUT:
        return true;
    default:
        return false;
    }
}

static bool compilerOpcode(uint32_t opcode)
{
    switch (opcode) {
    case INFERNO_METAL_QUERY_LIBRARY:
    case INFERNO_METAL_QUERY_PIPELINE:
    case INFERNO_METAL_QUERY_IMAGEBLOCK:
        return true;
    default:
        return typedOpcode(opcode);
    }
}

size_t imtl_compiler_output_size(uint32_t function_capacity,
                                 uint32_t metadata_capacity)
{
    if (function_capacity > INFERNO_METAL_COMPILER_MAX_FUNCTIONS ||
        metadata_capacity > INFERNO_METAL_COMPILER_MAX_METADATA_RECORDS) {
        return 0;
    }
    return INFERNO_METAL_COMPILER_MIN_OUTPUT +
           (size_t)function_capacity *
               INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
           (size_t)metadata_capacity *
               INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
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

static IOReturn submitNamed(ImtlUserClient *client, uint32_t opcode,
                            uint64_t sequence, const void *source,
                            size_t source_size, const char *kernel_name,
                            uint32_t width, uint32_t height, uint32_t depth)
{
    if (!kernel_name) {
        return kIOReturnBadArgument;
    }
    size_t name_size = strnlen(kernel_name, 64);
    if (!name_size || name_size == 64 ||
        !validUtf8((const uint8_t *)kernel_name, name_size)) {
        return kIOReturnUnsupported;
    }
    IOReturn result = validateBuilder(client, opcode, source, source_size,
                                      INFERNO_METAL_COMPILER_MIN_OUTPUT);
    if (result != kIOReturnSuccess) {
        return result;
    }
    ImtlUserSubmit request = {
        .opcode = opcode,
        .sequence = sequence,
        .output_size = INFERNO_METAL_COMPILER_MIN_OUTPUT,
        .width = width,
        .height = height,
        .depth = depth,
        .options = INFERNO_METAL_OPTIONS_DEFAULT,
        .source = source,
        .source_size = source_size,
    };
    memcpy(request.function, kernel_name, name_size + 1);
    return imtl_user_client_submit(client, &request);
}

IOReturn imtl_compiler_submit_library(ImtlUserClient *client, uint64_t sequence,
                                      const void *source, size_t source_size,
                                      uint32_t output_size)
{
    if (output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
        output_size > INFERNO_METAL_COMPILER_MAX_OUTPUT) {
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
        .output_size = output_size,
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
    return submitNamed(client, INFERNO_METAL_QUERY_PIPELINE, sequence, source,
                       source_size, kernel_name, 1, 1, 1);
}

IOReturn imtl_compiler_submit_imageblock(ImtlUserClient *client,
                                         uint64_t sequence, const void *source,
                                         size_t source_size,
                                         const char *kernel_name,
                                         uint32_t width, uint32_t height,
                                         uint32_t depth)
{
    if (!width || width > 65536 || !height || height > 65536 || !depth ||
        depth > 65536) {
        return kIOReturnBadArgument;
    }
    return submitNamed(client, INFERNO_METAL_QUERY_IMAGEBLOCK, sequence, source,
                       source_size, kernel_name, width, height, depth);
}

IOReturn imtl_compiler_submit_typed_query(ImtlUserClient *client,
                                          uint64_t sequence, uint32_t opcode,
                                          const void *manifest,
                                          size_t manifest_size,
                                          uint32_t output_size, uint32_t width,
                                          uint32_t height, uint32_t depth)
{
    const ImtlUserCaps *caps = imtl_user_client_caps(client);
    bool imageblock = opcode == INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED;
    bool argument = opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT;
    if (!client || !manifest ||
        manifest_size < INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE ||
        manifest_size > INFERNO_METAL_MAX_BUFFER ||
        (opcode != INFERNO_METAL_QUERY_LIBRARY_TYPED &&
         opcode != INFERNO_METAL_QUERY_PIPELINE_TYPED &&
         opcode != INFERNO_METAL_QUERY_RENDER_PIPELINE && !imageblock &&
         !argument) ||
        (opcode == INFERNO_METAL_QUERY_LIBRARY_TYPED ?
             (output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
              output_size > INFERNO_METAL_COMPILER_MAX_OUTPUT) :
             output_size != (argument ?
                                 INFERNO_METAL_ARGUMENT_LAYOUT_OUTPUT_SIZE :
                                 INFERNO_METAL_COMPILER_MIN_OUTPUT)) ||
        (imageblock ? (!width || width > 65536 || !height || height > 65536 ||
                       !depth || depth > 65536) :
                      (width != 1 || height != 1 || depth != 1))) {
        return kIOReturnBadArgument;
    }
    if (!caps || !(caps->opcode_mask & (1U << opcode)) ||
        manifest_size > caps->max_input_size ||
        output_size > caps->max_output_size ||
        caps->max_request_size < INFERNO_METAL_USER_SUBMIT_HEADER_SIZE ||
        manifest_size >
            caps->max_request_size - INFERNO_METAL_USER_SUBMIT_HEADER_SIZE) {
        return kIOReturnUnsupported;
    }
    ImtlUserSubmit request = {
        .opcode = opcode,
        .sequence = sequence,
        .output_size = output_size,
        .width = width,
        .height = height,
        .depth = depth,
        .options = INFERNO_METAL_OPTIONS_DEFAULT,
        .input = manifest,
        .input_size = manifest_size,
    };
    return imtl_user_client_submit(client, &request);
}

static bool validDiagnostics(const ImtlCompilerResult *r)
{
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
        r->error_description_length <
            INFERNO_METAL_COMPILER_DESCRIPTION_SIZE - 4) {
        return false;
    }
    if (r->outcome == INFERNO_METAL_COMPILER_OUTCOME_OK) {
        if (no_error) {
            return !r->error_description_length &&
                   r->flags == INFERNO_METAL_COMPILER_FLAG_NO_NSERROR;
        }
        return warning;
    }
    return true;
}

static bool zeroPipeline(const uint8_t *bytes)
{
    return allZero(bytes + INFERNO_METAL_COMPILER_PIPELINE_OFFSET,
                   INFERNO_METAL_COMPILER_PIPELINE_SIZE);
}

static bool zeroLibrary(const uint8_t *bytes)
{
    return allZero(bytes + INFERNO_METAL_COMPILER_LIBRARY_OFFSET,
                   INFERNO_METAL_COMPILER_LIBRARY_SIZE);
}

static bool validPipeline(const ImtlCompilerResult *r, const uint8_t *bytes)
{
    if (r->allocated_size > SIZE_MAX || !r->max_total_threads_per_threadgroup ||
        !r->thread_execution_width ||
        r->pipeline_flags & ~INFERNO_METAL_COMPILER_PIPELINE_FLAG_MASK ||
        !allZero(bytes + INFERNO_METAL_COMPILER_PIPELINE_RESERVED_OFFSET,
                 INFERNO_METAL_COMPILER_PIPELINE_RESERVED_SIZE)) {
        return false;
    }
    if (r->shader_validation < 0 || r->shader_validation > 2) {
        return false;
    }
    return imageblockOpcode(r->opcode) || !r->imageblock_memory_length;
}

static ImtlCompilerSize compilerSize(const uint8_t *record, size_t offset)
{
    return (ImtlCompilerSize){
        .width = get64(record + offset),
        .height = get64(record + offset + 8),
        .depth = get64(record + offset + 16),
    };
}

static bool validRenderPipeline(const ImtlCompilerRenderPipeline *pipeline,
                                const uint8_t *bytes)
{
    const uint8_t *record =
        bytes + INFERNO_METAL_RESOURCE_COMPILER_RENDER_RESULT_OFFSET;
    return pipeline->allocated_size <= SIZE_MAX &&
           pipeline->shader_validation >= 0 &&
           pipeline->shader_validation <= 2 &&
           !(pipeline->flags &
             ~INFERNO_METAL_RESOURCE_RENDER_RESULT_FLAG_MASK) &&
           allZero(record +
                       INFERNO_METAL_RESOURCE_RENDER_RESULT_RESERVED_OFFSET,
                   INFERNO_METAL_RESOURCE_RENDER_RESULT_RESERVED_SIZE);
}

static bool validStage(const ImtlCompilerResult *r)
{
    bool function_failure =
        r->outcome == INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_NOT_FOUND ||
        r->outcome == INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_TYPE_MISMATCH ||
        r->outcome == INFERNO_METAL_COMPILER_OUTCOME_SPECIALIZATION_REQUIRED;
    if (r->opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE && function_failure &&
        r->phase == INFERNO_METAL_COMPILER_PHASE_FUNCTION) {
        return r->stage == INFERNO_METAL_RESOURCE_COMPILER_STAGE_VERTEX ||
               r->stage == INFERNO_METAL_RESOURCE_COMPILER_STAGE_FRAGMENT;
    }
    return r->stage == INFERNO_METAL_RESOURCE_COMPILER_STAGE_NONE;
}

static bool validLibrary(const ImtlCompilerResult *r, const uint8_t *bytes)
{
    if (r->library_type != INFERNO_METAL_LIBRARY_TYPE_EXECUTABLE &&
        r->library_type != INFERNO_METAL_LIBRARY_TYPE_DYNAMIC) {
        return false;
    }
    if (r->library_flags & ~INFERNO_METAL_COMPILER_LIBRARY_FLAG_MASK ||
        get32(bytes + INFERNO_METAL_COMPILER_LIBRARY_RESERVED_OFFSET)) {
        return false;
    }
    const uint8_t *name = bytes + INFERNO_METAL_COMPILER_LIBRARY_NAME_OFFSET;
    bool present =
        r->library_flags & INFERNO_METAL_COMPILER_LIBRARY_FLAG_HAS_INSTALL_NAME;
    return present ?
               validString(name, r->library_install_name_length,
                           INFERNO_METAL_COMPILER_LIBRARY_NAME_SIZE) :
               !r->library_install_name_length &&
                   allZero(name, INFERNO_METAL_COMPILER_LIBRARY_NAME_SIZE);
}

static bool validFailure(const ImtlCompilerResult *r)
{
    bool library = libraryOpcode(r->opcode);
    bool render = r->opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE;
    bool no_error = r->flags & INFERNO_METAL_COMPILER_FLAG_NO_NSERROR;
    if (r->max_total_threads_per_threadgroup || r->thread_execution_width ||
        r->static_threadgroup_memory_length || r->allocated_size ||
        r->required_threads_width || r->required_threads_height ||
        r->required_threads_depth || r->shader_validation ||
        r->imageblock_memory_length || r->pipeline_flags || r->library_type ||
        r->library_flags || r->library_install_name_length) {
        return false;
    }
    switch (r->outcome) {
    case INFERNO_METAL_COMPILER_OUTCOME_COMPILE_FAILED:
        return !r->function_count && !r->metadata_record_count &&
               !r->function_type && !r->required_output_size &&
               (r->phase == INFERNO_METAL_COMPILER_PHASE_LIBRARY ||
                (!library &&
                 r->phase == INFERNO_METAL_COMPILER_PHASE_PIPELINE));
    case INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_NOT_FOUND:
        return !library && no_error && !r->function_count &&
               !r->metadata_record_count && !r->function_type &&
               !r->required_output_size &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_FUNCTION;
    case INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_TYPE_MISMATCH:
        return !library && no_error && !r->function_count &&
               !r->metadata_record_count && !r->required_output_size &&
               knownFunctionType(r->function_type) &&
               (render ?
                    ((r->stage ==
                          INFERNO_METAL_RESOURCE_COMPILER_STAGE_VERTEX &&
                      r->function_type != INFERNO_METAL_FUNCTION_TYPE_VERTEX) ||
                     (r->stage ==
                          INFERNO_METAL_RESOURCE_COMPILER_STAGE_FRAGMENT &&
                      r->function_type !=
                          INFERNO_METAL_FUNCTION_TYPE_FRAGMENT)) :
                    r->function_type != INFERNO_METAL_FUNCTION_TYPE_KERNEL) &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_FUNCTION;
    case INFERNO_METAL_COMPILER_OUTCOME_SPECIALIZATION_REQUIRED:
        return !library && no_error && !r->function_count &&
               !r->metadata_record_count && !r->required_output_size &&
               (render ?
                    ((r->stage ==
                          INFERNO_METAL_RESOURCE_COMPILER_STAGE_VERTEX &&
                      r->function_type == INFERNO_METAL_FUNCTION_TYPE_VERTEX) ||
                     (r->stage ==
                          INFERNO_METAL_RESOURCE_COMPILER_STAGE_FRAGMENT &&
                      r->function_type ==
                          INFERNO_METAL_FUNCTION_TYPE_FRAGMENT)) :
                    r->function_type == INFERNO_METAL_FUNCTION_TYPE_KERNEL) &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_FUNCTION;
    case INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED:
        return library && no_error && !r->required_output_size &&
               !r->function_type &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_INVENTORY;
    case INFERNO_METAL_COMPILER_OUTCOME_OUTPUT_TOO_SMALL:
        return library && no_error && !r->function_type &&
               r->function_count <= INFERNO_METAL_COMPILER_MAX_FUNCTIONS &&
               r->metadata_record_count <=
                   INFERNO_METAL_COMPILER_MAX_METADATA_RECORDS &&
               r->required_output_size ==
                   INFERNO_METAL_COMPILER_MIN_OUTPUT +
                       r->function_count *
                           INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
                       r->metadata_record_count *
                           INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE &&
               r->required_output_size >
                   r->function_records_size +
                       INFERNO_METAL_COMPILER_MIN_OUTPUT &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_INVENTORY;
    case INFERNO_METAL_COMPILER_OUTCOME_ARGUMENT_UNSUPPORTED:
        return r->opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT && no_error &&
               !r->function_count && !r->metadata_record_count &&
               !r->function_type && !r->required_output_size &&
               r->phase == INFERNO_METAL_COMPILER_PHASE_ARGUMENT &&
               r->error_description_length &&
               !(r->flags & (INFERNO_METAL_COMPILER_FLAG_DOMAIN_TRUNCATED |
                             INFERNO_METAL_COMPILER_FLAG_WARNING)) &&
               !(r->flags &
                 ~(INFERNO_METAL_COMPILER_FLAG_NO_NSERROR |
                   INFERNO_METAL_COMPILER_FLAG_DESCRIPTION_TRUNCATED));
    default:
        return false;
    }
}

static bool validMetadataRecord(const uint8_t *record, uint32_t expected_kind,
                                uint64_t *index_out)
{
    uint32_t kind = get32(record + INFERNO_METAL_COMPILER_METADATA_KIND_OFFSET);
    uint32_t type =
        get32(record + INFERNO_METAL_COMPILER_METADATA_DATA_TYPE_OFFSET);
    uint32_t flags =
        get32(record + INFERNO_METAL_COMPILER_METADATA_FLAGS_OFFSET);
    uint32_t length =
        get32(record + INFERNO_METAL_COMPILER_METADATA_NAME_LENGTH_OFFSET);
    uint32_t mask = kind == INFERNO_METAL_COMPILER_METADATA_CONSTANT ?
                        INFERNO_METAL_COMPILER_CONSTANT_FLAG_MASK :
                        INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_MASK;
    if (kind != expected_kind || !knownDataType(type) || flags & ~mask ||
        !length ||
        !validString(record + INFERNO_METAL_COMPILER_METADATA_NAME_OFFSET,
                     length, INFERNO_METAL_COMPILER_METADATA_NAME_SIZE)) {
        return false;
    }
    uint64_t index =
        get64(record + INFERNO_METAL_COMPILER_METADATA_INDEX_OFFSET);
    *index_out = index;
    return true;
}

static bool validFunctionRecord(const uint8_t *record, size_t available,
                                uint32_t *metadata_total, size_t *record_size)
{
    if (available < INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE) {
        return false;
    }
    uint32_t type = get32(record + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET);
    uint32_t name_length =
        get32(record + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET);
    uint32_t patch =
        get32(record + INFERNO_METAL_COMPILER_RECORD_PATCH_TYPE_OFFSET);
    uint32_t flags = get32(record + INFERNO_METAL_COMPILER_RECORD_FLAGS_OFFSET);
    uint64_t options =
        get64(record + INFERNO_METAL_COMPILER_RECORD_OPTIONS_OFFSET);
    uint32_t constants =
        get32(record + INFERNO_METAL_COMPILER_RECORD_CONSTANT_COUNT_OFFSET);
    uint32_t vertex = get32(
        record + INFERNO_METAL_COMPILER_RECORD_VERTEX_ATTRIBUTE_COUNT_OFFSET);
    uint32_t stage =
        get32(record +
              INFERNO_METAL_COMPILER_RECORD_STAGE_INPUT_ATTRIBUTE_COUNT_OFFSET);
    uint32_t encoded_size =
        get32(record + INFERNO_METAL_COMPILER_RECORD_SIZE_OFFSET);
    if (!knownFunctionType(type) || patch > 2 || options & ~0xfULL ||
        flags & ~INFERNO_METAL_COMPILER_FUNCTION_FLAG_MASK ||
        (!(flags &
           INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_VERTEX_ATTRIBUTES) &&
         vertex) ||
        (!(flags &
           INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_STAGE_INPUT_ATTRIBUTES) &&
         stage) ||
        !name_length ||
        !validString(record + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
                     name_length, INFERNO_METAL_COMPILER_FUNCTION_NAME_SIZE)) {
        return false;
    }
    uint64_t count = (uint64_t)constants + vertex + stage;
    uint64_t calculated = INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
                          count * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
    if (count > INFERNO_METAL_COMPILER_MAX_METADATA_RECORDS ||
        calculated != encoded_size || calculated > available) {
        return false;
    }
    const uint8_t *metadata =
        record + INFERNO_METAL_COMPILER_RECORD_METADATA_OFFSET;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t expected =
            i < constants ?
                INFERNO_METAL_COMPILER_METADATA_CONSTANT :
            i < constants + vertex ?
                INFERNO_METAL_COMPILER_METADATA_VERTEX_ATTRIBUTE :
                INFERNO_METAL_COMPILER_METADATA_STAGE_INPUT_ATTRIBUTE;
        const uint8_t *entry =
            metadata + (size_t)i * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
        uint64_t index;
        if (!validMetadataRecord(entry, expected, &index)) {
            return false;
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *previous =
                metadata +
                (size_t)j * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
            if (get32(previous + INFERNO_METAL_COMPILER_METADATA_KIND_OFFSET) !=
                expected) {
                continue;
            }
            uint64_t previous_index =
                get64(previous + INFERNO_METAL_COMPILER_METADATA_INDEX_OFFSET);
            if (previous_index == index) {
                return false;
            }
            if (expected == INFERNO_METAL_COMPILER_METADATA_CONSTANT) {
                uint32_t previous_length =
                    get32(previous +
                          INFERNO_METAL_COMPILER_METADATA_NAME_LENGTH_OFFSET);
                uint32_t length = get32(
                    entry + INFERNO_METAL_COMPILER_METADATA_NAME_LENGTH_OFFSET);
                if (previous_length == length &&
                    !memcmp(previous +
                                INFERNO_METAL_COMPILER_METADATA_NAME_OFFSET,
                            entry + INFERNO_METAL_COMPILER_METADATA_NAME_OFFSET,
                            length)) {
                    return false;
                }
            }
        }
    }
    *metadata_total += (uint32_t)count;
    *record_size = encoded_size;
    return true;
}

static bool argumentConstantType(uint32_t type)
{
    return (type >= 3 && type <= 6) || (type >= 16 && type <= 19) ||
           (type >= 29 && type <= 56);
}

static bool validArgumentMember(const uint8_t *record, uint64_t encoded_length,
                                ImtlArgumentLayoutMember *out)
{
    ImtlArgumentLayoutMember member = {
        .member_id =
            get32(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ID_OFFSET),
        .kind =
            get32(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_KIND_OFFSET),
        .data_type = get32(
            record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_DATA_TYPE_OFFSET),
        .access =
            get32(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ACCESS_OFFSET),
        .byte_offset = get64(
            record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_BYTE_OFFSET_OFFSET),
        .constant_size = get32(
            record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_CONSTANT_SIZE_OFFSET),
        .texture_type = get32(
            record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_TEXTURE_TYPE_OFFSET),
        .texture_data_type = get32(
            record +
            INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_TEXTURE_DATA_TYPE_OFFSET),
        .depth =
            get32(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_DEPTH_OFFSET),
        .array_length = get32(
            record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ARRAY_LENGTH_OFFSET),
        .argument_index_stride = get32(
            record +
            INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ARGUMENT_INDEX_STRIDE_OFFSET),
        .element_data_type = get32(
            record +
            INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ELEMENT_DATA_TYPE_OFFSET),
    };
    if (member.kind < INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER ||
        member.kind > INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT ||
        member.byte_offset >= encoded_length ||
        !allZero(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_RESERVED_OFFSET,
                 INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_RESERVED_SIZE)) {
        return false;
    }
    bool array = member.array_length || member.argument_index_stride ||
                 member.element_data_type;
    if (array &&
        (member.kind != INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE ||
         member.data_type != 2 || member.element_data_type != 58 ||
         member.array_length < 2 || !member.argument_index_stride)) {
        return false;
    }
    bool texture_shape =
        member.texture_type == 2 &&
        (member.texture_data_type == 3 || member.texture_data_type == 16) &&
        !member.depth;
    switch (member.kind) {
    case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER:
        if (member.data_type != 60 || member.access > 1 ||
            member.constant_size || member.texture_type ||
            member.texture_data_type || member.depth || array) {
            return false;
        }
        break;
    case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE:
        if ((!array && member.data_type != 58) || member.access ||
            member.constant_size || !texture_shape) {
            return false;
        }
        break;
    case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER:
        if (member.data_type != 59 || member.access || member.constant_size ||
            member.texture_type || member.texture_data_type || member.depth ||
            array) {
            return false;
        }
        break;
    case INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT:
        if (!argumentConstantType(member.data_type) || member.access ||
            !member.constant_size ||
            member.constant_size > INFERNO_METAL_ARGUMENT_MAX_CONSTANT ||
            member.constant_size > encoded_length ||
            member.byte_offset > encoded_length - member.constant_size ||
            member.texture_type || member.texture_data_type || member.depth ||
            array) {
            return false;
        }
        break;
    }
    *out = member;
    return true;
}

static bool argumentMemberContainsId(const ImtlArgumentLayoutMember *member,
                                     uint32_t id)
{
    uint32_t count = member->array_length ? member->array_length : 1;
    uint32_t stride = member->array_length ? member->argument_index_stride : 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t expanded = (uint64_t)member->member_id + (uint64_t)i * stride;
        if (expanded == id) {
            return true;
        }
    }
    return false;
}

static bool validArgumentLayout(const uint8_t *bytes, ImtlArgumentLayout *out)
{
    const uint8_t *header = bytes + INFERNO_METAL_ARGUMENT_LAYOUT_HEADER_OFFSET;
    ImtlArgumentLayout layout = {
        .encoded_length =
            get64(header + INFERNO_METAL_ARGUMENT_LAYOUT_ENCODED_LENGTH_OFFSET),
        .alignment =
            get64(header + INFERNO_METAL_ARGUMENT_LAYOUT_ALIGNMENT_OFFSET),
        .member_count =
            get32(header + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_COUNT_OFFSET),
        .flags = get32(header + INFERNO_METAL_ARGUMENT_LAYOUT_FLAGS_OFFSET),
        .members = bytes + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBERS_OFFSET,
    };
    if (!layout.encoded_length ||
        layout.encoded_length > INFERNO_METAL_ARGUMENT_MAX_ENCODED_LENGTH ||
        !layout.alignment || layout.alignment > 4096 ||
        (layout.alignment & (layout.alignment - 1)) || layout.flags ||
        !layout.member_count ||
        layout.member_count > INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS ||
        !allZero(header + INFERNO_METAL_ARGUMENT_LAYOUT_RESERVED_OFFSET,
                 INFERNO_METAL_ARGUMENT_LAYOUT_RESERVED_SIZE)) {
        return false;
    }
    ImtlArgumentLayoutMember decoded[INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS];
    uint32_t expanded_count = 0;
    for (uint32_t i = 0; i < layout.member_count; i++) {
        const uint8_t *record =
            layout.members +
            (size_t)i * INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_SIZE;
        if (!validArgumentMember(record, layout.encoded_length, &decoded[i]) ||
            (i && decoded[i].member_id <= decoded[i - 1].member_id)) {
            return false;
        }
        uint32_t count = decoded[i].array_length ? decoded[i].array_length : 1;
        if (count >
            INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS - expanded_count) {
            return false;
        }
        for (uint32_t j = 0; j < count; j++) {
            uint64_t id = (uint64_t)decoded[i].member_id +
                          (uint64_t)j * decoded[i].argument_index_stride;
            if (id > UINT32_MAX) {
                return false;
            }
            for (uint32_t k = 0; k < i; k++) {
                if (argumentMemberContainsId(&decoded[k], (uint32_t)id)) {
                    return false;
                }
            }
        }
        if (decoded[i].kind ==
            INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
            uint64_t end = decoded[i].byte_offset + decoded[i].constant_size;
            for (uint32_t k = 0; k < i; k++) {
                if (decoded[k].kind ==
                    INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
                    uint64_t prior_end =
                        decoded[k].byte_offset + decoded[k].constant_size;
                    if (decoded[i].byte_offset < prior_end &&
                        decoded[k].byte_offset < end) {
                        return false;
                    }
                }
            }
        }
        expanded_count += count;
    }
    size_t used =
        (size_t)layout.member_count * INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_SIZE;
    size_t capacity = (size_t)INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS *
                      INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_SIZE;
    if (!allZero(layout.members + used, capacity - used)) {
        return false;
    }
    *out = layout;
    return true;
}

bool imtl_compiler_decode_result(const uint8_t *bytes, size_t size,
                                 uint32_t expected_opcode,
                                 uint64_t expected_sequence,
                                 ImtlCompilerResult *out)
{
    if (!bytes || !out || !compilerOpcode(expected_opcode) ||
        size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
        size > INFERNO_METAL_COMPILER_MAX_OUTPUT ||
        (expected_opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ?
             size != INFERNO_METAL_ARGUMENT_LAYOUT_OUTPUT_SIZE :
             (typedOpcode(expected_opcode) &&
              expected_opcode != INFERNO_METAL_QUERY_LIBRARY_TYPED &&
              size != INFERNO_METAL_COMPILER_MIN_OUTPUT))) {
        return false;
    }
    const uint8_t *render =
        bytes + INFERNO_METAL_RESOURCE_COMPILER_RENDER_RESULT_OFFSET;
    ImtlCompilerResult r = {
        .opcode = get32(bytes + INFERNO_METAL_COMPILER_OPCODE_OFFSET),
        .sequence = get64(bytes + INFERNO_METAL_COMPILER_SEQUENCE_OFFSET),
        .outcome = get32(bytes + INFERNO_METAL_COMPILER_OUTCOME_OFFSET),
        .phase = get32(bytes + INFERNO_METAL_COMPILER_PHASE_OFFSET),
        .flags = get32(bytes + INFERNO_METAL_COMPILER_FLAGS_OFFSET),
        .stage = get32(bytes + INFERNO_METAL_RESOURCE_COMPILER_STAGE_OFFSET),
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
        .required_output_size =
            get32(bytes + INFERNO_METAL_COMPILER_REQUIRED_OUTPUT_SIZE_OFFSET),
        .metadata_record_count =
            get32(bytes + INFERNO_METAL_COMPILER_METADATA_RECORD_COUNT_OFFSET),
        .error_code =
            getSigned64(bytes + INFERNO_METAL_COMPILER_ERROR_CODE_OFFSET),
        .error_domain =
            (const char *)bytes + INFERNO_METAL_COMPILER_DOMAIN_OFFSET,
        .error_domain_length =
            get32(bytes + INFERNO_METAL_COMPILER_DOMAIN_LENGTH_OFFSET),
        .error_description =
            (const char *)bytes + INFERNO_METAL_COMPILER_DESCRIPTION_OFFSET,
        .error_description_length =
            get32(bytes + INFERNO_METAL_COMPILER_DESCRIPTION_LENGTH_OFFSET),
        .allocated_size = get64(
            bytes + INFERNO_METAL_COMPILER_PIPELINE_ALLOCATED_SIZE_OFFSET),
        .required_threads_width = get64(
            bytes + INFERNO_METAL_COMPILER_PIPELINE_REQUIRED_WIDTH_OFFSET),
        .required_threads_height = get64(
            bytes + INFERNO_METAL_COMPILER_PIPELINE_REQUIRED_HEIGHT_OFFSET),
        .required_threads_depth = get64(
            bytes + INFERNO_METAL_COMPILER_PIPELINE_REQUIRED_DEPTH_OFFSET),
        .shader_validation = getSigned64(
            bytes + INFERNO_METAL_COMPILER_PIPELINE_SHADER_VALIDATION_OFFSET),
        .imageblock_memory_length = get64(
            bytes +
            INFERNO_METAL_COMPILER_PIPELINE_IMAGEBLOCK_MEMORY_LENGTH_OFFSET),
        .pipeline_flags =
            get32(bytes + INFERNO_METAL_COMPILER_PIPELINE_FLAGS_OFFSET),
        .library_type =
            get32(bytes + INFERNO_METAL_COMPILER_LIBRARY_TYPE_OFFSET),
        .library_flags =
            get32(bytes + INFERNO_METAL_COMPILER_LIBRARY_FLAGS_OFFSET),
        .library_install_name =
            (const char *)bytes + INFERNO_METAL_COMPILER_LIBRARY_NAME_OFFSET,
        .library_install_name_length =
            get32(bytes + INFERNO_METAL_COMPILER_LIBRARY_NAME_LENGTH_OFFSET),
        .render_pipeline =
            {
                .allocated_size = get64(
                    render +
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_ALLOCATED_SIZE_OFFSET),
                .imageblock_sample_length = get64(
                    render + INFERNO_METAL_RESOURCE_RENDER_RESULT_IMAGEBLOCK_SAMPLE_LENGTH_OFFSET),
                .max_total_threads_per_threadgroup = get64(
                    render +
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_TOTAL_THREADS_OFFSET),
                .max_total_threads_per_object_threadgroup = get64(
                    render +
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_OBJECT_THREADS_OFFSET),
                .max_total_threads_per_mesh_threadgroup = get64(
                    render +
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_MESH_THREADS_OFFSET),
                .object_thread_execution_width = get64(
                    render + INFERNO_METAL_RESOURCE_RENDER_RESULT_OBJECT_EXECUTION_WIDTH_OFFSET),
                .mesh_thread_execution_width = get64(
                    render + INFERNO_METAL_RESOURCE_RENDER_RESULT_MESH_EXECUTION_WIDTH_OFFSET),
                .max_total_threadgroups_per_mesh_grid = get64(
                    render + INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_MESH_THREADGROUPS_OFFSET),
                .shader_validation = getSigned64(
                    render +
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_SHADER_VALIDATION_OFFSET),
                .required_threads_per_tile_threadgroup = compilerSize(
                    render,
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_TILE_THREADS_OFFSET),
                .required_threads_per_object_threadgroup = compilerSize(
                    render,
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_OBJECT_THREADS_OFFSET),
                .required_threads_per_mesh_threadgroup = compilerSize(
                    render,
                    INFERNO_METAL_RESOURCE_RENDER_RESULT_MESH_THREADS_OFFSET),
                .flags = get32(
                    render + INFERNO_METAL_RESOURCE_RENDER_RESULT_FLAGS_OFFSET),
            },
        .function_records = bytes + INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET,
        .function_records_size = size - INFERNO_METAL_COMPILER_MIN_OUTPUT,
        .argument_layout =
            expected_opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ?
                bytes + INFERNO_METAL_ARGUMENT_LAYOUT_HEADER_OFFSET :
                NULL,
        .argument_layout_size =
            expected_opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ?
                INFERNO_METAL_ARGUMENT_LAYOUT_OUTPUT_SIZE -
                    INFERNO_METAL_ARGUMENT_LAYOUT_HEADER_OFFSET :
                0,
    };
    if (expected_opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE) {
        r.library_type = 0;
        r.library_flags = 0;
        r.library_install_name = NULL;
        r.library_install_name_length = 0;
    } else {
        memset(&r.render_pipeline, 0, sizeof(r.render_pipeline));
    }
    uint32_t expected_version = typedOpcode(expected_opcode) ?
                                    INFERNO_METAL_RESOURCE_VERSION :
                                    INFERNO_METAL_VERSION;
    const uint8_t *reserved =
        bytes + (typedOpcode(expected_opcode) ?
                     INFERNO_METAL_RESOURCE_COMPILER_RESERVED_OFFSET :
                     INFERNO_METAL_COMPILER_RESERVED_OFFSET);
    size_t reserved_size = typedOpcode(expected_opcode) ?
                               INFERNO_METAL_RESOURCE_COMPILER_RESERVED_SIZE :
                               INFERNO_METAL_COMPILER_RESERVED_SIZE;
    if (get32(bytes + INFERNO_METAL_COMPILER_VERSION_OFFSET) !=
            expected_version ||
        r.opcode != expected_opcode || r.sequence != expected_sequence ||
        r.flags & ~INFERNO_METAL_COMPILER_FLAG_MASK ||
        !allZero(reserved, reserved_size) || !validStage(&r) ||
        !validString(bytes + INFERNO_METAL_COMPILER_DOMAIN_OFFSET,
                     r.error_domain_length,
                     INFERNO_METAL_COMPILER_DOMAIN_SIZE) ||
        !validString(bytes + INFERNO_METAL_COMPILER_DESCRIPTION_OFFSET,
                     r.error_description_length,
                     INFERNO_METAL_COMPILER_DESCRIPTION_SIZE) ||
        !validDiagnostics(&r)) {
        return false;
    }
    if (r.outcome != INFERNO_METAL_COMPILER_OUTCOME_OK) {
        if (!zeroPipeline(bytes) || !zeroLibrary(bytes) || !validFailure(&r) ||
            !allZero(r.function_records, r.function_records_size)) {
            return false;
        }
        *out = r;
        return true;
    }
    if (r.required_output_size) {
        return false;
    }
    if (r.opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) {
        ImtlArgumentLayout layout;
        if (r.phase != INFERNO_METAL_COMPILER_PHASE_ARGUMENT ||
            r.function_count || r.metadata_record_count || r.function_type ||
            r.max_total_threads_per_threadgroup || r.thread_execution_width ||
            r.static_threadgroup_memory_length || !zeroPipeline(bytes) ||
            !zeroLibrary(bytes) || !validArgumentLayout(bytes, &layout)) {
            return false;
        }
        *out = r;
        return true;
    }
    if (r.opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE) {
        if (r.phase != INFERNO_METAL_COMPILER_PHASE_PIPELINE ||
            size != INFERNO_METAL_COMPILER_MIN_OUTPUT || r.function_count ||
            r.metadata_record_count || r.function_type ||
            r.max_total_threads_per_threadgroup || r.thread_execution_width ||
            r.static_threadgroup_memory_length || !zeroPipeline(bytes) ||
            !validRenderPipeline(&r.render_pipeline, bytes) ||
            !allZero(r.function_records, r.function_records_size)) {
            return false;
        }
        *out = r;
        return true;
    }
    if (!libraryOpcode(r.opcode)) {
        if (r.phase != INFERNO_METAL_COMPILER_PHASE_PIPELINE ||
            size != INFERNO_METAL_COMPILER_MIN_OUTPUT || r.function_count ||
            r.metadata_record_count ||
            r.function_type != INFERNO_METAL_FUNCTION_TYPE_KERNEL ||
            !zeroLibrary(bytes) || !validPipeline(&r, bytes) ||
            !allZero(r.function_records, r.function_records_size)) {
            return false;
        }
        *out = r;
        return true;
    }
    if (r.phase != INFERNO_METAL_COMPILER_PHASE_INVENTORY || r.function_type ||
        r.max_total_threads_per_threadgroup || r.thread_execution_width ||
        r.static_threadgroup_memory_length || !zeroPipeline(bytes) ||
        !validLibrary(&r, bytes) ||
        r.function_count > INFERNO_METAL_COMPILER_MAX_FUNCTIONS ||
        r.metadata_record_count > INFERNO_METAL_COMPILER_MAX_METADATA_RECORDS) {
        return false;
    }
    size_t offset = 0;
    uint32_t metadata_total = 0;
    for (uint32_t i = 0; i < r.function_count; i++) {
        size_t record_size;
        if (!validFunctionRecord(r.function_records + offset,
                                 r.function_records_size - offset,
                                 &metadata_total, &record_size)) {
            return false;
        }
        const uint8_t *name = r.function_records + offset +
                              INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET;
        uint32_t name_length =
            get32(r.function_records + offset +
                  INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET);
        size_t previous_offset = 0;
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *previous = r.function_records + previous_offset;
            uint32_t previous_length = get32(
                previous + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET);
            if (previous_length == name_length &&
                !memcmp(previous + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
                        name, name_length)) {
                return false;
            }
            previous_offset +=
                get32(previous + INFERNO_METAL_COMPILER_RECORD_SIZE_OFFSET);
        }
        offset += record_size;
    }
    if (metadata_total != r.metadata_record_count ||
        !allZero(r.function_records + offset,
                 r.function_records_size - offset)) {
        return false;
    }
    *out = r;
    return true;
}

bool imtl_compiler_function_at(const ImtlCompilerResult *result, uint32_t index,
                               ImtlCompilerFunction *out)
{
    if (!result || !out || !libraryOpcode(result->opcode) ||
        result->outcome != INFERNO_METAL_COMPILER_OUTCOME_OK ||
        index >= result->function_count || !result->function_records) {
        return false;
    }
    const uint8_t *record = result->function_records;
    for (uint32_t i = 0; i < index; i++) {
        record += get32(record + INFERNO_METAL_COMPILER_RECORD_SIZE_OFFSET);
    }
    ImtlCompilerFunction function = {
        .type = get32(record + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET),
        .name =
            (const char *)record + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
        .name_length =
            get32(record + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET),
        .patch_type =
            get32(record + INFERNO_METAL_COMPILER_RECORD_PATCH_TYPE_OFFSET),
        .flags = get32(record + INFERNO_METAL_COMPILER_RECORD_FLAGS_OFFSET),
        .patch_control_point_count = getSigned64(
            record +
            INFERNO_METAL_COMPILER_RECORD_PATCH_CONTROL_POINT_COUNT_OFFSET),
        .options = get64(record + INFERNO_METAL_COMPILER_RECORD_OPTIONS_OFFSET),
        .constant_count =
            get32(record + INFERNO_METAL_COMPILER_RECORD_CONSTANT_COUNT_OFFSET),
        .vertex_attribute_count =
            get32(record +
                  INFERNO_METAL_COMPILER_RECORD_VERTEX_ATTRIBUTE_COUNT_OFFSET),
        .stage_input_attribute_count = get32(
            record +
            INFERNO_METAL_COMPILER_RECORD_STAGE_INPUT_ATTRIBUTE_COUNT_OFFSET),
        .metadata_records =
            record + INFERNO_METAL_COMPILER_RECORD_METADATA_OFFSET,
    };
    *out = function;
    return true;
}

bool imtl_compiler_metadata_at(const ImtlCompilerFunction *function,
                               uint32_t index, ImtlCompilerMetadata *out)
{
    if (!function || !out || !function->metadata_records ||
        index >= function->constant_count + function->vertex_attribute_count +
                     function->stage_input_attribute_count) {
        return false;
    }
    const uint8_t *record =
        function->metadata_records +
        (size_t)index * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
    ImtlCompilerMetadata metadata = {
        .kind = get32(record + INFERNO_METAL_COMPILER_METADATA_KIND_OFFSET),
        .data_type =
            get32(record + INFERNO_METAL_COMPILER_METADATA_DATA_TYPE_OFFSET),
        .index = get64(record + INFERNO_METAL_COMPILER_METADATA_INDEX_OFFSET),
        .flags = get32(record + INFERNO_METAL_COMPILER_METADATA_FLAGS_OFFSET),
        .name =
            (const char *)record + INFERNO_METAL_COMPILER_METADATA_NAME_OFFSET,
        .name_length =
            get32(record + INFERNO_METAL_COMPILER_METADATA_NAME_LENGTH_OFFSET),
    };
    *out = metadata;
    return true;
}

bool imtl_compiler_argument_layout(const ImtlCompilerResult *result,
                                   ImtlArgumentLayout *out)
{
    if (!result || !out ||
        result->opcode != INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ||
        result->outcome != INFERNO_METAL_COMPILER_OUTCOME_OK ||
        !result->argument_layout ||
        result->argument_layout_size !=
            INFERNO_METAL_ARGUMENT_LAYOUT_OUTPUT_SIZE -
                INFERNO_METAL_ARGUMENT_LAYOUT_HEADER_OFFSET) {
        return false;
    }
    const uint8_t *header = result->argument_layout;
    ImtlArgumentLayout layout = {
        .encoded_length =
            get64(header + INFERNO_METAL_ARGUMENT_LAYOUT_ENCODED_LENGTH_OFFSET),
        .alignment =
            get64(header + INFERNO_METAL_ARGUMENT_LAYOUT_ALIGNMENT_OFFSET),
        .member_count =
            get32(header + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_COUNT_OFFSET),
        .flags = get32(header + INFERNO_METAL_ARGUMENT_LAYOUT_FLAGS_OFFSET),
        .members = header + INFERNO_METAL_ARGUMENT_LAYOUT_HEADER_SIZE,
    };
    *out = layout;
    return true;
}

bool imtl_argument_layout_member_at(const ImtlArgumentLayout *layout,
                                    uint32_t index,
                                    ImtlArgumentLayoutMember *out)
{
    if (!layout || !out || !layout->members || index >= layout->member_count) {
        return false;
    }
    const uint8_t *record =
        layout->members +
        (size_t)index * INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_SIZE;
    ImtlArgumentLayoutMember member;
    if (!validArgumentMember(record, layout->encoded_length, &member)) {
        return false;
    }
    *out = member;
    return true;
}
