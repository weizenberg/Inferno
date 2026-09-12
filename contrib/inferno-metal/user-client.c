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

#include "user-client.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(IOReturn) == sizeof(uint32_t),
               "Inferno Metal wire IOReturn must be 32 bits");

struct ImtlUserClient {
    io_connect_t connection;
    ImtlUserCaps caps;
};

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

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static bool knownState(uint32_t state)
{
    switch (state) {
    case INFERNO_METAL_USER_IDLE:
    case INFERNO_METAL_USER_SUBMITTED:
    case INFERNO_METAL_USER_COMPLETED:
    case INFERNO_METAL_USER_RESETTING:
    case INFERNO_METAL_USER_FAULTED:
    case INFERNO_METAL_USER_DRAINING:
    case INFERNO_METAL_USER_DRAINED:
    case INFERNO_METAL_USER_STOPPED:
        return true;
    default:
        return false;
    }
}

static bool knownResult(uint32_t result)
{
    switch (result) {
    case INFERNO_METAL_USER_RESULT_OK:
    case INFERNO_METAL_USER_RESULT_AGAIN:
    case INFERNO_METAL_USER_RESULT_BUSY:
    case INFERNO_METAL_USER_RESULT_BAD_ARGUMENT:
    case INFERNO_METAL_USER_RESULT_BAD_DEVICE:
    case INFERNO_METAL_USER_RESULT_SEQUENCE_MISMATCH:
        return true;
    default:
        return false;
    }
}

static uint32_t knownOpcodeMask(void)
{
    return (1U << INFERNO_METAL_COMPUTE) | (1U << INFERNO_METAL_RENDER) |
           (1U << INFERNO_METAL_CLEAR);
}

static IOReturn fetchCaps(io_connect_t connection, ImtlUserCaps *out)
{
    uint8_t reply[INFERNO_METAL_USER_CAPABILITIES_SIZE] = { 0 };
    size_t reply_size = sizeof(reply);
    IOReturn result =
        IOConnectCallStructMethod(connection, INFERNO_METAL_USER_CAPABILITIES,
                                  NULL, 0, reply, &reply_size);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (reply_size != sizeof(reply)) {
        return kIOReturnBadMessageID;
    }

    ImtlUserCaps caps = {
        .version = get32(reply + INFERNO_METAL_USER_CAP_VERSION_OFFSET),
        .opcode_mask = get32(reply + INFERNO_METAL_USER_CAP_OPCODES_OFFSET),
        .max_source_size = get32(reply + INFERNO_METAL_USER_CAP_SOURCE_OFFSET),
        .max_input_size = get32(reply + INFERNO_METAL_USER_CAP_INPUT_OFFSET),
        .max_output_size = get32(reply + INFERNO_METAL_USER_CAP_OUTPUT_OFFSET),
        .max_request_size =
            get32(reply + INFERNO_METAL_USER_CAP_REQUEST_OFFSET),
    };
    uint32_t known_opcodes = knownOpcodeMask();
    if (caps.version != INFERNO_METAL_USER_VERSION ||
        get32(reply + INFERNO_METAL_USER_CAP_SIZE_OFFSET) != sizeof(reply) ||
        !caps.opcode_mask || (caps.opcode_mask & ~known_opcodes) ||
        get32(reply + INFERNO_METAL_USER_CAP_FLAGS_OFFSET) != 0 ||
        !caps.max_source_size ||
        caps.max_source_size > INFERNO_METAL_MAX_SOURCE ||
        !caps.max_input_size ||
        caps.max_input_size > INFERNO_METAL_MAX_BUFFER ||
        !caps.max_output_size ||
        caps.max_output_size > INFERNO_METAL_MAX_BUFFER ||
        caps.max_request_size < INFERNO_METAL_USER_SUBMIT_HEADER_SIZE ||
        caps.max_request_size > INFERNO_METAL_USER_MAX_REQUEST) {
        return kIOReturnBadMessageID;
    }
    *out = caps;
    return kIOReturnSuccess;
}

