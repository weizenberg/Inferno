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

#import "InfernoMetalArgumentObjects.h"
#import "InfernoMetalBuffer.h"
#import "InfernoMetalCommandBuffer.h"
#import "InfernoMetalCommandQueue.h"
#import "InfernoMetalComputeCommandEncoder.h"
#import "InfernoMetalComputePipelineState.h"
#import "InfernoMetalContextPrivate.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#import "InfernoMetalLibrary.h"
#import "InfernoMetalRenderCommandEncoder.h"
#import "InfernoMetalRenderPipelineState.h"
#import "InfernoMetalSamplerState.h"
#import "InfernoMetalTexture.h"

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

static BOOL sameLibrary(InfernoMetalLibrary *a, InfernoMetalLibrary *b)
{
    return a.infernoLibraryKind == b.infernoLibraryKind &&
           [a.infernoPayload isEqual:b.infernoPayload];
}

static NSUInteger libraryIndex(NSArray<InfernoMetalLibrary *> *libraries,
                               InfernoMetalLibrary *library)
{
    for (NSUInteger i = 0; i < libraries.count; i++) {
        if (sameLibrary(libraries[i], library))
            return i;
    }
    return NSNotFound;
}

static BOOL sameComputePipeline(InfernoMetalComputePipelineState *a,
                                InfernoMetalComputePipelineState *b)
{
    return [a.function.name isEqualToString:b.function.name] &&
           sameLibrary(a.function.infernoLibrary, b.function.infernoLibrary);
}

static NSUInteger
computePipelineIndex(NSArray<InfernoMetalComputePipelineState *> *pipelines,
                     InfernoMetalComputePipelineState *pipeline)
{
    for (NSUInteger i = 0; i < pipelines.count; i++) {
        if (sameComputePipeline(pipelines[i], pipeline))
            return i;
    }
    return NSNotFound;
}

static BOOL sameRenderPipeline(InfernoMetalRenderPipelineState *a,
                               InfernoMetalRenderPipelineState *b)
{
    ImtlBatch5RenderPipeline ar = a.infernoRecord;
    ImtlBatch5RenderPipeline br = b.infernoRecord;
    return sameLibrary(a.infernoVertexFunction.infernoLibrary,
                       b.infernoVertexFunction.infernoLibrary) &&
           sameLibrary(a.infernoFragmentFunction.infernoLibrary,
                       b.infernoFragmentFunction.infernoLibrary) &&
           [a.infernoVertexFunction.name
               isEqualToString:b.infernoVertexFunction.name] &&
           [a.infernoFragmentFunction.name
               isEqualToString:b.infernoFragmentFunction.name] &&
           ar.color0_pixel_format == br.color0_pixel_format &&
           ar.raster_sample_count == br.raster_sample_count &&
           ar.blending_enabled == br.blending_enabled &&
           ar.source_rgb_blend_factor == br.source_rgb_blend_factor &&
           ar.destination_rgb_blend_factor == br.destination_rgb_blend_factor &&
           ar.rgb_blend_operation == br.rgb_blend_operation &&
           ar.source_alpha_blend_factor == br.source_alpha_blend_factor &&
           ar.destination_alpha_blend_factor ==
               br.destination_alpha_blend_factor &&
           ar.alpha_blend_operation == br.alpha_blend_operation &&
           ar.write_mask == br.write_mask;
}

static NSUInteger
renderPipelineIndex(NSArray<InfernoMetalRenderPipelineState *> *pipelines,
                    InfernoMetalRenderPipelineState *pipeline)
{
    for (NSUInteger i = 0; i < pipelines.count; i++) {
        if (sameRenderPipeline(pipelines[i], pipeline))
            return i;
    }
    return NSNotFound;
}

static NSUInteger samplerIndex(NSArray<InfernoMetalSamplerState *> *samplers,
                               InfernoMetalSamplerState *sampler);

