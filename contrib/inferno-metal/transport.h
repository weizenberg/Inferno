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

#ifndef INFERNO_METAL_GUEST_TRANSPORT_H
#define INFERNO_METAL_GUEST_TRANSPORT_H

#include "standard-headers/inferno/metal.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ImtlResult {
    IMTL_OK,
    IMTL_AGAIN,
    IMTL_BUSY,
    IMTL_BAD_ARGUMENT,
    IMTL_BAD_DEVICE,
    IMTL_SEQUENCE_MISMATCH,
} ImtlResult;

typedef enum ImtlPhase {
    IMTL_CLOSED,
    IMTL_IDLE,
    IMTL_SUBMITTED,
    IMTL_COMPLETED,
    IMTL_DRAINING,
} ImtlPhase;

typedef struct ImtlTransport {
    volatile uint32_t *registers;
    uint64_t sequence;
    ImtlPhase phase;
} ImtlTransport;

typedef struct ImtlCompletion {
    uint64_t sequence;
    uint32_t error;
} ImtlCompletion;

/* Single owner: serialize all calls, including interrupt callbacks. Registers
 * must be a device-memory mapping of at least INFERNO_METAL_REGISTER_SIZE.
 * Initialize the object to zero before opening. Open requires an idle device;
 * it does not reset or take over work from an unknown former owner. A failed
 * open leaves the transport CLOSED. No function waits or allocates.
 */
ImtlResult imtl_open(ImtlTransport *t, volatile void *registers, size_t size);
/* Raw descriptor and command must not overlap. Encoding preserves all field
 * bytes, including malformed names; the host remains the protocol validator.
 */
void imtl_encode(uint8_t descriptor[INFERNO_METAL_DESCRIPTOR_SIZE],
                 const InfernoMetalCommand *command);
/* Descriptor and all referenced ranges must be prepared, contiguous guest
 * physical RAM. Keep them alive and immutable until acknowledgement or until
 * imtl_poll reports IMTL_OK for a reset drain. No IOVA translation is provided.
 */
ImtlResult imtl_submit(ImtlTransport *t, uint64_t descriptor_gpa,
                       uint64_t sequence, bool interrupt);
/* On completion, output DMA is visible and completion is filled. The caller
 * may consume output before acknowledging. On a completed reset drain, return
 * IMTL_OK with phase IDLE; completion is untouched. Device errors are returned
 * in completion.error, distinct from transport errors. Sequence mismatch keeps
 * ownership and requires a reset drain before releasing the buffers.
 */
ImtlResult imtl_poll(ImtlTransport *t, ImtlCompletion *completion);
/* Reads live batch progress without changing ownership or completion state. */
ImtlResult imtl_progress(ImtlTransport *t, uint32_t *flags);
ImtlResult imtl_ack(ImtlTransport *t);
ImtlResult imtl_reset(ImtlTransport *t);
/* Close succeeds only after all work is acknowledged or reset has drained. */
ImtlResult imtl_close(ImtlTransport *t);

#ifdef __cplusplus
}
#endif
#endif