IOReturn imtl_user_client_open(io_service_t service, ImtlUserClient **out)
{
    if (!out) {
        return kIOReturnBadArgument;
    }
    *out = NULL;
    if (service == MACH_PORT_NULL) {
        return kIOReturnBadArgument;
    }

    io_connect_t connection = MACH_PORT_NULL;
    IOReturn result =
        IOServiceOpen(service, mach_task_self(),
                      INFERNO_METAL_USER_CONNECTION_TYPE, &connection);
    if (result != kIOReturnSuccess) {
        return result;
    }

    ImtlUserCaps caps;
    result = fetchCaps(connection, &caps);
    if (result != kIOReturnSuccess) {
        IOServiceClose(connection);
        return result;
    }

    ImtlUserClient *client = calloc(1, sizeof(*client));
    if (!client) {
        IOServiceClose(connection);
        return kIOReturnNoMemory;
    }
    client->connection = connection;
    client->caps = caps;
    *out = client;
    return kIOReturnSuccess;
}

const ImtlUserCaps *imtl_user_client_caps(const ImtlUserClient *client)
{
    return client ? &client->caps : NULL;
}

static size_t nameSize(const char name[64])
{
    const char *end = memchr(name, '\0', 64);
    return end ? (size_t)(end - name) + 1 : 0;
}

IOReturn imtl_user_client_submit(ImtlUserClient *client,
                                 const ImtlUserSubmit *request)
{
    if (!client || !request) {
        return kIOReturnBadArgument;
    }
    if (request->opcode < INFERNO_METAL_COMPUTE ||
        request->opcode > INFERNO_METAL_CLEAR) {
        return kIOReturnBadArgument;
    }
    if (!(client->caps.opcode_mask & (1U << request->opcode)) ||
        request->source_size > client->caps.max_source_size ||
        request->input_size > client->caps.max_input_size ||
        !request->output_size ||
        request->output_size > client->caps.max_output_size ||
        (request->source_size && !request->source) ||
        (request->input_size && !request->input)) {
        return kIOReturnBadArgument;
    }
    size_t function_size = nameSize(request->function);
    size_t fragment_size = nameSize(request->fragment);
    if (!function_size || !fragment_size ||
        request->source_size >
            SIZE_MAX - INFERNO_METAL_USER_SUBMIT_HEADER_SIZE ||
        request->input_size > SIZE_MAX - INFERNO_METAL_USER_SUBMIT_HEADER_SIZE -
                                  request->source_size) {
        return kIOReturnBadArgument;
    }
    size_t packet_size = INFERNO_METAL_USER_SUBMIT_HEADER_SIZE +
                         request->source_size + request->input_size;
    if (packet_size > client->caps.max_request_size ||
        packet_size > INFERNO_METAL_USER_MAX_REQUEST) {
        return kIOReturnBadArgument;
    }

    uint8_t *packet = calloc(1, packet_size);
    if (!packet) {
        return kIOReturnNoMemory;
    }
    put32(packet + INFERNO_METAL_USER_VERSION_OFFSET,
          INFERNO_METAL_USER_VERSION);
    put32(packet + INFERNO_METAL_USER_OPCODE_OFFSET, request->opcode);
    put64(packet + INFERNO_METAL_USER_SEQUENCE_OFFSET, request->sequence);
    put32(packet + INFERNO_METAL_USER_SOURCE_SIZE_OFFSET,
          (uint32_t)request->source_size);
    put32(packet + INFERNO_METAL_USER_INPUT_SIZE_OFFSET,
          (uint32_t)request->input_size);
    put32(packet + INFERNO_METAL_USER_OUTPUT_SIZE_OFFSET, request->output_size);
    put32(packet + INFERNO_METAL_USER_WIDTH_OFFSET, request->width);
    put32(packet + INFERNO_METAL_USER_HEIGHT_OFFSET, request->height);
    put32(packet + INFERNO_METAL_USER_DEPTH_OFFSET, request->depth);
    memcpy(packet + INFERNO_METAL_USER_FUNCTION_OFFSET, request->function,
           function_size);
    memcpy(packet + INFERNO_METAL_USER_FRAGMENT_OFFSET, request->fragment,
           fragment_size);
    if (request->source_size) {
        memcpy(packet + INFERNO_METAL_USER_SUBMIT_HEADER_SIZE, request->source,
               request->source_size);
    }
    if (request->input_size) {
        memcpy(packet + INFERNO_METAL_USER_SUBMIT_HEADER_SIZE +
                   request->source_size,
               request->input, request->input_size);
    }

    size_t output_size = 0;
    IOReturn result =
        IOConnectCallStructMethod(client->connection, INFERNO_METAL_USER_SUBMIT,
                                  packet, packet_size, NULL, &output_size);
    free(packet);
    return result;
}

