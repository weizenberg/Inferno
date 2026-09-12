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

#ifndef INFERNO_METAL_BATCH_CLIENT_H
#define INFERNO_METAL_BATCH_CLIENT_H

#include "user-client.h"

#ifdef __cplusplus
extern "C" {
#endif

IOReturn imtl_batch_submit(ImtlUserClient *client, uint64_t sequence,
                           const void *manifest, size_t manifest_size,
                           uint32_t images_size);

#ifdef __cplusplus
}
#endif
#endif