static void
collectBindingResources(NSArray *bufferSlots, NSArray *textureSlots,
                        NSArray *samplerSlots,
                        NSMutableArray<InfernoMetalBuffer *> *buffers,
                        NSMutableArray<InfernoMetalTexture *> *textures,
                        NSMutableArray<InfernoMetalSamplerState *> *samplers)
{
    for (id slot in bufferSlots) {
        if ([slot isKindOfClass:[InfernoMetalEncodedArgument class]]) {
            InfernoMetalEncodedArgument *argument = slot;
            for (InfernoMetalEncodedArgumentMember *member in argument
                     .members) {
                id resource = member.resource;
                if ([resource isKindOfClass:[InfernoMetalBuffer class]] &&
                    identicalIndex(buffers, resource) == NSNotFound)
                    [buffers addObject:resource];
                else if ([resource isKindOfClass:[InfernoMetalTexture class]] &&
                         identicalIndex(textures, resource) == NSNotFound)
                    [textures addObject:resource];
                else if ([resource
                             isKindOfClass:[InfernoMetalSamplerState class]] &&
                         samplerIndex(samplers, resource) == NSNotFound)
                    [samplers addObject:resource];
            }
            continue;
        }
        if ([slot isKindOfClass:[NSDictionary class]] &&
            [slot[@"kind"] unsignedIntValue] ==
                INFERNO_METAL_RESOURCE_BINDING_BUFFER &&
            identicalIndex(buffers, slot[@"buffer"]) == NSNotFound)
            [buffers addObject:slot[@"buffer"]];
    }
    for (id slot in textureSlots) {
        if ([slot isKindOfClass:[NSDictionary class]] &&
            identicalIndex(textures, slot[@"texture"]) == NSNotFound)
            [textures addObject:slot[@"texture"]];
    }
    for (id slot in samplerSlots) {
        if ([slot isKindOfClass:[NSDictionary class]]) {
            InfernoMetalSamplerState *sampler = slot[@"sampler"];
            BOOL found = NO;
            ImtlBatch5Sampler record = sampler.infernoRecord;
            for (InfernoMetalSamplerState *candidate in samplers) {
                ImtlBatch5Sampler other = candidate.infernoRecord;
                if (!memcmp(&record, &other, sizeof(record))) {
                    found = YES;
                    break;
                }
            }
            if (!found)
                [samplers addObject:sampler];
        }
    }
}

static void
collectDeclarations(NSArray<InfernoMetalResourceDeclaration *> *declarations,
                    NSMutableArray<InfernoMetalBuffer *> *buffers,
                    NSMutableArray<InfernoMetalTexture *> *textures)
{
    for (InfernoMetalResourceDeclaration *declaration in declarations) {
        id resource = declaration.resource;
        if ([resource isKindOfClass:[InfernoMetalBuffer class]] &&
            identicalIndex(buffers, resource) == NSNotFound)
            [buffers addObject:resource];
        else if ([resource isKindOfClass:[InfernoMetalTexture class]] &&
                 identicalIndex(textures, resource) == NSNotFound)
            [textures addObject:resource];
    }
}

static void addUsage(NSMapTable *usages, id resource, uint32_t usage)
{
    NSNumber *old = [usages objectForKey:resource];
    [usages setObject:@(old.unsignedIntValue | usage) forKey:resource];
}

static NSUInteger samplerIndex(NSArray<InfernoMetalSamplerState *> *samplers,
                               InfernoMetalSamplerState *sampler)
{
    ImtlBatch5Sampler record = sampler.infernoRecord;
    for (NSUInteger i = 0; i < samplers.count; i++) {
        ImtlBatch5Sampler other = samplers[i].infernoRecord;
        if (!memcmp(&record, &other, sizeof(record)))
            return i;
    }
    return NSNotFound;
}

static uint32_t bindingCountForSlots(NSArray *buffers, NSArray *textures,
                                     NSArray *samplers)
{
    uint32_t count = 0;
    for (id slot in buffers)
        count += slot != [NSNull null];
    for (id slot in textures)
        count += slot != [NSNull null];
    for (id slot in samplers)
        count += slot != [NSNull null];
    return count;
}

static void appendBindings(ImtlBatchBinding *bindings, uint32_t *next,
                           NSArray *bufferSlots, NSArray *threadgroups,
                           NSArray *textureSlots, NSArray *samplerSlots,
                           NSArray<InfernoMetalEncodedArgument *> *arguments,
                           NSArray<InfernoMetalBuffer *> *buffers,
                           NSArray<InfernoMetalTexture *> *textures,
                           NSArray<InfernoMetalSamplerState *> *samplers)
{
    for (NSUInteger index = 0; index < 31; index++) {
        id slot = bufferSlots[index];
        if (slot != [NSNull null]) {
            if ([slot isKindOfClass:[InfernoMetalEncodedArgument class]]) {
                bindings[(*next)++] = (ImtlBatchBinding){
                    .kind = INFERNO_METAL_RESOURCE_BINDING_ARGUMENT,
                    .index = (uint32_t)index,
                    .resource_id = (uint32_t)identicalIndex(arguments, slot),
                };
            } else {
                uint32_t kind = [slot[@"kind"] unsignedIntValue];
                ImtlBatchBinding binding = { .kind = kind, .index = index };
                if (kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
                    binding.resource_id =
                        (uint32_t)identicalIndex(buffers, slot[@"buffer"]);
                    binding.offset = [slot[@"offset"] unsignedLongLongValue];
                } else {
                    NSData *data = slot[@"data"];
                    binding.bytes = data.bytes;
                    binding.length = data.length;
                }
                bindings[(*next)++] = binding;
            }
        }
        if (threadgroups && [threadgroups[index] unsignedIntegerValue]) {
            bindings[(*next)++] = (ImtlBatchBinding){
                .kind = INFERNO_METAL_RESOURCE_BINDING_THREADGROUP,
                .index = index,
                .length = [threadgroups[index] unsignedIntegerValue],
            };
        }
        slot = textureSlots[index];
        if (slot != [NSNull null]) {
            bindings[(*next)++] = (ImtlBatchBinding){
                .kind = INFERNO_METAL_RESOURCE_BINDING_TEXTURE,
                .index = index,
                .resource_id =
                    (uint32_t)identicalIndex(textures, slot[@"texture"]),
            };
        }
        if (index < 16) {
            slot = samplerSlots[index];
            if (slot != [NSNull null]) {
                bindings[(*next)++] = (ImtlBatchBinding){
                    .kind = INFERNO_METAL_RESOURCE_BINDING_SAMPLER,
                    .index = index,
                    .resource_id =
                        (uint32_t)samplerIndex(samplers, slot[@"sampler"]),
                };
            }
        }
    }
}

