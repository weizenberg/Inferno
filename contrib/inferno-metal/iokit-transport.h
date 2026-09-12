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

#ifndef INFERNO_METAL_IOKIT_TRANSPORT_H
#define INFERNO_METAL_IOKIT_TRANSPORT_H

#include "transport.h"
#include <IOKit/IOReturn.h>

class IOService;
struct ImtlIokitTransport;

/* Kernel-only API. Caller owns the provider connection and serializes every
 * operation on its workloop. Calls may allocate or free memory and must not
 * run in a hardware interrupt or while holding a simple lock. No user pointers
 * or user-selected physical addresses are accepted. Open refuses another
 * owner's busy/pending device; it does not take over.
 */
IOReturn imtl_iokit_create(IOService *provider, ImtlIokitTransport **result);
/* Copies command/source/input before ringing the doorbell. The three GPA fields
 * in command are ignored and replaced by this owner's physical buffer ranges.
 * Caller buffers need remain alive only through this function. Descriptor and
 * source must be distinct from the opaque owner's private allocations.
 */
IOReturn imtl_iokit_submit(ImtlIokitTransport *t,
                           const InfernoMetalCommand *command,
                           const void *source, const void *input,
                           bool interrupt);
ImtlResult imtl_iokit_poll(ImtlIokitTransport *t, ImtlCompletion *completion);
ImtlResult imtl_iokit_progress(ImtlIokitTransport *t, uint32_t *flags);
/* Copy from successful completed output only. On failure nothing is copied.
 * Use a kernel destination; an OS user client must implement its own copyout.
 */
ImtlResult imtl_iokit_read_output(ImtlIokitTransport *t, size_t offset,
                                  void *output, size_t size);
ImtlResult imtl_iokit_ack(ImtlIokitTransport *t);
ImtlResult imtl_iokit_reset(ImtlIokitTransport *t);
/* Destruction stops new submissions and resets any unacknowledged work.
 * AGAIN retains *t and all resources; schedule a retry after retirement.
 * Only OK clears *t. A timeout never permits freeing the object or buffers.
 */
ImtlResult imtl_iokit_destroy(ImtlIokitTransport **t);

#endif
