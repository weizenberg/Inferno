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

#ifndef INFERNO_METAL_USER_CLIENT_API_H
#define INFERNO_METAL_USER_CLIENT_API_H

#include "standard-headers/inferno/metal-user.h"
#include <IOKit/IOKitLib.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImtlUserClient ImtlUserClient;

/* Host-native negotiated values, not a wire structure. */
typedef struct ImtlUserCaps {
    uint32_t version;
    uint32_t opcode_mask;
    uint32_t max_source_size;
    uint32_t max_input_size;
    uint32_t max_output_size;
    uint32_t max_request_size;
} ImtlUserCaps;

/* Host-native submission. Names must contain a NUL within their fixed arrays.
 * Source and input are borrowed only for the synchronous submit call.
 */
typedef struct ImtlUserSubmit {
    uint32_t opcode;
    uint64_t sequence;
    uint32_t output_size;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t options;
    char function[64];
    char fragment[64];
    const void *source;
    size_t source_size;
    const void *input;
    size_t input_size;
} ImtlUserSubmit;

/* Host-native status, not a wire structure. timer_error preserves the exact
 * 32-bit IOReturn pattern received from the kernel.
 */
typedef struct ImtlUserStatus {
    uint32_t version;
    uint32_t state;
    uint32_t transport_result;
    IOReturn timer_error;
    uint64_t sequence;
    uint32_t completion_error;
} ImtlUserStatus;

/* service is borrowed. On success the returned handle owns one type-0
 * connection. The caller must serialize every operation, caps access and
 * close on a handle; this layer intentionally contains no lock or GPU state.
 */
IOReturn imtl_user_client_open(io_service_t service, ImtlUserClient **out);
/* The returned view is borrowed and remains valid only until serialized close.
 */
const ImtlUserCaps *imtl_user_client_caps(const ImtlUserClient *client);
IOReturn imtl_user_client_submit(ImtlUserClient *client,
                                 const ImtlUserSubmit *request);
IOReturn imtl_user_client_status(ImtlUserClient *client, ImtlUserStatus *out);

/* On failure, *out_bytes is zero. dst may have been partially modified by
 * IOKit and must not be consumed unless this call succeeds.
 */
IOReturn imtl_user_client_read(ImtlUserClient *client, uint64_t offset,
                               void *dst, size_t capacity, size_t *out_bytes);
IOReturn imtl_user_client_ack(ImtlUserClient *client);
IOReturn imtl_user_client_reset(ImtlUserClient *client);

/* Invalidates and frees *client exactly once before returning, including when
 * IOServiceClose fails. A NULL *client is a successful no-op.
 */
IOReturn imtl_user_client_close(ImtlUserClient **client);

#ifdef __cplusplus
}
#endif
#endif
