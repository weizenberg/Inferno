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

#import "InfernoMetalBuffer.h"
#import "InfernoMetalCommandBuffer.h"
#import "InfernoMetalCommandQueue.h"
#import "InfernoMetalComputeCommandEncoder.h"
#import "InfernoMetalComputePipelineState.h"
#import "InfernoMetalContextPrivate.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#import "InfernoMetalLibrary.h"

#include "../batch-wire.h"
#include <stdlib.h>
#include <string.h>

@interface InfernoMetalComputePipelineState (ExecutionAccess)
@property(nonatomic, readonly) InfernoMetalFunction *function;
@end

static void observeScheduled(void *opaque, uint64_t sequence)
{
    (void)sequence;
    [(__bridge InfernoMetalCommandBuffer *)opaque infernoObserveScheduled];
}

static NSUInteger identicalIndex(NSArray *array, id object)
{
    for (NSUInteger i = 0; i < array.count; i++) {
        if (array[i] == object)
            return i;
    }
    return NSNotFound;
}

static NSError *coordinatorBatchError(const ImtlCoordinatorError *error,
                                      BOOL scheduled)
{
    NSError *base = InfernoMetalErrorFromCoordinator(error);
    if (!scheduled)
        return base;
    NSMutableDictionary *info = [base.userInfo mutableCopy];
    info[InfernoMetalScheduledKey] = @YES;
    return [NSError errorWithDomain:base.domain code:base.code userInfo:info];
}

@implementation InfernoMetalCompilerContext (Execution)

- (BOOL)registerCommandQueue:(InfernoMetalCommandQueue *)queue
{
    [self.schedulerLock lock];
    BOOL accepted = !self.executionClosed;
    if (accepted)
        [self.queues addObject:queue];
    [self.schedulerLock unlock];
    return accepted;
}

- (uint64_t)commitCommandBuffer:(InfernoMetalCommandBuffer *)commandBuffer
                          queue:(InfernoMetalCommandQueue *)queue
{
    [self.schedulerLock lock];
    uint64_t result = 0;
    if (!self.executionClosed && self.nextCommitSerial != UINT64_MAX) {
        uint64_t serial = self.nextCommitSerial;
        self.nextCommitSerial = serial + 1;
        if ([commandBuffer infernoPublishCommitSerial:serial] &&
            [queue infernoCommitReservation:commandBuffer]) {
            result = serial;
        }
    }
    [self.schedulerLock unlock];
    return result;
}

- (void)scheduleExecutionPump
{
    dispatch_async(self.executionQueue, ^{
      [self runExecutionPump];
    });
}

- (void)runExecutionPump
{
    for (;;) {
        [self.schedulerLock lock];
        if (self.executionClosed) {
            [self.schedulerLock unlock];
            return;
        }
        InfernoMetalCommandBuffer *chosen = nil;
        InfernoMetalCommandQueue *chosenQueue = nil;
        for (InfernoMetalCommandQueue *queue in self.queues) {
            InfernoMetalCommandBuffer *candidate = [queue infernoReadyHead];
            if (candidate && (!chosen || candidate.infernoCommitSerial <
                                             chosen.infernoCommitSerial)) {
                chosen = candidate;
                chosenQueue = queue;
            }
        }
        if (!chosen || ![chosenQueue infernoAdmitHead:chosen]) {
            [self.schedulerLock unlock];
            return;
        }
        [chosen infernoSetAdmitted];
        [self.schedulerLock unlock];
        [self executeCommandBuffer:chosen];
        [chosenQueue infernoRemove:chosen];
    }
}

