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
#import "InfernoMetalComputeCommandEncoder.h"
#import "InfernoMetalComputePipelineState.h"
#import "InfernoMetalErrors.h"

#include "standard-headers/inferno/metal.h"

@implementation InfernoMetalEncodedDispatch
@end

@interface InfernoMetalComputePipelineState (Execution)
@property(nonatomic, readonly) InfernoMetalCompilerContext *context;
@end

@interface InfernoMetalComputeCommandEncoder ()
@property(nonatomic, strong, nullable) InfernoMetalCommandBuffer *commandBuffer;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, strong) id<MTLComputePipelineState> pipeline;
@property(nonatomic, strong) NSMutableArray *slots;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *threadgroups;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@property(nonatomic) BOOL ended;
@end

@implementation InfernoMetalComputeCommandEncoder

static void encoderInvalid(NSString *message)
{
    [NSException raise:InfernoMetalInvalidUseException format:@"%@", message];
}

- (instancetype)initWithCommandBuffer:(InfernoMetalCommandBuffer *)buffer
                               device:(id<MTLDevice>)device
{
    if (!(self = [super init]) || !buffer || !device)
        return nil;
    _commandBuffer = buffer;
    _storedDevice = device;
    _slots = [NSMutableArray arrayWithCapacity:31];
    _threadgroups = [NSMutableArray arrayWithCapacity:31];
    for (unsigned i = 0; i < 31; i++) {
        [_slots addObject:[NSNull null]];
        [_threadgroups addObject:@0];
    }
    return self;
}

- (void)dealloc
{
    if (!_ended && _commandBuffer)
        [_commandBuffer infernoEncoderEnded:self];
}

- (void)requireActive
{
    if (_ended || !_commandBuffer)
        encoderInvalid(@"The compute encoder has ended");
}

- (void)requireIndex:(NSUInteger)index
{
    if (index > 30)
        encoderInvalid(@"A buffer binding index must be at most 30");
}

- (id<MTLDevice>)device
{
    return _storedDevice;
}
- (MTLDispatchType)dispatchType
{
    return MTLDispatchTypeSerial;
}
- (NSString *)label
{
    return _storedLabel;
}
- (void)setLabel:(NSString *)label
{
    [self requireActive];
    _storedLabel = [label copy];
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline
{
    [self requireActive];
    if (![pipeline isKindOfClass:[InfernoMetalComputePipelineState class]] ||
        ((InfernoMetalComputePipelineState *)pipeline).context !=
            _commandBuffer.infernoContext)
        encoderInvalid(@"The pipeline belongs to another Metal context");
    _pipeline = pipeline;
}

- (void)setBytes:(const void *)bytes
          length:(NSUInteger)length
         atIndex:(NSUInteger)index
{
    [self requireActive];
    [self requireIndex:index];
    if (!bytes || !length || length > 4096)
        encoderInvalid(@"Inline buffer data must contain 1 through 4096 bytes");
    _slots[index] = @{
        @"kind" : @(INFERNO_METAL_BATCH_BINDING_INLINE),
        @"data" : [NSData dataWithBytes:bytes length:length]
    };
}

- (void)setBuffer:(id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
          atIndex:(NSUInteger)index
{
    [self requireActive];
    [self requireIndex:index];
    if (!buffer) {
        _slots[index] = [NSNull null];
        return;
    }
    if (![buffer isKindOfClass:[InfernoMetalBuffer class]] ||
        ((InfernoMetalBuffer *)buffer).infernoContext !=
            _commandBuffer.infernoContext ||
        (offset & 3) || offset >= buffer.length)
        encoderInvalid(
            @"The buffer binding is foreign, misaligned, or out of range");
    _slots[index] = @{
        @"kind" : @(INFERNO_METAL_BATCH_BINDING_BUFFER),
        @"buffer" : buffer,
        @"offset" : @(offset)
    };
}

- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    [self requireActive];
    [self requireIndex:index];
    id value = _slots[index];
    if (![value isKindOfClass:[NSDictionary class]] ||
        [value[@"kind"] unsignedIntValue] != INFERNO_METAL_BATCH_BINDING_BUFFER)
        encoderInvalid(@"setBufferOffset requires an existing buffer binding");
    [self setBuffer:value[@"buffer"] offset:offset atIndex:index];
}

- (void)setBuffers:(const id<MTLBuffer>[])buffers
           offsets:(const NSUInteger[])offsets
         withRange:(NSRange)range
{
    [self requireActive];
    if (!buffers || !offsets || range.location > 31 ||
        range.length > 31 - range.location)
        encoderInvalid(@"The buffer binding range is invalid");
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i]
                 offset:offsets[i]
                atIndex:range.location + i];
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
    [self requireActive];
    [self requireIndex:index];
    if (length > 32768 || (length & 15))
        encoderInvalid(
            @"Threadgroup memory must be a multiple of 16 up to 32768");
    _threadgroups[index] = @(length);
}