IOReturn imtl_user_client_status(ImtlUserClient *client, ImtlUserStatus *out)
{
    if (!client || !out) {
        return kIOReturnBadArgument;
    }
    uint8_t reply[INFERNO_METAL_USER_STATUS_SIZE] = { 0 };
    size_t reply_size = sizeof(reply);
    IOReturn result =
        IOConnectCallStructMethod(client->connection, INFERNO_METAL_USER_STATUS,
                                  NULL, 0, reply, &reply_size);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (reply_size != sizeof(reply)) {
        return kIOReturnBadMessageID;
    }

    ImtlUserStatus status = {
        .version = get32(reply + INFERNO_METAL_USER_STATUS_VERSION_OFFSET),
        .state = get32(reply + INFERNO_METAL_USER_STATUS_STATE_OFFSET),
        .transport_result =
            get32(reply + INFERNO_METAL_USER_STATUS_TRANSPORT_OFFSET),
        .sequence = get64(reply + INFERNO_METAL_USER_STATUS_SEQUENCE_OFFSET),
        .completion_error =
            get32(reply + INFERNO_METAL_USER_STATUS_ERROR_OFFSET),
    };
    uint32_t timer = get32(reply + INFERNO_METAL_USER_STATUS_TIMER_OFFSET);
    memcpy(&status.timer_error, &timer, sizeof(timer));
    if (status.version != INFERNO_METAL_USER_VERSION ||
        !knownState(status.state) || !knownResult(status.transport_result) ||
        get32(reply + INFERNO_METAL_USER_STATUS_RESERVED_OFFSET) != 0) {
        return kIOReturnBadMessageID;
    }
    *out = status;
    return kIOReturnSuccess;
}

IOReturn imtl_user_client_read(ImtlUserClient *client, uint64_t offset,
                               void *dst, size_t capacity, size_t *out_bytes)
{
    if (!out_bytes) {
        return kIOReturnBadArgument;
    }
    *out_bytes = 0;
    if (!client || !dst || !capacity ||
        capacity > client->caps.max_output_size) {
        return kIOReturnBadArgument;
    }
    uint64_t capacity64 = (uint64_t)capacity;
    if ((size_t)capacity64 != capacity || offset > UINT64_MAX - capacity64) {
        return kIOReturnBadArgument;
    }

    uint64_t scalar_input[] = { offset };
    size_t output_size = capacity;
    IOReturn result = IOConnectCallMethod(
        client->connection, INFERNO_METAL_USER_READ, scalar_input, 1, NULL, 0,
        NULL, NULL, dst, &output_size);
    if (result != kIOReturnSuccess) {
        return result;
    }
    if (output_size != capacity) {
        return kIOReturnBadMessageID;
    }
    *out_bytes = capacity;
    return kIOReturnSuccess;
}

IOReturn imtl_user_client_ack(ImtlUserClient *client)
{
    if (!client) {
        return kIOReturnBadArgument;
    }
    return IOConnectCallScalarMethod(client->connection, INFERNO_METAL_USER_ACK,
                                     NULL, 0, NULL, NULL);
}

IOReturn imtl_user_client_reset(ImtlUserClient *client)
{
    if (!client) {
        return kIOReturnBadArgument;
    }
    return IOConnectCallScalarMethod(
        client->connection, INFERNO_METAL_USER_RESET, NULL, 0, NULL, NULL);
}

IOReturn imtl_user_client_close(ImtlUserClient **client_ptr)
{
    if (!client_ptr) {
        return kIOReturnBadArgument;
    }
    ImtlUserClient *client = *client_ptr;
    if (!client) {
        return kIOReturnSuccess;
    }
    *client_ptr = NULL;
    io_connect_t connection = client->connection;
    client->connection = MACH_PORT_NULL;
    IOReturn result = IOServiceClose(connection);
    free(client);
    return result;
}
