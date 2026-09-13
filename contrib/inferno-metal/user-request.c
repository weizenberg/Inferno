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

#include "user-request.h"

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

static bool emptyNames(const InfernoMetalCommand *command)
{
    return allZero((const uint8_t *)command->function,
                   sizeof(command->function)) &&
           allZero((const uint8_t *)command->fragment,
                   sizeof(command->fragment));
}

static bool validLegacyQuery(const InfernoMetalCommand *command)
{
    bool library = command->opcode == INFERNO_METAL_QUERY_LIBRARY;
    bool imageblock = command->opcode == INFERNO_METAL_QUERY_IMAGEBLOCK;

    return command->source_size && !command->input_size &&
           (imageblock ? (command->width && command->width <= 65536 &&
                          command->height && command->height <= 65536 &&
                          command->depth && command->depth <= 65536) :
                         (command->width == 1 && command->height == 1 &&
                          command->depth == 1)) &&
           command->output_size >= INFERNO_METAL_COMPILER_MIN_OUTPUT &&
           command->output_size <= INFERNO_METAL_COMPILER_MAX_OUTPUT &&
           (library ||
            command->output_size == INFERNO_METAL_COMPILER_MIN_OUTPUT) &&
           allZero((const uint8_t *)command->fragment,
                   sizeof(command->fragment)) &&
           (library ? allZero((const uint8_t *)command->function,
                              sizeof(command->function)) :
                      command->function[0]);
}

static bool validTypedQuery(const InfernoMetalCommand *command)
{
    bool library = command->opcode == INFERNO_METAL_QUERY_LIBRARY_TYPED;
    bool imageblock = command->opcode == INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED;
    bool argument = command->opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT;

    return !command->source_size &&
           command->input_size >= INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE &&
           (imageblock ? (command->width && command->width <= 65536 &&
                          command->height && command->height <= 65536 &&
                          command->depth && command->depth <= 65536) :
                         (command->width == 1 && command->height == 1 &&
                          command->depth == 1)) &&
           (library ?
                (command->output_size >= INFERNO_METAL_COMPILER_MIN_OUTPUT &&
                 command->output_size <= INFERNO_METAL_COMPILER_MAX_OUTPUT) :
                command->output_size ==
                    (argument ? INFERNO_METAL_ARGUMENT_LAYOUT_OUTPUT_SIZE :
                                INFERNO_METAL_COMPILER_MIN_OUTPUT)) &&
           emptyNames(command);
}

bool imtl_user_decode(const void *packet, size_t size, ImtlUserRequest *result)
{
    if (!packet || !result || size < INFERNO_METAL_USER_SUBMIT_HEADER_SIZE ||
        size > INFERNO_METAL_USER_MAX_REQUEST) {
        return false;
    }
    const uint8_t *p = packet;
    if (get32(p + INFERNO_METAL_USER_VERSION_OFFSET) !=
            INFERNO_METAL_USER_VERSION ||
        get32(p + INFERNO_METAL_USER_FLAGS_OFFSET) ||
        get32(p + INFERNO_METAL_USER_RESERVED_OFFSET) ||
        !allZero(p + INFERNO_METAL_USER_V2_RESERVED_OFFSET,
                 INFERNO_METAL_USER_V2_RESERVED_SIZE)) {
        return false;
    }
    ImtlUserRequest r = { 0 };
    r.command.opcode = get32(p + INFERNO_METAL_USER_OPCODE_OFFSET);
    r.command.sequence = get64(p + INFERNO_METAL_USER_SEQUENCE_OFFSET);
    r.command.source_size = get32(p + INFERNO_METAL_USER_SOURCE_SIZE_OFFSET);
    r.command.input_size = get32(p + INFERNO_METAL_USER_INPUT_SIZE_OFFSET);
    r.command.output_size = get32(p + INFERNO_METAL_USER_OUTPUT_SIZE_OFFSET);
    r.command.width = get32(p + INFERNO_METAL_USER_WIDTH_OFFSET);
    r.command.height = get32(p + INFERNO_METAL_USER_HEIGHT_OFFSET);
    r.command.depth = get32(p + INFERNO_METAL_USER_DEPTH_OFFSET);
    r.command.options = get32(p + INFERNO_METAL_USER_OPTIONS_OFFSET);
    if (r.command.opcode < INFERNO_METAL_COMPUTE ||
        r.command.opcode > INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ||
        r.command.options || r.command.source_size > INFERNO_METAL_MAX_SOURCE ||
        r.command.input_size > INFERNO_METAL_MAX_BUFFER ||
        !r.command.output_size ||
        r.command.output_size > INFERNO_METAL_MAX_BUFFER) {
        return false;
    }
    /* The validated caps keep this sum below 2^32 on either host ABI. */
    size_t expected = INFERNO_METAL_USER_SUBMIT_HEADER_SIZE +
                      r.command.source_size + r.command.input_size;
    if (size != expected) {
        return false;
    }
    bool function_end = false;
    bool fragment_end = false;
    for (unsigned i = 0; i < 64; i++) {
        r.command.function[i] = (char)p[INFERNO_METAL_USER_FUNCTION_OFFSET + i];
        r.command.fragment[i] = (char)p[INFERNO_METAL_USER_FRAGMENT_OFFSET + i];
        function_end |= r.command.function[i] == 0;
        fragment_end |= r.command.fragment[i] == 0;
    }
    if (!function_end || !fragment_end) {
        return false;
    }
    switch (r.command.opcode) {
    case INFERNO_METAL_BATCH:
        if (r.command.source_size ||
            r.command.input_size < INFERNO_METAL_BATCH_HEADER_SIZE ||
            r.command.output_size < INFERNO_METAL_BATCH_RESULT_SIZE ||
            r.command.width != 1 || r.command.height != 1 ||
            r.command.depth != 1 || !emptyNames(&r.command)) {
            return false;
        }
        break;
    case INFERNO_METAL_BATCH_RESOURCES:
        if (r.command.source_size ||
            r.command.input_size < INFERNO_METAL_RESOURCE_HEADER_SIZE ||
            r.command.output_size < INFERNO_METAL_RESOURCE_RESULT_SIZE ||
            r.command.width != 1 || r.command.height != 1 ||
            r.command.depth != 1 || !emptyNames(&r.command)) {
            return false;
        }
        break;
    case INFERNO_METAL_QUERY_LIBRARY:
    case INFERNO_METAL_QUERY_PIPELINE:
    case INFERNO_METAL_QUERY_IMAGEBLOCK:
        if (!validLegacyQuery(&r.command)) {
            return false;
        }
        break;
    case INFERNO_METAL_QUERY_LIBRARY_TYPED:
    case INFERNO_METAL_QUERY_PIPELINE_TYPED:
    case INFERNO_METAL_QUERY_RENDER_PIPELINE:
    case INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED:
    case INFERNO_METAL_QUERY_ARGUMENT_LAYOUT:
        if (!validTypedQuery(&r.command)) {
            return false;
        }
        break;
    case INFERNO_METAL_COMPUTE:
    case INFERNO_METAL_RENDER:
    case INFERNO_METAL_CLEAR:
        break;
    default:
        return false;
    }
    r.source = r.command.source_size ?
                   p + INFERNO_METAL_USER_SUBMIT_HEADER_SIZE :
                   NULL;
    r.input = r.command.input_size ? p + INFERNO_METAL_USER_SUBMIT_HEADER_SIZE +
                                         r.command.source_size :
                                     NULL;
    *result = r;
    return true;
}
