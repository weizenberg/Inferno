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

#include "InfernoMetalUserClient.h"
#include "InfernoMetalService.h"
#include "user-request.h"
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(InfernoMetalUserClient, IOUserClient);

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

static bool sizeFrom64(uint64_t value, size_t *result)
{
    size_t converted = (size_t)value;
    if ((uint64_t)converted != value) {
        return false;
    }
    *result = converted;
    return true;
}

static bool wireState(InfernoMetalService::State state, uint32_t *wire)
{
    switch (state) {
    case InfernoMetalService::Idle:
        *wire = INFERNO_METAL_USER_IDLE;
        return true;
    case InfernoMetalService::Submitted:
        *wire = INFERNO_METAL_USER_SUBMITTED;
        return true;
    case InfernoMetalService::Completed:
        *wire = INFERNO_METAL_USER_COMPLETED;
        return true;
    case InfernoMetalService::Resetting:
        *wire = INFERNO_METAL_USER_RESETTING;
        return true;
    case InfernoMetalService::Faulted:
        *wire = INFERNO_METAL_USER_FAULTED;
        return true;
    case InfernoMetalService::Draining:
        *wire = INFERNO_METAL_USER_DRAINING;
        return true;
    case InfernoMetalService::Drained:
        *wire = INFERNO_METAL_USER_DRAINED;
        return true;
    case InfernoMetalService::Stopped:
        *wire = INFERNO_METAL_USER_STOPPED;
        return true;
    }
    return false;
}

static bool wireResult(ImtlResult result, uint32_t *wire)
{
    switch (result) {
    case IMTL_OK:
        *wire = INFERNO_METAL_USER_RESULT_OK;
        return true;
    case IMTL_AGAIN:
        *wire = INFERNO_METAL_USER_RESULT_AGAIN;
        return true;
    case IMTL_BUSY:
        *wire = INFERNO_METAL_USER_RESULT_BUSY;
        return true;
    case IMTL_BAD_ARGUMENT:
        *wire = INFERNO_METAL_USER_RESULT_BAD_ARGUMENT;
        return true;
    case IMTL_BAD_DEVICE:
        *wire = INFERNO_METAL_USER_RESULT_BAD_DEVICE;
        return true;
    case IMTL_SEQUENCE_MISMATCH:
        *wire = INFERNO_METAL_USER_RESULT_SEQUENCE_MISMATCH;
        return true;
    }
    return false;
}

static IOReturn readDescriptor(IOMemoryDescriptor *descriptor, void *bytes,
                               size_t size)
{
    IOReturn result = descriptor->prepare(kIODirectionNone);
    if (result != kIOReturnSuccess) {
        return result;
    }
    IOReturn copy_result = descriptor->readBytes(0, bytes, size) == size ?
                               kIOReturnSuccess :
                               kIOReturnIOError;
    IOReturn complete_result = descriptor->complete(kIODirectionNone);
    return copy_result != kIOReturnSuccess ? copy_result : complete_result;
}

static IOReturn writeDescriptor(IOMemoryDescriptor *descriptor,
                                const void *bytes, size_t size)
{
    IOReturn result = descriptor->prepare(kIODirectionNone);
    if (result != kIOReturnSuccess) {
        return result;
    }
    IOReturn copy_result = descriptor->writeBytes(0, bytes, size) == size ?
                               kIOReturnSuccess :
                               kIOReturnIOError;
    IOReturn complete_result = descriptor->complete(kIODirectionNone);
    return copy_result != kIOReturnSuccess ? copy_result : complete_result;
}

bool InfernoMetalUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) {
        return false;
    }
    auto *service = OSDynamicCast(InfernoMetalService, provider);
    if (!service ||
        !setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue) ||
        !setProperty(kIOUserClientDefaultLockingSetPropertiesKey,
                     kOSBooleanTrue) ||
        !setProperty(kIOUserClientDefaultLockingSingleThreadExternalMethodKey,
                     kOSBooleanTrue)) {
        IOUserClient::stop(provider);
        return false;
    }

    service->retain();
    service_ = service;
    loop_ = service_->getWorkLoop();
    if (!loop_) {
        goto fail;
    }
    loop_->retain();
    if (service_->newSession(&session_) != kIOReturnSuccess || !session_) {
        goto fail;
    }
    return true;

