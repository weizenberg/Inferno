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

#ifndef STANDARD_HEADERS_INFERNO_METAL_USER_H
#define STANDARD_HEADERS_INFERNO_METAL_USER_H

#include "metal.h"

#define INFERNO_METAL_USER_VERSION 2U
#define INFERNO_METAL_USER_SUBMIT_HEADER_SIZE 192U
#define INFERNO_METAL_USER_CAPABILITIES_SIZE 32U
#define INFERNO_METAL_USER_STATUS_SIZE 32U
#define INFERNO_METAL_USER_INLINE_MAX 4096U
#define INFERNO_METAL_USER_MAX_REQUEST                                  \
    (INFERNO_METAL_USER_SUBMIT_HEADER_SIZE + INFERNO_METAL_MAX_SOURCE + \
     INFERNO_METAL_MAX_BUFFER)

/* Synchronous methods; GPU execution itself is asynchronous. Wire structures
 * are little endian and contain neither user pointers nor physical addresses.
 * Submit: [192-byte header][source_size bytes][input_size bytes].
 * Read: one uint64 scalar byte offset, variable bounded structure output.
 * Capabilities/status: no input, fixed-size structure output. ACK/reset: none.
 */
enum {
    INFERNO_METAL_USER_CONNECTION_TYPE = 0,
};

enum {
    INFERNO_METAL_USER_CAPABILITIES = 0,
    INFERNO_METAL_USER_SUBMIT = 1,
    INFERNO_METAL_USER_STATUS = 2,
    INFERNO_METAL_USER_READ = 3,
    INFERNO_METAL_USER_ACK = 4,
    INFERNO_METAL_USER_RESET = 5,
    INFERNO_METAL_USER_METHOD_COUNT = 6,
};

/* Stable wire values for the status transport-result field. */
enum {
    INFERNO_METAL_USER_RESULT_OK = 0,
    INFERNO_METAL_USER_RESULT_AGAIN = 1,
    INFERNO_METAL_USER_RESULT_BUSY = 2,
    INFERNO_METAL_USER_RESULT_BAD_ARGUMENT = 3,
    INFERNO_METAL_USER_RESULT_BAD_DEVICE = 4,
    INFERNO_METAL_USER_RESULT_SEQUENCE_MISMATCH = 5,
};

/* Offsets within the packed submission header. All fields are uint32 except
 * sequence (uint64), and the two zero-terminated 64-byte function names.
 * Flags, options and reserved are zero in version 2. No native C struct is the
 * ABI.
 */
enum {
    INFERNO_METAL_USER_VERSION_OFFSET = 0,
    INFERNO_METAL_USER_OPCODE_OFFSET = 4,
    INFERNO_METAL_USER_SEQUENCE_OFFSET = 8,
    INFERNO_METAL_USER_SOURCE_SIZE_OFFSET = 16,
    INFERNO_METAL_USER_INPUT_SIZE_OFFSET = 20,
    INFERNO_METAL_USER_OUTPUT_SIZE_OFFSET = 24,
    INFERNO_METAL_USER_WIDTH_OFFSET = 28,
    INFERNO_METAL_USER_HEIGHT_OFFSET = 32,
    INFERNO_METAL_USER_DEPTH_OFFSET = 36,
    INFERNO_METAL_USER_FUNCTION_OFFSET = 40,
    INFERNO_METAL_USER_FRAGMENT_OFFSET = 104,
    INFERNO_METAL_USER_FLAGS_OFFSET = 168,
    INFERNO_METAL_USER_RESERVED_OFFSET = 172,
    INFERNO_METAL_USER_OPTIONS_OFFSET = 176,
    INFERNO_METAL_USER_V2_RESERVED_OFFSET = 180,
    INFERNO_METAL_USER_V2_RESERVED_SIZE = 12,
};

/* Capabilities: eight uint32 fields, all little endian. Opcode mask uses
 * bit (1U << opcode). Flags are zero in version 2.
 */
enum {
    INFERNO_METAL_USER_CAP_VERSION_OFFSET = 0,
    INFERNO_METAL_USER_CAP_SIZE_OFFSET = 4,
    INFERNO_METAL_USER_CAP_OPCODES_OFFSET = 8,
    INFERNO_METAL_USER_CAP_FLAGS_OFFSET = 12,
    INFERNO_METAL_USER_CAP_SOURCE_OFFSET = 16,
    INFERNO_METAL_USER_CAP_INPUT_OFFSET = 20,
    INFERNO_METAL_USER_CAP_OUTPUT_OFFSET = 24,
    INFERNO_METAL_USER_CAP_REQUEST_OFFSET = 28,
};

/* Status: uint32 fields except sequence (uint64). Timer error encodes the
 * 32-bit IOReturn bit pattern. Reserved is zero. Only the owning connection's
 * completion sequence/error may appear here.
 */
enum {
    INFERNO_METAL_USER_STATUS_VERSION_OFFSET = 0,
    INFERNO_METAL_USER_STATUS_STATE_OFFSET = 4,
    INFERNO_METAL_USER_STATUS_TRANSPORT_OFFSET = 8,
    INFERNO_METAL_USER_STATUS_TIMER_OFFSET = 12,
    INFERNO_METAL_USER_STATUS_SEQUENCE_OFFSET = 16,
    INFERNO_METAL_USER_STATUS_ERROR_OFFSET = 24,
    INFERNO_METAL_USER_STATUS_RESERVED_OFFSET = 28,
};

enum {
    INFERNO_METAL_USER_IDLE = 0,
    INFERNO_METAL_USER_SUBMITTED = 1,
    INFERNO_METAL_USER_COMPLETED = 2,
    INFERNO_METAL_USER_RESETTING = 3,
    INFERNO_METAL_USER_FAULTED = 4,
    INFERNO_METAL_USER_DRAINING = 5,
    INFERNO_METAL_USER_DRAINED = 6,
    INFERNO_METAL_USER_STOPPED = 7,
};

#endif