static BOOL dimensionsWithin(MTLSize value, uint64_t limit)
{
    if (!value.width || !value.height || !value.depth || value.width > limit ||
        value.height > limit / value.width)
        return NO;
    uint64_t area = value.width * value.height;
    return value.depth <= limit / area;
}

- (void)recordDispatch:(MTLSize)grid group:(MTLSize)group mode:(uint32_t)mode
{
    [self requireActive];
    if (!_pipeline)
        encoderInvalid(@"A compute pipeline is required before dispatch");
    if (!dimensionsWithin(grid, INFERNO_METAL_MAX_THREADS) ||
        !dimensionsWithin(group, INFERNO_METAL_MAX_THREADS))
        encoderInvalid(@"Dispatch dimensions exceed the bridge limit");
    uint64_t gridProduct = grid.width * grid.height * grid.depth;
    uint64_t groupProduct = group.width * group.height * group.depth;
    if (mode == INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS &&
        gridProduct > INFERNO_METAL_MAX_THREADS / groupProduct)
        encoderInvalid(@"The dispatched thread count exceeds the bridge limit");
    InfernoMetalEncodedDispatch *dispatch =
        [[InfernoMetalEncodedDispatch alloc] init];
    dispatch.pipeline = _pipeline;
    dispatch.bindings = [_slots copy];
    dispatch.threadgroupLengths = [_threadgroups copy];
    dispatch.grid = grid;
    dispatch.group = group;
    dispatch.mode = mode;
    if (![_commandBuffer infernoAppendDispatch:dispatch])
        encoderInvalid(@"Cannot encode into a committed command buffer");
}

- (void)dispatchThreads:(MTLSize)threadsPerGrid
    threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup
{
    [self recordDispatch:threadsPerGrid
                   group:threadsPerThreadgroup
                    mode:INFERNO_METAL_BATCH_DISPATCH_THREADS];
}

- (void)dispatchThreadgroups:(MTLSize)threadgroupsPerGrid
       threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup
{
    [self recordDispatch:threadgroupsPerGrid
                   group:threadsPerThreadgroup
                    mode:INFERNO_METAL_BATCH_DISPATCH_THREADGROUPS];
}

- (void)endEncoding
{
    [self requireActive];
    InfernoMetalCommandBuffer *buffer = _commandBuffer;
    _ended = YES;
    _commandBuffer = nil;
    [buffer infernoEncoderEnded:self];
}