fail:
    if (session_) {
        session_->release();
        session_ = nullptr;
    }
    if (loop_) {
        loop_->release();
        loop_ = nullptr;
    }
    service_->release();
    service_ = nullptr;
    IOUserClient::stop(provider);
    return false;
}

IOReturn InfernoMetalUserClient::closeGated()
{
    closing_ = true;
    if (!service_ || !session_) {
        return kIOReturnSuccess;
    }
    return service_->closeSession(session_);
}

IOReturn InfernoMetalUserClient::closeAction(OSObject *object, void *, void *,
                                             void *, void *)
{
    return static_cast<InfernoMetalUserClient *>(object)->closeGated();
}

IOReturn InfernoMetalUserClient::clientClose()
{
    if (loop_) {
        loop_->runAction(closeAction, this);
    }
    IOUserClient::terminate();
    return kIOReturnSuccess;
}

void InfernoMetalUserClient::stop(IOService *provider)
{
    if (loop_) {
        loop_->runAction(closeAction, this);
    }
    IOUserClient::stop(provider);
}

void InfernoMetalUserClient::free()
{
    if (session_) {
        session_->release();
        session_ = nullptr;
    }
    if (loop_) {
        loop_->release();
        loop_ = nullptr;
    }
    if (service_) {
        service_->release();
        service_ = nullptr;
    }
    IOUserClient::free();
}

static bool noInputStructure(const IOExternalMethodArguments *arguments)
{
    return !arguments->structureInputDescriptor &&
           arguments->structureInputSize == 0;
}

static bool noOutputStructure(const IOExternalMethodArguments *arguments,
                              uint32_t inline_capacity,
                              uint32_t descriptor_capacity)
{
    return !arguments->structureOutputDescriptor && inline_capacity == 0 &&
           descriptor_capacity == 0;
}

static bool validCommonArguments(const IOExternalMethodArguments *arguments)
{
    return arguments->asyncWakePort == MACH_PORT_NULL &&
           !arguments->asyncReference && arguments->asyncReferenceCount == 0 &&
           !arguments->structureVariableOutputData;
}

static bool outputCapacity(IOExternalMethodArguments *arguments,
                           uint32_t inline_capacity,
                           uint32_t descriptor_capacity, size_t minimum,
                           size_t maximum, size_t *capacity)
{
    if (arguments->structureOutputDescriptor) {
        if (inline_capacity != 0 || descriptor_capacity < minimum ||
            descriptor_capacity > maximum ||
            descriptor_capacity >
                arguments->structureOutputDescriptor->getLength()) {
            return false;
        }
        *capacity = descriptor_capacity;
        return true;
    }
    if (descriptor_capacity != 0 || inline_capacity < minimum ||
        inline_capacity > maximum ||
        inline_capacity > INFERNO_METAL_USER_INLINE_MAX ||
        (inline_capacity && !arguments->structureOutput)) {
        return false;
    }
    *capacity = inline_capacity;
    return true;
}

static IOReturn copyOutput(IOExternalMethodArguments *arguments,
                           const void *bytes, size_t size)
{
    IOReturn result;
    if (arguments->structureOutputDescriptor) {
        result =
            writeDescriptor(arguments->structureOutputDescriptor, bytes, size);
        if (result == kIOReturnSuccess) {
            arguments->structureOutputDescriptorSize = (uint32_t)size;
        }
    } else {
        memcpy(arguments->structureOutput, bytes, size);
        arguments->structureOutputSize = (uint32_t)size;
        result = kIOReturnSuccess;
    }
    return result;
}

