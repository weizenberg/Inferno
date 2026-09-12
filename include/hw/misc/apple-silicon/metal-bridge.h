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

#ifndef HW_APPLE_METAL_BRIDGE_H
#define HW_APPLE_METAL_BRIDGE_H

#include "qapi/error.h"
#include "standard-headers/inferno/metal.h"

#define TYPE_INFERNO_METAL_BRIDGE "inferno-metal-bridge"
typedef struct InfernoMetalBackend InfernoMetalBackend;

InfernoMetalBackend *inferno_metal_backend_new(Error **errp);
void inferno_metal_backend_free(InfernoMetalBackend *backend);
/* Called off the BQL, with owned host buffers and no guest-memory access. */
bool inferno_metal_backend_execute(InfernoMetalBackend *backend,
                                   const InfernoMetalCommand *command,
                                   const uint8_t *source, const uint8_t *input,
                                   uint8_t *output, char *message,
                                   size_t message_size);

#endif
