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

#include "iokit-transport.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/libkern.h>

struct ImtlIokitBuffer {
    IOBufferMemoryDescriptor *memory;
    uint8_t *bytes;
    uint64_t gpa;
    uint32_t size;
};

struct ImtlIokitTransport {
    IOMemoryMap *register_map;
    ImtlTransport core;
    ImtlIokitBuffer descriptor;
    ImtlIokitBuffer source;
    ImtlIokitBuffer input;
    ImtlIokitBuffer output;
    bool closing;
};

static void release_buffer(ImtlIokitBuffer *buffer)
{
    if (buffer->memory) {
        buffer->memory->release();
    }
    *buffer = {};
}

static void release_request(ImtlIokitTransport *t)
{
    release_buffer(&t->output);
    release_buffer(&t->input);
    release_buffer(&t->source);
}

static IOReturn allocate_buffer(ImtlIokitBuffer *buffer, uint32_t size)
{
    IOByteCount length = 0;

    if (!size) {
        return kIOReturnSuccess;
    }
    /* Nonpageable IOBufferMemoryDescriptor owns automatic preparation and
     * completion. Do not add an unmatched explicit complete() on release.
     * MapperNone is required: a contiguous IOVA is not a protocol GPA.
     */
    auto memory = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        size, 16384);
#if defined(IOKIT_ENABLE_SHARED_PTR)
    buffer->memory = memory.detach();
#else
    buffer->memory = memory;
#endif
    if (!buffer->memory) {
        return kIOReturnNoMemory;
    }
    buffer->bytes = (uint8_t *)buffer->memory->getBytesNoCopy();
    buffer->gpa =
        buffer->memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    if (!buffer->bytes || !buffer->gpa || length < size ||
        buffer->gpa > UINT64_MAX - size) {
        release_buffer(buffer);
        return kIOReturnIOError;
    }
    buffer->size = size;
    return kIOReturnSuccess;
}

static IOReturn transport_error(ImtlResult result)
{
    switch (result) {
    case IMTL_OK:
        return kIOReturnSuccess;
    case IMTL_BUSY:
        return kIOReturnBusy;
    case IMTL_AGAIN:
        return kIOReturnNotReady;
    case IMTL_BAD_ARGUMENT:
        return kIOReturnBadArgument;
    default:
        return kIOReturnIOError;
    }
}

IOReturn imtl_iokit_create(IOService *provider, ImtlIokitTransport **result)
{
    IOReturn error;

    if (!result) {
        return kIOReturnBadArgument;
    }
    *result = nullptr;
    if (!provider || !provider->getDeviceMemoryCount()) {
        return kIOReturnBadArgument;
    }
    ImtlIokitTransport *t = IONewZero(ImtlIokitTransport, 1);
    if (!t) {
        return kIOReturnNoMemory;
    }
    t->register_map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
    if (!t->register_map) {
        error = kIOReturnNoMemory;
        goto fail;
    }
    error = allocate_buffer(&t->descriptor, 16384);
    if (error != kIOReturnSuccess) {
        goto fail;
    }
    error = transport_error(
        imtl_open(&t->core, (volatile void *)t->register_map->getAddress(),
                  t->register_map->getLength()));
    if (error != kIOReturnSuccess) {
        goto fail;
    }
    *result = t;
    return kIOReturnSuccess;

fail:
    /* No request was submitted, so no DMA retirement is needed here. */
    release_buffer(&t->descriptor);
    if (t->register_map) {
        t->register_map->release();
    }
    IODelete(t, ImtlIokitTransport, 1);
    return error;
}