IOReturn InfernoMetalUserClient::dispatchMethod(MethodRequest *request)
{
    IOExternalMethodArguments *arguments = request->arguments;
    request->scalar_output_capacity = arguments->scalarOutputCount;
    request->inline_output_capacity = arguments->structureOutputSize;
    request->descriptor_output_capacity =
        arguments->structureOutputDescriptorSize;
    arguments->scalarOutputCount = 0;
    arguments->structureOutputSize = 0;
    arguments->structureOutputDescriptorSize = 0;
    if (closing_ || isInactive() || !service_ || !session_) {
        return kIOReturnNotReady;
    }
    if (!validCommonArguments(arguments)) {
        return kIOReturnBadArgument;
    }

    switch (request->selector) {
    case INFERNO_METAL_USER_CAPABILITIES: {
        size_t capacity;
        if (arguments->scalarInputCount != 0 ||
            request->scalar_output_capacity != 0 ||
            !noInputStructure(arguments) ||
            !outputCapacity(arguments, request->inline_output_capacity,
                            request->descriptor_output_capacity,
                            INFERNO_METAL_USER_CAPABILITIES_SIZE,
                            INFERNO_METAL_USER_CAPABILITIES_SIZE, &capacity)) {
            return kIOReturnBadArgument;
        }
        uint8_t output[INFERNO_METAL_USER_CAPABILITIES_SIZE] = {};
        put32(output + INFERNO_METAL_USER_CAP_VERSION_OFFSET,
              INFERNO_METAL_USER_VERSION);
        put32(output + INFERNO_METAL_USER_CAP_SIZE_OFFSET,
              INFERNO_METAL_USER_CAPABILITIES_SIZE);
        put32(output + INFERNO_METAL_USER_CAP_OPCODES_OFFSET,
              (1U << INFERNO_METAL_COMPUTE) | (1U << INFERNO_METAL_RENDER) |
                  (1U << INFERNO_METAL_CLEAR) |
                  (1U << INFERNO_METAL_QUERY_LIBRARY) |
                  (1U << INFERNO_METAL_QUERY_PIPELINE) |
                  (1U << INFERNO_METAL_QUERY_IMAGEBLOCK));
        put32(output + INFERNO_METAL_USER_CAP_SOURCE_OFFSET,
              INFERNO_METAL_MAX_SOURCE);
        put32(output + INFERNO_METAL_USER_CAP_INPUT_OFFSET,
              INFERNO_METAL_MAX_BUFFER);
        put32(output + INFERNO_METAL_USER_CAP_OUTPUT_OFFSET,
              INFERNO_METAL_MAX_BUFFER);
        put32(output + INFERNO_METAL_USER_CAP_REQUEST_OFFSET,
              INFERNO_METAL_USER_MAX_REQUEST);
        return copyOutput(arguments, output, capacity);
    }
    case INFERNO_METAL_USER_SUBMIT: {
        if (arguments->scalarInputCount != 0 ||
            request->scalar_output_capacity != 0 ||
            !noOutputStructure(arguments, request->inline_output_capacity,
                               request->descriptor_output_capacity)) {
            return kIOReturnBadArgument;
        }
        size_t size;
        bool descriptor = arguments->structureInputDescriptor != nullptr;
        if (descriptor) {
            if (arguments->structureInputSize != 0 ||
                !sizeFrom64(arguments->structureInputDescriptor->getLength(),
                            &size)) {
                return kIOReturnBadArgument;
            }
        } else {
            size = arguments->structureInputSize;
            if (size > INFERNO_METAL_USER_INLINE_MAX ||
                (size && !arguments->structureInput)) {
                return kIOReturnBadArgument;
            }
        }
        if (size < INFERNO_METAL_USER_SUBMIT_HEADER_SIZE ||
            size > INFERNO_METAL_USER_MAX_REQUEST) {
            return kIOReturnBadArgument;
        }
        uint8_t *packet = IONew(uint8_t, size);
        if (!packet) {
            return kIOReturnNoMemory;
        }
        IOReturn result;
        if (descriptor) {
            result = readDescriptor(arguments->structureInputDescriptor, packet,
                                    size);
        } else {
            memcpy(packet, arguments->structureInput, size);
            result = kIOReturnSuccess;
        }
        ImtlUserRequest decoded;
        if (result == kIOReturnSuccess &&
            !imtl_user_decode(packet, size, &decoded)) {
            result = kIOReturnBadArgument;
        }
        if (result == kIOReturnSuccess) {
            result = service_->submitSession(session_, &decoded.command,
                                             decoded.source, decoded.input);
        }
        IODelete(packet, uint8_t, size);
        return result;
    }
    case INFERNO_METAL_USER_STATUS: {
        size_t capacity;
        if (arguments->scalarInputCount != 0 ||
            request->scalar_output_capacity != 0 ||
            !noInputStructure(arguments) ||
            !outputCapacity(arguments, request->inline_output_capacity,
                            request->descriptor_output_capacity,
                            INFERNO_METAL_USER_STATUS_SIZE,
                            INFERNO_METAL_USER_STATUS_SIZE, &capacity)) {
            return kIOReturnBadArgument;
        }
        InfernoMetalService::Status status;
        IOReturn result = service_->statusSession(session_, &status);
        if (result != kIOReturnSuccess) {
            return result;
        }
        uint32_t state;
        uint32_t transport;
        if (!wireState(status.state, &state) ||
            !wireResult(status.transport_result, &transport)) {
            return kIOReturnIOError;
        }
        uint8_t output[INFERNO_METAL_USER_STATUS_SIZE] = {};
        put32(output + INFERNO_METAL_USER_STATUS_VERSION_OFFSET,
              INFERNO_METAL_USER_VERSION);
        put32(output + INFERNO_METAL_USER_STATUS_STATE_OFFSET, state);
        put32(output + INFERNO_METAL_USER_STATUS_TRANSPORT_OFFSET, transport);
        put32(output + INFERNO_METAL_USER_STATUS_TIMER_OFFSET,
              (uint32_t)status.timer_error);
        put64(output + INFERNO_METAL_USER_STATUS_SEQUENCE_OFFSET,
              status.completion.sequence);
        put32(output + INFERNO_METAL_USER_STATUS_ERROR_OFFSET,
              status.completion.error);
        return copyOutput(arguments, output, capacity);
    }
    case INFERNO_METAL_USER_READ: {
        size_t capacity;
        size_t offset;
        if (arguments->scalarInputCount != 1 || !arguments->scalarInput ||
            request->scalar_output_capacity != 0 ||
            !noInputStructure(arguments) ||
            !outputCapacity(arguments, request->inline_output_capacity,
                            request->descriptor_output_capacity, 1,
                            INFERNO_METAL_MAX_BUFFER, &capacity)) {
            return kIOReturnBadArgument;
        }
        if (!sizeFrom64(arguments->scalarInput[0], &offset) ||
            capacity > SIZE_MAX - offset) {
            return kIOReturnBadArgument;
        }
        uint8_t *output = IONew(uint8_t, capacity);
        if (!output) {
            return kIOReturnNoMemory;
        }
        IOReturn result =
            service_->readSession(session_, offset, output, capacity);
        if (result == kIOReturnSuccess) {
            result = copyOutput(arguments, output, capacity);
        }
        IODelete(output, uint8_t, capacity);
        return result;
    }
    case INFERNO_METAL_USER_ACK:
        if (arguments->scalarInputCount != 0 ||
            request->scalar_output_capacity != 0 ||
            !noInputStructure(arguments) ||
            !noOutputStructure(arguments, request->inline_output_capacity,
                               request->descriptor_output_capacity)) {
            return kIOReturnBadArgument;
        }
        return service_->acknowledgeSession(session_);
    case INFERNO_METAL_USER_RESET:
        if (arguments->scalarInputCount != 0 ||
            request->scalar_output_capacity != 0 ||
            !noInputStructure(arguments) ||
            !noOutputStructure(arguments, request->inline_output_capacity,
                               request->descriptor_output_capacity)) {
            return kIOReturnBadArgument;
        }
        return service_->resetSession(session_);
    default:
        return kIOReturnUnsupported;
    }
}

IOReturn InfernoMetalUserClient::methodAction(OSObject *object, void *request,
                                              void *, void *, void *)
{
    return static_cast<InfernoMetalUserClient *>(object)->dispatchMethod(
        static_cast<MethodRequest *>(request));
}

IOReturn InfernoMetalUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *, OSObject *, void *)
{
    if (!arguments ||
        arguments->version != kIOExternalMethodArgumentsCurrentVersion) {
        return kIOReturnBadArgument;
    }
    MethodRequest request = {
        selector, arguments, 0, 0, 0,
    };
    if (!loop_) {
        request.scalar_output_capacity = arguments->scalarOutputCount;
        request.inline_output_capacity = arguments->structureOutputSize;
        request.descriptor_output_capacity =
            arguments->structureOutputDescriptorSize;
        arguments->scalarOutputCount = 0;
        arguments->structureOutputSize = 0;
        arguments->structureOutputDescriptorSize = 0;
        return kIOReturnNotReady;
    }
    return loop_->runAction(methodAction, this, &request);
}