- (void)insertDebugSignpost:(NSString *)string
{
    (void)string;
    [self requireActive];
}
- (void)pushDebugGroup:(NSString *)string
{
    (void)string;
    [self requireActive];
}
- (void)popDebugGroup
{
    [self requireActive];
}
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before
{
    (void)after;
    (void)before;
    [self requireActive];
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
{
    (void)scope;
    [self requireActive];
}
- (void)memoryBarrierWithResources:(const id<MTLResource>[])resources
                             count:(NSUInteger)count
{
    (void)resources;
    (void)count;
    [self requireActive];
}

- (void)unsupported
{
    [self requireActive];
    [NSException raise:InfernoMetalUnsupportedException
                format:@"This compute encoder operation is unsupported"];
}

- (void)setBuffer:(id<MTLBuffer>)buffer
             offset:(NSUInteger)offset
    attributeStride:(NSUInteger)stride
            atIndex:(NSUInteger)index
{
    (void)buffer;
    (void)offset;
    (void)stride;
    (void)index;
    [self unsupported];
}
- (void)setBuffers:(const id<MTLBuffer>[])buffers
             offsets:(const NSUInteger[])offsets
    attributeStrides:(const NSUInteger[])strides
           withRange:(NSRange)range
{
    (void)buffers;
    (void)offsets;
    (void)strides;
    (void)range;
    [self unsupported];
}
- (void)setBufferOffset:(NSUInteger)offset
        attributeStride:(NSUInteger)stride
                atIndex:(NSUInteger)index
{
    (void)offset;
    (void)stride;
    (void)index;
    [self unsupported];
}
- (void)setBytes:(const void *)bytes
             length:(NSUInteger)length
    attributeStride:(NSUInteger)stride
            atIndex:(NSUInteger)index
{
    (void)bytes;
    (void)length;
    (void)stride;
    (void)index;
    [self unsupported];
}
- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table
                  atBufferIndex:(NSUInteger)index
{
    (void)table;
    (void)index;
    [self unsupported];
}
- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable>[])tables
                 withBufferRange:(NSRange)range
{
    (void)tables;
    (void)range;
    [self unsupported];
}
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)table
                       atBufferIndex:(NSUInteger)index
{
    (void)table;
    (void)index;
    [self unsupported];
}
- (void)setIntersectionFunctionTables:
            (const id<MTLIntersectionFunctionTable>[])tables
                      withBufferRange:(NSRange)range
{
    (void)tables;
    (void)range;
    [self unsupported];
}
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)structure
                   atBufferIndex:(NSUInteger)index
{
    (void)structure;
    (void)index;
    [self unsupported];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    (void)texture;
    (void)index;
    [self unsupported];
}
- (void)setTextures:(const id<MTLTexture>[])textures withRange:(NSRange)range
{
    (void)textures;
    (void)range;
    [self unsupported];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)index;
    [self unsupported];
}
- (void)setSamplerStates:(const id<MTLSamplerState>[])samplers
               withRange:(NSRange)range
{
    (void)samplers;
    (void)range;
    [self unsupported];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler
            lodMinClamp:(float)min
            lodMaxClamp:(float)max
                atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)min;
    (void)max;
    (void)index;
    [self unsupported];
}
- (void)setSamplerStates:(const id<MTLSamplerState>[])samplers
            lodMinClamps:(const float[])mins
            lodMaxClamps:(const float[])maxes
               withRange:(NSRange)range
{
    (void)samplers;
    (void)mins;
    (void)maxes;
    (void)range;
    [self unsupported];
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height
{
    (void)width;
    (void)height;
    [self unsupported];
}
- (void)setStageInRegion:(MTLRegion)region
{
    (void)region;
    [self unsupported];
}
- (void)setStageInRegionWithIndirectBuffer:(id<MTLBuffer>)buffer
                      indirectBufferOffset:(NSUInteger)offset
{
    (void)buffer;
    (void)offset;
    [self unsupported];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buffer
                          indirectBufferOffset:(NSUInteger)offset
                         threadsPerThreadgroup:(MTLSize)size
{
    (void)buffer;
    (void)offset;
    (void)size;
    [self unsupported];
}
- (void)updateFence:(id<MTLFence>)fence
{
    (void)fence;
    [self unsupported];
}
- (void)waitForFence:(id<MTLFence>)fence
{
    (void)fence;
    [self unsupported];
}
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage
{
    (void)resource;
    (void)usage;
    [self unsupported];
}
- (void)useResources:(const id<MTLResource>[])resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
{
    (void)resources;
    (void)count;
    (void)usage;
    [self unsupported];
}
- (void)useHeap:(id<MTLHeap>)heap
{
    (void)heap;
    [self unsupported];
}
- (void)useHeaps:(const id<MTLHeap>[])heaps count:(NSUInteger)count
{
    (void)heaps;
    (void)count;
    [self unsupported];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)buffer
                      withRange:(NSRange)range
{
    (void)buffer;
    (void)range;
    [self unsupported];
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)commands
                 indirectBuffer:(id<MTLBuffer>)buffer
           indirectBufferOffset:(NSUInteger)offset
{
    (void)commands;
    (void)buffer;
    (void)offset;
    [self unsupported];
}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)buffer
                 atSampleIndex:(NSUInteger)index
                   withBarrier:(BOOL)barrier
{
    (void)buffer;
    (void)index;
    (void)barrier;
    [self unsupported];
}

@end