IOReturn imtl_iokit_submit(ImtlIokitTransport *t,
                           const InfernoMetalCommand *command,
                           const void *source, const void *input,
                           bool interrupt)
{
    IOReturn error;

    if (!t || !command || t->closing) {
        return kIOReturnBadArgument;
    }
    if (t->core.phase != IMTL_IDLE) {
        return kIOReturnBusy;
    }
    InfernoMetalCommand c = *command;
    if (c.source_size > INFERNO_METAL_MAX_SOURCE ||
        c.input_size > INFERNO_METAL_MAX_BUFFER || !c.output_size ||
        c.output_size > INFERNO_METAL_MAX_BUFFER ||
        (c.source_size && !source) || (c.input_size && !input)) {
        return kIOReturnBadArgument;
    }
    error = allocate_buffer(&t->source, c.source_size);
    if (error != kIOReturnSuccess) {
        goto fail;
    }
    error = allocate_buffer(&t->input, c.input_size);
    if (error != kIOReturnSuccess) {
        goto fail;
    }
    error = allocate_buffer(&t->output, c.output_size);
    if (error != kIOReturnSuccess) {
        goto fail;
    }
    if (c.source_size) {
        memcpy(t->source.bytes, source, c.source_size);
    }
    if (c.input_size) {
        memcpy(t->input.bytes, input, c.input_size);
    }
    memset(t->output.bytes, 0, c.output_size);
    c.source_gpa = t->source.gpa;
    c.input_gpa = t->input.gpa;
    c.output_gpa = t->output.gpa;
    imtl_encode(t->descriptor.bytes, &c);
    error = transport_error(
        imtl_submit(&t->core, t->descriptor.gpa, c.sequence, interrupt));
    if (error == kIOReturnSuccess) {
        return error;
    }
fail:
    /* All failures above precede the doorbell; allocations are still ours. */
    release_request(t);
    return error;
}

ImtlResult imtl_iokit_poll(ImtlIokitTransport *t, ImtlCompletion *completion)
{
    if (!t) {
        return IMTL_BAD_ARGUMENT;
    }
    ImtlResult result = imtl_poll(&t->core, completion);
    if (result == IMTL_OK && t->core.phase == IMTL_IDLE) {
        /* A reset drain retired. No output or completion may be published. */
        release_request(t);
    }
    return result;
}

ImtlResult imtl_iokit_progress(ImtlIokitTransport *t, uint32_t *flags)
{
    return t ? imtl_progress(&t->core, flags) : IMTL_BAD_ARGUMENT;
}

ImtlResult imtl_iokit_read_output(ImtlIokitTransport *t, size_t offset,
                                  void *output, size_t size)
{
    ImtlCompletion completion;

    if (!t || t->core.phase != IMTL_COMPLETED || !output ||
        offset > t->output.size || size > t->output.size - offset) {
        return IMTL_BAD_ARGUMENT;
    }
    ImtlResult result = imtl_poll(&t->core, &completion);
    if (result != IMTL_OK) {
        return result;
    }
    if (completion.error != INFERNO_METAL_OK) {
        return IMTL_BAD_DEVICE;
    }
    memcpy(output, t->output.bytes + offset, size);
    return IMTL_OK;
}

ImtlResult imtl_iokit_ack(ImtlIokitTransport *t)
{
    if (!t) {
        return IMTL_BAD_ARGUMENT;
    }
    ImtlResult result = imtl_ack(&t->core);
    if (result == IMTL_OK) {
        release_request(t);
    }
    return result;
}

ImtlResult imtl_iokit_reset(ImtlIokitTransport *t)
{
    if (!t) {
        return IMTL_BAD_ARGUMENT;
    }
    ImtlResult result = imtl_reset(&t->core);
    if (result == IMTL_OK) {
        release_request(t);
    }
    return result;
}

ImtlResult imtl_iokit_destroy(ImtlIokitTransport **owner)
{
    if (!owner || !*owner) {
        return IMTL_BAD_ARGUMENT;
    }
    ImtlIokitTransport *t = *owner;
    t->closing = true;
    if (t->core.phase != IMTL_IDLE) {
        ImtlResult result = imtl_iokit_reset(t);
        if (result != IMTL_OK) {
            return result;
        }
    }
    ImtlResult result = imtl_close(&t->core);
    if (result != IMTL_OK) {
        return result;
    }
    release_request(t);
    release_buffer(&t->descriptor);
    t->register_map->release();
    IODelete(t, ImtlIokitTransport, 1);
    *owner = nullptr;
    return IMTL_OK;
}
