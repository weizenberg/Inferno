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
        r.command.opcode > INFERNO_METAL_BATCH || r.command.options ||
        r.command.source_size > INFERNO_METAL_MAX_SOURCE ||
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
    if (r.command.opcode == INFERNO_METAL_BATCH) {
        if (r.command.source_size ||
            r.command.input_size < INFERNO_METAL_BATCH_HEADER_SIZE ||
            r.command.output_size < INFERNO_METAL_BATCH_RESULT_SIZE ||
            r.command.width != 1 || r.command.height != 1 ||
            r.command.depth != 1 ||
            !allZero((const uint8_t *)r.command.function,
                     sizeof(r.command.function)) ||
            !allZero((const uint8_t *)r.command.fragment,
                     sizeof(r.command.fragment))) {
            return false;
        }
    } else if (r.command.opcode >= INFERNO_METAL_QUERY_LIBRARY) {
        bool library = r.command.opcode == INFERNO_METAL_QUERY_LIBRARY;
        bool imageblock = r.command.opcode == INFERNO_METAL_QUERY_IMAGEBLOCK;

        if (!r.command.source_size || r.command.input_size ||
            (!imageblock && (r.command.width != 1 || r.command.height != 1 ||
                             r.command.depth != 1)) ||
            (imageblock && (!r.command.width || r.command.width > 65536 ||
                            !r.command.height || r.command.height > 65536 ||
                            !r.command.depth || r.command.depth > 65536)) ||
            r.command.output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
            r.command.output_size > INFERNO_METAL_COMPILER_MAX_OUTPUT ||
            (!library &&
             r.command.output_size != INFERNO_METAL_COMPILER_MIN_OUTPUT) ||
            !allZero((const uint8_t *)r.command.fragment,
                     sizeof(r.command.fragment)) ||
            (library ? !allZero((const uint8_t *)r.command.function,
                                sizeof(r.command.function)) :
                       !r.command.function[0])) {
            return false;
        }
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