- (void)executeCommandBuffer:(InfernoMetalCommandBuffer *)commandBuffer
{
    NSArray<InfernoMetalEncodedDispatch *> *dispatchObjects =
        commandBuffer.infernoDispatches;
    NSMutableArray *pipelineObjects = [NSMutableArray array];
    NSMutableArray<InfernoMetalBuffer *> *bufferObjects =
        [NSMutableArray array];
    for (InfernoMetalEncodedDispatch *dispatch in dispatchObjects) {
        if (identicalIndex(pipelineObjects, dispatch.pipeline) == NSNotFound)
            [pipelineObjects addObject:dispatch.pipeline];
        for (id slot in dispatch.bindings) {
            if ([slot isKindOfClass:[NSDictionary class]] &&
                [slot[@"kind"] unsignedIntValue] ==
                    INFERNO_METAL_BATCH_BINDING_BUFFER) {
                InfernoMetalBuffer *buffer = slot[@"buffer"];
                if (identicalIndex(bufferObjects, buffer) == NSNotFound)
                    [bufferObjects addObject:buffer];
            }
        }
    }

    uint32_t pipelineCount = (uint32_t)pipelineObjects.count;
    uint32_t bufferCount = (uint32_t)bufferObjects.count;
    uint32_t dispatchCount = (uint32_t)dispatchObjects.count;
    uint32_t bindingCount = 0;
    for (InfernoMetalEncodedDispatch *dispatch in dispatchObjects) {
        for (NSUInteger i = 0; i < 31; i++) {
            if (dispatch.bindings[i] != [NSNull null])
                bindingCount++;
            if (dispatch.threadgroupLengths[i].unsignedIntegerValue)
                bindingCount++;
        }
    }
    if (pipelineCount > INFERNO_METAL_BATCH_MAX_PIPELINES ||
        bufferCount > INFERNO_METAL_BATCH_MAX_BUFFERS ||
        dispatchCount > INFERNO_METAL_BATCH_MAX_DISPATCHES ||
        bindingCount > INFERNO_METAL_BATCH_MAX_BINDINGS) {
        [commandBuffer
            infernoFail:InfernoMetalMakeError(
                            InfernoMetalErrorInvalidArgument,
                            @"The command buffer exceeds batch record limits")
              scheduled:NO];
        return;
    }

    ImtlBatchPipeline *pipelines =
        calloc(pipelineCount ? pipelineCount : 1, sizeof(*pipelines));
    ImtlBatchBuffer *buffers =
        calloc(bufferCount ? bufferCount : 1, sizeof(*buffers));
    ImtlBatchDispatch *dispatches =
        calloc(dispatchCount ? dispatchCount : 1, sizeof(*dispatches));
    ImtlBatchBinding *bindings =
        calloc(bindingCount ? bindingCount : 1, sizeof(*bindings));
    NSMutableArray<NSData *> *sources = [NSMutableArray array];
    NSMutableArray<NSData *> *images = [NSMutableArray array];
    if (!pipelines || !buffers || !dispatches || !bindings) {
        free(pipelines);
        free(buffers);
        free(dispatches);
        free(bindings);
        [commandBuffer
            infernoFail:InfernoMetalMakeError(
                            InfernoMetalErrorTransport,
                            @"Cannot allocate a Metal batch manifest")
              scheduled:NO];
        return;
    }

    for (uint32_t i = 0; i < pipelineCount; i++) {
        InfernoMetalComputePipelineState *pipeline = pipelineObjects[i];
        InfernoMetalFunction *function = pipeline.function;
        NSData *source = function.infernoLibrary.infernoSource;
        [sources addObject:source];
        pipelines[i] = (ImtlBatchPipeline){
            .source = source.bytes,
            .source_size = source.length,
            .function_name = function.name.UTF8String,
        };
    }
    for (uint32_t i = 0; i < bufferCount; i++) {
        InfernoMetalBuffer *buffer = bufferObjects[i];
        NSData *image =
            [NSData dataWithBytes:buffer.contents length:buffer.length];
        [images addObject:image];
        buffers[i] = (ImtlBatchBuffer){
            .bytes = image.bytes,
            .length = image.length,
        };
    }
    uint32_t nextBinding = 0;
    for (uint32_t i = 0; i < dispatchCount; i++) {
        InfernoMetalEncodedDispatch *dispatch = dispatchObjects[i];
        uint32_t start = nextBinding;
        for (uint32_t index = 0; index < 31; index++) {
            id slot = dispatch.bindings[index];
            if (slot != [NSNull null]) {
                uint32_t kind = [slot[@"kind"] unsignedIntValue];
                ImtlBatchBinding binding = {
                    .kind = kind,
                    .index = index,
                };
                if (kind == INFERNO_METAL_BATCH_BINDING_BUFFER) {
                    binding.resource_id = (uint32_t)identicalIndex(
                        bufferObjects, slot[@"buffer"]);
                    binding.offset = [slot[@"offset"] unsignedLongLongValue];
                } else {
                    NSData *data = slot[@"data"];
                    binding.bytes = data.bytes;
                    binding.length = data.length;
                }
                bindings[nextBinding++] = binding;
            }
            NSUInteger length =
                dispatch.threadgroupLengths[index].unsignedIntegerValue;
            if (length) {
                bindings[nextBinding++] = (ImtlBatchBinding){
                    .kind = INFERNO_METAL_BATCH_BINDING_THREADGROUP,
                    .index = index,
                    .length = length,
                };
            }
        }
        dispatches[i] = (ImtlBatchDispatch){
            .pipeline_id =
                (uint32_t)identicalIndex(pipelineObjects, dispatch.pipeline),
            .mode = dispatch.mode,
            .binding_start = start,
            .binding_count = nextBinding - start,
            .grid_width = (uint32_t)dispatch.grid.width,
            .grid_height = (uint32_t)dispatch.grid.height,
            .grid_depth = (uint32_t)dispatch.grid.depth,
            .group_width = (uint32_t)dispatch.group.width,
            .group_height = (uint32_t)dispatch.group.height,
            .group_depth = (uint32_t)dispatch.group.depth,
        };
    }
    ImtlBatchManifest specification = {
        .pipelines = pipelines,
        .pipeline_count = pipelineCount,
        .buffers = buffers,
        .buffer_count = bufferCount,
        .dispatches = dispatches,
        .dispatch_count = dispatchCount,
        .bindings = bindings,
        .binding_count = bindingCount,
    };
    uint8_t *manifest = NULL;
    size_t manifestSize = 0;
    uint32_t imagesSize = 0;
    BOOL built = imtl_batch_builder_build(&specification, &manifest,
                                          &manifestSize, &imagesSize);
    free(pipelines);
    free(buffers);
    free(dispatches);
    free(bindings);
    if (!built) {
        [commandBuffer
            infernoFail:
                InfernoMetalMakeError(
                    InfernoMetalErrorInvalidArgument,
                    @"The encoded command buffer is outside batch limits")
              scheduled:NO];
        return;
    }

    ImtlBatchObserver observer = {
        .scheduled = observeScheduled,
        .opaque = (__bridge void *)commandBuffer,
    };
    ImtlBatchReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    ImtlCoordinator *coordinator = self.coordinator;
    BOOL ok =
        coordinator && imtl_coordinator_execute_batch(
                           coordinator, manifest, manifestSize, bufferCount,
                           imagesSize, &observer, &reply, &coordinatorError);
    imtl_batch_builder_free(manifest);
    if (!ok) {
        [commandBuffer
            infernoFail:coordinatorBatchError(&coordinatorError,
                                              reply.scheduled_observed)
              scheduled:reply.scheduled_observed];
        return;
    }
    BOOL scheduled = reply.scheduled_observed ||
                     (reply.result.flags & INFERNO_METAL_BATCH_FLAG_SCHEDULED);
    NSError *error = InfernoMetalErrorFromBatchResult(
        &reply.result, reply.timer_error, reply.cleanup_io);
    if (reply.result.outcome == INFERNO_METAL_BATCH_OUTCOME_OK && !error) {
        size_t cursor = 0;
        for (uint32_t i = 0; i < bufferCount; i++) {
            InfernoMetalBuffer *buffer = bufferObjects[i];
            memcpy(buffer.contents, reply.result.images + cursor,
                   buffer.length);
            cursor += buffer.length;
        }
        [commandBuffer infernoCompleteWithError:nil scheduled:scheduled];
    } else {
        [commandBuffer infernoFail:error ?:
                                           InfernoMetalMakeError(
                                           InfernoMetalErrorProtocol,
                                           @"Missing batch failure diagnostic")
                         scheduled:scheduled];
    }
    imtl_batch_reply_free(&reply);
}

@end