- (void)executeCommandBuffer:(InfernoMetalCommandBuffer *)commandBuffer
{
    NSArray *commandObjects = commandBuffer.infernoCommands;
    NSMutableArray<InfernoMetalLibrary *> *libraryObjects =
        [NSMutableArray array];
    NSMutableArray<InfernoMetalComputePipelineState *> *computeObjects =
        [NSMutableArray array];
    NSMutableArray<InfernoMetalRenderPipelineState *> *renderObjects =
        [NSMutableArray array];
    NSMutableArray<InfernoMetalBuffer *> *bufferObjects =
        [NSMutableArray array];
    NSMutableArray<InfernoMetalTexture *> *textureObjects =
        [NSMutableArray array];
    NSMutableArray<InfernoMetalSamplerState *> *samplerObjects =
        [NSMutableArray array];
    NSMutableArray<InfernoMetalEncodedArgument *> *argumentObjects =
        [NSMutableArray array];
    NSMapTable *effectiveUsages = [NSMapTable
        mapTableWithKeyOptions:NSPointerFunctionsStrongMemory |
                               NSPointerFunctionsObjectPointerPersonality
                  valueOptions:NSPointerFunctionsStrongMemory];
    uint64_t bindingCountWide = 0;
    uint64_t drawCountWide = 0;
    uint64_t memberCountWide = 0;
    uint64_t declarationCountWide = 0;

    for (id command in commandObjects) {
        if ([command isKindOfClass:[InfernoMetalEncodedDispatch class]]) {
            InfernoMetalEncodedDispatch *dispatch = command;
            InfernoMetalComputePipelineState *pipeline = (id)dispatch.pipeline;
            InfernoMetalLibrary *library = pipeline.function.infernoLibrary;
            if (libraryIndex(libraryObjects, library) == NSNotFound)
                [libraryObjects addObject:library];
            if (computePipelineIndex(computeObjects, pipeline) == NSNotFound)
                [computeObjects addObject:pipeline];
            collectBindingResources(dispatch.bindings, dispatch.textureBindings,
                                    dispatch.samplerBindings, bufferObjects,
                                    textureObjects, samplerObjects);
            collectDeclarations(dispatch.resourceDeclarations, bufferObjects,
                                textureObjects);
            declarationCountWide += dispatch.resourceDeclarations.count;
            for (id slot in dispatch.bindings) {
                if ([slot isKindOfClass:[InfernoMetalEncodedArgument class]]) {
                    InfernoMetalEncodedArgument *argument = slot;
                    if (identicalIndex(argumentObjects, argument) == NSNotFound)
                        [argumentObjects addObject:argument];
                    memberCountWide += argument.members.count;
                    for (InfernoMetalEncodedArgumentMember *member in argument
                             .members) {
                        uint32_t usage = member.layout.record.access ==
                                                 MTLBindingAccessReadOnly ?
                                             INFERNO_METAL_RESOURCE_USAGE_READ :
                                             INFERNO_METAL_RESOURCE_USAGE_WRITE;
                        if (member.resource)
                            addUsage(effectiveUsages, member.resource, usage);
                    }
                } else if ([slot isKindOfClass:[NSDictionary class]] &&
                           [slot[@"kind"] unsignedIntValue] ==
                               INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
                    addUsage(effectiveUsages, slot[@"buffer"],
                             INFERNO_METAL_RESOURCE_USAGE_WRITE);
                }
            }
            for (id slot in dispatch.textureBindings) {
                if ([slot isKindOfClass:[NSDictionary class]])
                    addUsage(effectiveUsages, slot[@"texture"],
                             INFERNO_METAL_RESOURCE_USAGE_WRITE);
            }
            for (InfernoMetalResourceDeclaration *declaration in dispatch
                     .resourceDeclarations)
                addUsage(effectiveUsages, declaration.resource,
                         declaration.usage);
            bindingCountWide += bindingCountForSlots(dispatch.bindings,
                                                     dispatch.textureBindings,
                                                     dispatch.samplerBindings);
            for (NSNumber *length in dispatch.threadgroupLengths)
                bindingCountWide += length.unsignedIntegerValue != 0;
        } else if ([command
                       isKindOfClass:[InfernoMetalEncodedRenderPass class]]) {
            InfernoMetalEncodedRenderPass *pass = command;
            if (identicalIndex(textureObjects, pass.attachment) == NSNotFound)
                [textureObjects addObject:pass.attachment];
            addUsage(effectiveUsages, pass.attachment,
                     INFERNO_METAL_RESOURCE_USAGE_WRITE);
            drawCountWide += pass.draws.count;
            for (InfernoMetalEncodedDraw *draw in pass.draws) {
                InfernoMetalRenderPipelineState *pipeline = draw.pipeline;
                for (InfernoMetalLibrary *library in @[
                         pipeline.infernoVertexFunction.infernoLibrary,
                         pipeline.infernoFragmentFunction.infernoLibrary
                     ]) {
                    if (libraryIndex(libraryObjects, library) == NSNotFound)
                        [libraryObjects addObject:library];
                }
                if (renderPipelineIndex(renderObjects, pipeline) == NSNotFound)
                    [renderObjects addObject:pipeline];
                collectBindingResources(
                    draw.vertexBindings, draw.vertexTextureBindings,
                    draw.vertexSamplerBindings, bufferObjects, textureObjects,
                    samplerObjects);
                for (id slot in draw.vertexBindings) {
                    if ([slot isKindOfClass:[NSDictionary class]] &&
                        [slot[@"kind"] unsignedIntValue] ==
                            INFERNO_METAL_RESOURCE_BINDING_BUFFER)
                        addUsage(effectiveUsages, slot[@"buffer"],
                                 INFERNO_METAL_RESOURCE_USAGE_WRITE);
                }
                for (id slot in draw.fragmentBindings) {
                    if ([slot isKindOfClass:[NSDictionary class]] &&
                        [slot[@"kind"] unsignedIntValue] ==
                            INFERNO_METAL_RESOURCE_BINDING_BUFFER)
                        addUsage(effectiveUsages, slot[@"buffer"],
                                 INFERNO_METAL_RESOURCE_USAGE_WRITE);
                }
                for (id slot in [draw.vertexTextureBindings
                         arrayByAddingObjectsFromArray:
                             draw.fragmentTextureBindings]) {
                    if ([slot isKindOfClass:[NSDictionary class]])
                        addUsage(effectiveUsages, slot[@"texture"],
                                 INFERNO_METAL_RESOURCE_USAGE_WRITE);
                }
                collectBindingResources(
                    draw.fragmentBindings, draw.fragmentTextureBindings,
                    draw.fragmentSamplerBindings, bufferObjects, textureObjects,
                    samplerObjects);
                bindingCountWide += bindingCountForSlots(
                    draw.vertexBindings, draw.vertexTextureBindings,
                    draw.vertexSamplerBindings);
                bindingCountWide += bindingCountForSlots(
                    draw.fragmentBindings, draw.fragmentTextureBindings,
                    draw.fragmentSamplerBindings);
            }
        } else {
            [commandBuffer
                infernoFail:InfernoMetalMakeError(
                                InfernoMetalErrorProtocol,
                                @"The ordered command record is unknown")
                  scheduled:NO];
            return;
        }
    }

    NSUInteger libraryCount = libraryObjects.count;
    NSUInteger computeCount = computeObjects.count;
    NSUInteger renderCount = renderObjects.count;
    NSUInteger bufferCount = bufferObjects.count;
    NSUInteger textureCount = textureObjects.count;
    NSUInteger samplerCount = samplerObjects.count;
    NSUInteger commandCount = commandObjects.count;
    NSUInteger argumentCount = argumentObjects.count;
    if (libraryCount > INFERNO_METAL_RESOURCE_MAX_LIBRARIES ||
        computeCount > INFERNO_METAL_RESOURCE_MAX_COMPUTE_PIPELINES ||
        renderCount > INFERNO_METAL_RESOURCE_MAX_RENDER_PIPELINES ||
        bufferCount > INFERNO_METAL_RESOURCE_MAX_BUFFERS ||
        textureCount > INFERNO_METAL_RESOURCE_MAX_TEXTURES ||
        samplerCount > INFERNO_METAL_RESOURCE_MAX_SAMPLERS ||
        commandCount > INFERNO_METAL_RESOURCE_MAX_COMMANDS ||
        drawCountWide > INFERNO_METAL_RESOURCE_MAX_DRAWS ||
        bindingCountWide > INFERNO_METAL_RESOURCE_MAX_BINDINGS ||
        argumentCount > INFERNO_METAL_RESOURCE_MAX_ARGUMENTS ||
        memberCountWide > INFERNO_METAL_RESOURCE_MAX_MEMBERS ||
        declarationCountWide > INFERNO_METAL_RESOURCE_MAX_DECLARATIONS) {
        [commandBuffer
            infernoFail:InfernoMetalMakeError(
                            InfernoMetalErrorInvalidArgument,
                            @"The command buffer exceeds resource batch limits")
              scheduled:NO];
        return;
    }

#define ALLOC_TABLE(Type, Name, Count) \
    Type *Name = calloc((Count) ? (Count) : 1, sizeof(*Name))
    ALLOC_TABLE(ImtlBatch5Library, libraries, libraryCount);
    ALLOC_TABLE(ImtlBatch5ComputePipeline, computePipelines, computeCount);
    ALLOC_TABLE(ImtlBatch5RenderPipeline, renderPipelines, renderCount);
    ALLOC_TABLE(ImtlBatchBuffer, buffers, bufferCount);
    ALLOC_TABLE(ImtlBatch5Texture, textures, textureCount);
    ALLOC_TABLE(ImtlBatch5Sampler, samplers, samplerCount);
    ALLOC_TABLE(ImtlBatch5Command, commands, commandCount);
    ALLOC_TABLE(ImtlBatch5Draw, draws, drawCountWide);
    ALLOC_TABLE(ImtlBatchBinding, bindings, bindingCountWide);
    ALLOC_TABLE(ImtlBatch5Argument, arguments, argumentCount);
    ALLOC_TABLE(ImtlBatch5ArgumentMember, members, memberCountWide);
    ALLOC_TABLE(ImtlBatch5ResourceDeclaration, declarations,
                declarationCountWide);
#undef ALLOC_TABLE
    if (!libraries || !computePipelines || !renderPipelines || !buffers ||
        !textures || !samplers || !commands || !draws || !bindings ||
        !arguments || !members || !declarations) {
        free(libraries);
        free(computePipelines);
        free(renderPipelines);
        free(buffers);
        free(textures);
        free(samplers);
        free(commands);
        free(draws);
        free(bindings);
        free(arguments);
        free(members);
        free(declarations);
        [commandBuffer
            infernoFail:InfernoMetalMakeError(
                            InfernoMetalErrorTransport,
                            @"Cannot allocate a resource batch manifest")
              scheduled:NO];
        return;
    }

    NSMutableArray<NSData *> *images = [NSMutableArray array];
    for (NSUInteger i = 0; i < libraryCount; i++) {
        InfernoMetalLibrary *library = libraryObjects[i];
        libraries[i] = (ImtlBatch5Library){
            .kind = library.infernoLibraryKind,
            .bytes = library.infernoPayload.bytes,
            .size = library.infernoPayload.length,
        };
    }
    for (NSUInteger i = 0; i < computeCount; i++) {
        InfernoMetalFunction *function = computeObjects[i].function;
        computePipelines[i] = (ImtlBatch5ComputePipeline){
            .library_id =
                (uint32_t)libraryIndex(libraryObjects, function.infernoLibrary),
            .function_name = function.name.UTF8String,
        };
    }
    for (NSUInteger i = 0; i < renderCount; i++) {
        InfernoMetalRenderPipelineState *pipeline = renderObjects[i];
        ImtlBatch5RenderPipeline record = pipeline.infernoRecord;
        record.vertex_library_id = (uint32_t)libraryIndex(
            libraryObjects, pipeline.infernoVertexFunction.infernoLibrary);
        record.fragment_library_id = (uint32_t)libraryIndex(
            libraryObjects, pipeline.infernoFragmentFunction.infernoLibrary);
        renderPipelines[i] = record;
    }
    for (NSUInteger i = 0; i < bufferCount; i++) {
        NSData *image = bufferObjects[i].infernoSnapshot;
        [images addObject:image];
        buffers[i] =
            (ImtlBatchBuffer){ .bytes = image.bytes, .length = image.length };
    }
    for (NSUInteger i = 0; i < textureCount; i++) {
        InfernoMetalTexture *texture = textureObjects[i];
        NSData *image = texture.infernoSnapshot;
        [images addObject:image];
        textures[i] = (ImtlBatch5Texture){
            .bytes = image.bytes,
            .width = (uint32_t)texture.width,
            .height = (uint32_t)texture.height,
            .pixel_format = (uint32_t)texture.pixelFormat,
            .usage = (uint32_t)texture.usage,
            .flags =
                texture.allowGPUOptimizedContents ?
                    INFERNO_METAL_RESOURCE_TEXTURE_ALLOW_GPU_OPTIMIZED_CONTENTS :
                    0,
        };
    }
    for (NSUInteger i = 0; i < samplerCount; i++)
        samplers[i] = samplerObjects[i].infernoRecord;

    uint32_t nextMember = 0;
    for (NSUInteger i = 0; i < argumentCount; i++) {
        InfernoMetalEncodedArgument *argument = argumentObjects[i];
        InfernoMetalArgumentLayoutKey *key = argument.layout.key;
        NSUInteger argumentLibrary = NSNotFound;
        for (NSUInteger j = 0; j < libraryObjects.count; j++) {
            InfernoMetalLibrary *library = libraryObjects[j];
            if (library.infernoLibraryKind == key.libraryKind &&
                [library.infernoPayload isEqual:key.payload]) {
                argumentLibrary = j;
                break;
            }
        }
        arguments[i] = (ImtlBatch5Argument){
            .library_id = (uint32_t)argumentLibrary,
            .buffer_index = (uint32_t)key.bufferIndex,
            .member_start = nextMember,
            .member_count = (uint32_t)argument.members.count,
            .encoded_length = (uint32_t)argument.layout.encodedLength,
            .alignment = (uint32_t)argument.layout.alignment,
            .function_name = key.functionName.UTF8String,
        };
        for (InfernoMetalEncodedArgumentMember *member in argument.members) {
            ImtlArgumentLayoutMember layout = member.layout.record;
            ImtlBatch5ArgumentMember wire = {
                .kind = layout.kind,
                .member_id = (uint32_t)member.memberIndex,
            };
            if (layout.kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER) {
                wire.resource =
                    (uint32_t)identicalIndex(bufferObjects, member.resource);
                wire.offset = member.offset;
            } else if (layout.kind ==
                       INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE) {
                wire.resource =
                    (uint32_t)identicalIndex(textureObjects, member.resource);
            } else if (layout.kind ==
                       INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER) {
                wire.resource =
                    (uint32_t)samplerIndex(samplerObjects, member.resource);
            } else {
                wire.constant_bytes = member.constantData.bytes;
                wire.constant_length = (uint32_t)member.constantData.length;
            }
            members[nextMember++] = wire;
        }
    }

    uint32_t nextBinding = 0;
    uint32_t nextDraw = 0;
    uint32_t nextDeclaration = 0;
    for (NSUInteger i = 0; i < commandCount; i++) {
        id command = commandObjects[i];
        if ([command isKindOfClass:[InfernoMetalEncodedDispatch class]]) {
            InfernoMetalEncodedDispatch *dispatch = command;
            uint32_t start = nextBinding;
            appendBindings(bindings, &nextBinding, dispatch.bindings,
                           dispatch.threadgroupLengths,
                           dispatch.textureBindings, dispatch.samplerBindings,
                           argumentObjects, bufferObjects, textureObjects,
                           samplerObjects);
            NSArray<InfernoMetalResourceDeclaration *> *orderedDeclarations =
                [dispatch.resourceDeclarations
                    sortedArrayUsingComparator:^NSComparisonResult(
                        InfernoMetalResourceDeclaration *a,
                        InfernoMetalResourceDeclaration *b) {
                      uint32_t ak =
                          [a.resource
                              isKindOfClass:[InfernoMetalBuffer class]] ?
                              INFERNO_METAL_RESOURCE_DECLARATION_BUFFER :
                              INFERNO_METAL_RESOURCE_DECLARATION_TEXTURE;
                      uint32_t bk =
                          [b.resource
                              isKindOfClass:[InfernoMetalBuffer class]] ?
                              INFERNO_METAL_RESOURCE_DECLARATION_BUFFER :
                              INFERNO_METAL_RESOURCE_DECLARATION_TEXTURE;
                      NSUInteger ai =
                          ak == INFERNO_METAL_RESOURCE_DECLARATION_BUFFER ?
                              identicalIndex(bufferObjects, a.resource) :
                              identicalIndex(textureObjects, a.resource);
                      NSUInteger bi =
                          bk == INFERNO_METAL_RESOURCE_DECLARATION_BUFFER ?
                              identicalIndex(bufferObjects, b.resource) :
                              identicalIndex(textureObjects, b.resource);
                      if (ak != bk)
                          return ak < bk ? NSOrderedAscending :
                                           NSOrderedDescending;
                      if (ai == bi)
                          return NSOrderedSame;
                      return ai < bi ? NSOrderedAscending : NSOrderedDescending;
                    }];
            uint32_t declarationStart = nextDeclaration;
            for (InfernoMetalResourceDeclaration
                     *declaration in orderedDeclarations) {
                BOOL isBuffer = [declaration.resource
                    isKindOfClass:[InfernoMetalBuffer class]];
                declarations[nextDeclaration++] =
                    (ImtlBatch5ResourceDeclaration){
                        .resource_kind =
                            isBuffer ?
                                INFERNO_METAL_RESOURCE_DECLARATION_BUFFER :
                                INFERNO_METAL_RESOURCE_DECLARATION_TEXTURE,
                        .resource =
                            (uint32_t)(isBuffer ? identicalIndex(
                                                      bufferObjects,
                                                      declaration.resource) :
                                                  identicalIndex(
                                                      textureObjects,
                                                      declaration.resource)),
                        .usage = declaration.usage,
                    };
            }
            commands[i] = (ImtlBatch5Command){
                .kind = INFERNO_METAL_RESOURCE_COMMAND_COMPUTE,
                .value.compute = {
                    .pipeline_id = (uint32_t)computePipelineIndex(
                        computeObjects, (id)dispatch.pipeline),
                    .mode = dispatch.mode,
                    .binding_start = start,
                    .binding_count = nextBinding - start,
                    .declaration_start = declarationStart,
                    .declaration_count = nextDeclaration - declarationStart,
                    .grid_width = (uint32_t)dispatch.grid.width,
                    .grid_height = (uint32_t)dispatch.grid.height,
                    .grid_depth = (uint32_t)dispatch.grid.depth,
                    .group_width = (uint32_t)dispatch.group.width,
                    .group_height = (uint32_t)dispatch.group.height,
                    .group_depth = (uint32_t)dispatch.group.depth,
                },
            };
        } else {
            InfernoMetalEncodedRenderPass *pass = command;
            uint32_t drawStart = nextDraw;
            for (InfernoMetalEncodedDraw *draw in pass.draws) {
                uint32_t vertexStart = nextBinding;
                appendBindings(bindings, &nextBinding, draw.vertexBindings, nil,
                               draw.vertexTextureBindings,
                               draw.vertexSamplerBindings, argumentObjects,
                               bufferObjects, textureObjects, samplerObjects);
                uint32_t fragmentStart = nextBinding;
                appendBindings(bindings, &nextBinding, draw.fragmentBindings,
                               nil, draw.fragmentTextureBindings,
                               draw.fragmentSamplerBindings, argumentObjects,
                               bufferObjects, textureObjects, samplerObjects);
                uint32_t flags = 0;
                if (draw.hasScissor)
                    flags |= INFERNO_METAL_RESOURCE_DRAW_SCISSOR;
                if (draw.hasViewport)
                    flags |= INFERNO_METAL_RESOURCE_DRAW_VIEWPORT;
                if (draw.hasBlendColor)
                    flags |= INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR;
                draws[nextDraw++] = (ImtlBatch5Draw){
                    .render_pipeline_id = (uint32_t)renderPipelineIndex(
                        renderObjects, draw.pipeline),
                    .primitive_type = (uint32_t)draw.primitiveType,
                    .vertex_start = (uint32_t)draw.vertexStart,
                    .vertex_count = (uint32_t)draw.vertexCount,
                    .instance_count = (uint32_t)draw.instanceCount,
                    .base_instance = (uint32_t)draw.baseInstance,
                    .vertex_binding_start = vertexStart,
                    .vertex_binding_count = fragmentStart - vertexStart,
                    .fragment_binding_start = fragmentStart,
                    .fragment_binding_count = nextBinding - fragmentStart,
                    .cull_mode = (uint32_t)draw.cullMode,
                    .winding = (uint32_t)draw.winding,
                    .fill_mode = (uint32_t)draw.fillMode,
                    .flags = flags,
                    .scissor_x = (uint32_t)draw.scissor.x,
                    .scissor_y = (uint32_t)draw.scissor.y,
                    .scissor_width = (uint32_t)draw.scissor.width,
                    .scissor_height = (uint32_t)draw.scissor.height,
                    .viewport_origin_x = draw.viewport.originX,
                    .viewport_origin_y = draw.viewport.originY,
                    .viewport_width = draw.viewport.width,
                    .viewport_height = draw.viewport.height,
                    .viewport_znear = draw.viewport.znear,
                    .viewport_zfar = draw.viewport.zfar,
                    .blend_red = (float)draw.blendColor.red,
                    .blend_green = (float)draw.blendColor.green,
                    .blend_blue = (float)draw.blendColor.blue,
                    .blend_alpha = (float)draw.blendColor.alpha,
                };
            }
            commands[i] = (ImtlBatch5Command){
                .kind = INFERNO_METAL_RESOURCE_COMMAND_RENDER,
                .value.render = {
                    .texture_id = (uint32_t)identicalIndex(
                        textureObjects, pass.attachment),
                    .load_action = (uint32_t)pass.loadAction,
                    .store_action = (uint32_t)pass.storeAction,
                    .draw_start = drawStart,
                    .draw_count = nextDraw - drawStart,
                    .clear_red = pass.clearColor.red,
                    .clear_green = pass.clearColor.green,
                    .clear_blue = pass.clearColor.blue,
                    .clear_alpha = pass.clearColor.alpha,
                },
            };
        }
    }

    ImtlBatch5Manifest specification = {
        .libraries = libraries,
        .library_count = (uint32_t)libraryCount,
        .compute_pipelines = computePipelines,
        .compute_pipeline_count = (uint32_t)computeCount,
        .render_pipelines = renderPipelines,
        .render_pipeline_count = (uint32_t)renderCount,
        .buffers = buffers,
        .buffer_count = (uint32_t)bufferCount,
        .textures = textures,
        .texture_count = (uint32_t)textureCount,
        .samplers = samplers,
        .sampler_count = (uint32_t)samplerCount,
        .commands = commands,
        .command_count = (uint32_t)commandCount,
        .draws = draws,
        .draw_count = (uint32_t)drawCountWide,
        .bindings = bindings,
        .binding_count = (uint32_t)bindingCountWide,
        .arguments = arguments,
        .argument_count = (uint32_t)argumentCount,
        .members = members,
        .member_count = (uint32_t)memberCountWide,
        .declarations = declarations,
        .declaration_count = (uint32_t)declarationCountWide,
    };
    uint8_t *manifest = NULL;
    size_t manifestSize = 0;
    uint32_t imagesSize = 0;
    BOOL built = imtl_batch5_builder_build(&specification, &manifest,
                                           &manifestSize, &imagesSize);
    free(libraries);
    free(computePipelines);
    free(renderPipelines);
    free(buffers);
    free(textures);
    free(samplers);
    free(commands);
    free(draws);
    free(bindings);
    free(arguments);
    free(members);
    free(declarations);
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
    ImtlBatch5Reply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    ImtlCoordinator *coordinator = self.coordinator;
    BOOL ok =
        coordinator && imtl_coordinator_execute_batch5(
                           coordinator, manifest, manifestSize,
                           (uint32_t)bufferCount, (uint32_t)textureCount,
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
    NSError *error = InfernoMetalErrorFromBatch5Result(
        &reply.result, reply.timer_error, reply.cleanup_io);
    if (reply.result.outcome == INFERNO_METAL_BATCH_OUTCOME_OK && !error) {
        NSMutableArray<NSData *> *writebacks = [NSMutableArray array];
        size_t cursor = 0;
        for (InfernoMetalBuffer *buffer in bufferObjects) {
            [writebacks
                addObject:[NSData dataWithBytes:reply.result.images + cursor
                                         length:buffer.length]];
            cursor += buffer.length;
        }
        for (InfernoMetalTexture *texture in textureObjects) {
            [writebacks
                addObject:[NSData dataWithBytes:reply.result.images + cursor
                                         length:texture.infernoImageSize]];
            cursor += texture.infernoImageSize;
        }
        if (cursor != reply.result.images_size) {
            error =
                InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                      @"Resource reply size is inconsistent");
        } else {
            NSUInteger index = 0;
            for (InfernoMetalBuffer *buffer in bufferObjects) {
                NSData *writeback = writebacks[index++];
                if ([[effectiveUsages objectForKey:buffer] unsignedIntValue] &
                    INFERNO_METAL_RESOURCE_USAGE_WRITE)
                    [buffer infernoReplaceSnapshot:writeback];
            }
            for (InfernoMetalTexture *texture in textureObjects) {
                NSData *writeback = writebacks[index++];
                if ([[effectiveUsages objectForKey:texture] unsignedIntValue] &
                    INFERNO_METAL_RESOURCE_USAGE_WRITE)
                    [texture infernoReplaceSnapshot:writeback];
            }
        }
    }
    if (error)
        [commandBuffer infernoFail:error scheduled:scheduled];
    else
        [commandBuffer infernoCompleteWithError:nil scheduled:scheduled];
    imtl_batch5_reply_free(&reply);
}

@end
