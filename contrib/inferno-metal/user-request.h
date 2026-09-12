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

#ifndef INFERNO_METAL_USER_REQUEST_H
#define INFERNO_METAL_USER_REQUEST_H

#include "standard-headers/inferno/metal-user.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native parse result, not a wire structure. The command has zero GPA fields.
 * Source/input point into the immutable kernel-owned packet supplied below.
 */
typedef struct ImtlUserRequest {
    InfernoMetalCommand command;
    const void *source;
    const void *input;
} ImtlUserRequest;

/* Decode a fully copied kernel packet. Never pass a live user mapping here.
 * result must not overlap packet; failure leaves result unchanged. The owner
 * copies decoded source/input before returning from synchronous submission.
 */
bool imtl_user_decode(const void *packet, size_t size, ImtlUserRequest *result);

#ifdef __cplusplus
}
#endif
#endif
