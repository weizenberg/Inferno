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

#include "batch-client.h"

IOReturn imtl_batch_submit(ImtlUserClient *client, uint64_t sequence,
                           const void *manifest, size_t manifest_size,
                           uint32_t images_size)
{
    const ImtlUserCaps *caps = imtl_user_client_caps(client);
    if (!client || !manifest ||
        manifest_size < INFERNO_METAL_BATCH_HEADER_SIZE ||
        manifest_size > INFERNO_METAL_MAX_BUFFER ||
        images_size > INFERNO_METAL_BATCH_MAX_IMAGES) {
        return kIOReturnBadArgument;
    }
    uint32_t output_size = INFERNO_METAL_BATCH_RESULT_SIZE + images_size;
    if (!caps || !(caps->opcode_mask & (1U << INFERNO_METAL_BATCH)) ||
        manifest_size > caps->max_input_size ||
        output_size > caps->max_output_size ||
        manifest_size >
            caps->max_request_size - INFERNO_METAL_USER_SUBMIT_HEADER_SIZE) {
        return kIOReturnUnsupported;
    }
    ImtlUserSubmit request = {
        .opcode = INFERNO_METAL_BATCH,
        .sequence = sequence,
        .output_size = output_size,
        .width = 1,
        .height = 1,
        .depth = 1,
        .options = INFERNO_METAL_OPTIONS_DEFAULT,
        .input = manifest,
        .input_size = manifest_size,
    };
    return imtl_user_client_submit(client, &request);
}

IOReturn imtl_batch5_submit(ImtlUserClient *client, uint64_t sequence,
                            const void *manifest, size_t manifest_size,
                            uint32_t images_size)
{
    const ImtlUserCaps *caps = imtl_user_client_caps(client);
    if (!client || !manifest ||
        manifest_size < INFERNO_METAL_RESOURCE_HEADER_SIZE ||
        manifest_size > INFERNO_METAL_MAX_BUFFER ||
        images_size > INFERNO_METAL_RESOURCE_MAX_IMAGES) {
        return kIOReturnBadArgument;
    }
    uint32_t output_size = INFERNO_METAL_RESOURCE_RESULT_SIZE + images_size;
    if (!caps || !(caps->opcode_mask & (1U << INFERNO_METAL_BATCH_RESOURCES)) ||
        manifest_size > caps->max_input_size ||
        output_size > caps->max_output_size ||
        caps->max_request_size < INFERNO_METAL_USER_SUBMIT_HEADER_SIZE ||
        manifest_size >
            caps->max_request_size - INFERNO_METAL_USER_SUBMIT_HEADER_SIZE) {
        return kIOReturnUnsupported;
    }
    ImtlUserSubmit request = {
        .opcode = INFERNO_METAL_BATCH_RESOURCES,
        .sequence = sequence,
        .output_size = output_size,
        .width = 1,
        .height = 1,
        .depth = 1,
        .options = INFERNO_METAL_OPTIONS_DEFAULT,
        .input = manifest,
        .input_size = manifest_size,
    };
    return imtl_user_client_submit(client, &request);
}
