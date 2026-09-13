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

#import "InfernoMetalArgumentEncoder.h"
#import "InfernoMetalArgumentObjects.h"
#import "InfernoMetalBuffer.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalSamplerState.h"
#import "InfernoMetalTexture.h"

#include "standard-headers/inferno/metal.h"

@interface InfernoMetalArgumentEncoder ()
@property(nonatomic, strong) InfernoMetalArgumentLayout *layout;
@property(nonatomic, strong) InfernoMetalCompilerContext *context;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(atomic, copy, nullable) NSString *storedLabel;
@property(nonatomic, strong, nullable) InfernoMetalBuffer *destination;
@property(nonatomic) NSUInteger destinationOffset;
@end

@implementation InfernoMetalArgumentEncoder

static void argumentInvalid(NSString *message)
{
    [NSException raise:InfernoMetalInvalidUseException format:@"%@", message];
}

- (instancetype)initWithLayout:(InfernoMetalArgumentLayout *)layout
                       context:(InfernoMetalCompilerContext *)context
                        device:(id<MTLDevice>)device
{
    if (!(self = [super init]) || !layout || !context || !device)
        return nil;
    _layout = layout;
    _context = context;
    _storedDevice = device;
    return self;
}
- (NSUInteger)encodedLength
{
    return _layout.encodedLength;
}
- (NSUInteger)alignment
{
    return _layout.alignment;
}
- (id<MTLDevice>)device
{
    return _storedDevice;
}
- (NSString *)label
{
    return self.storedLabel;
}
- (void)setLabel:(NSString *)label
{
    self.storedLabel = label;
}
- (void)setArgumentBuffer:(id<MTLBuffer>)argumentBuffer
                   offset:(NSUInteger)offset
{
    if (!argumentBuffer) {
        _destination = nil;
        _destinationOffset = 0;
        return;
    }
    if (![argumentBuffer isKindOfClass:[InfernoMetalBuffer class]] ||
        ((InfernoMetalBuffer *)argumentBuffer).infernoContext != _context ||
        (_layout.alignment && offset % _layout.alignment) ||
        offset > argumentBuffer.length ||
        _layout.encodedLength > argumentBuffer.length - offset)
        argumentInvalid(@"The argument destination is foreign, misaligned, or "
                        @"out of range");
    _destination = (InfernoMetalBuffer *)argumentBuffer;
    _destinationOffset = offset;
}
- (void)setArgumentBuffer:(id<MTLBuffer>)argumentBuffer
              startOffset:(NSUInteger)startOffset
             arrayElement:(NSUInteger)arrayElement
{
    (void)argumentBuffer;
    (void)startOffset;
    (void)arrayElement;
    [self unsupported:@"Argument-buffer array elements are unsupported"];
}
- (InfernoMetalArgumentMemberLayout *)requireMember:(NSUInteger)index
                                               kind:(uint32_t)kind
{
    if (!_destination)
        argumentInvalid(@"An argument destination must be selected first");
    InfernoMetalArgumentMemberLayout *member = [_layout memberForIndex:index];
    if (!member || member.record.kind != kind)
        argumentInvalid([NSString
            stringWithFormat:@"Argument member %lu has the wrong kind",
                             (unsigned long)index]);
    return member;
}
- (void)validateResource:(id)resource
                   index:(NSUInteger)index
                    kind:(uint32_t)kind
                  offset:(NSUInteger)offset
{
    InfernoMetalArgumentMemberLayout *member =
        [self requireMember:index kind:kind];
    if (!resource)
        return;
    BOOL valid = NO;
    if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER) {
        valid = [resource isKindOfClass:[InfernoMetalBuffer class]] &&
                ((InfernoMetalBuffer *)resource).infernoContext == _context &&
                offset < ((InfernoMetalBuffer *)resource).length &&
                !(offset & 3);
    } else if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE) {
        valid = [resource isKindOfClass:[InfernoMetalTexture class]] &&
                ((InfernoMetalTexture *)resource).infernoContext == _context &&
                ((InfernoMetalTexture *)resource).textureType ==
                    member.record.texture_type &&
                !member.record.depth &&
                (((InfernoMetalTexture *)resource).usage &
                 MTLTextureUsageShaderRead);
    } else if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER) {
        valid =
            [resource isKindOfClass:[InfernoMetalSamplerState class]] &&
            ((InfernoMetalSamplerState *)resource).infernoContext == _context &&
            ((InfernoMetalSamplerState *)resource)
                .infernoSupportsArgumentBuffers;
    }
    if (!valid)
        argumentInvalid([NSString
            stringWithFormat:@"Argument member %lu uses an invalid resource",
                             (unsigned long)index]);
}
- (InfernoMetalArgumentRegion *)materializedRegion
{
    return [_destination infernoMaterializeArgumentLayout:_layout
                                                   offset:_destinationOffset];
}
- (void)applyResources:(NSArray *)resources
               offsets:(NSArray<NSNumber *> *)offsets
               indexes:(NSArray<NSNumber *> *)indexes
{
    [_destination infernoApplyArgumentLayout:_layout
                                      offset:_destinationOffset
                                   resources:resources
                                     offsets:offsets
                                     indexes:indexes];
}
- (void)setBuffer:(id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
          atIndex:(NSUInteger)index
{
    [self validateResource:buffer
                     index:index
                      kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER
                    offset:offset];
    [self applyResources:@[ (id)buffer ?: [NSNull null] ]
                 offsets:@[ @(offset) ]
                 indexes:@[ @(index) ]];
}
- (void)setBuffers:(const id<MTLBuffer> __nullable __unsafe_unretained[])buffers
           offsets:(const NSUInteger[])offsets
         withRange:(NSRange)range
{
    if (!buffers || !offsets || range.location > NSUIntegerMax - range.length)
        argumentInvalid(@"The argument buffer range is invalid");
    for (NSUInteger i = 0; i < range.length; i++)
        [self validateResource:buffers[i]
                         index:range.location + i
                          kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER
                        offset:offsets[i]];
    NSMutableArray *values = [NSMutableArray arrayWithCapacity:range.length];
    NSMutableArray *valueOffsets =
        [NSMutableArray arrayWithCapacity:range.length];
    NSMutableArray *indexes = [NSMutableArray arrayWithCapacity:range.length];
    for (NSUInteger i = 0; i < range.length; i++) {
        [values addObject:(id)buffers[i] ?: [NSNull null]];
        [valueOffsets addObject:@(offsets[i])];
        [indexes addObject:@(range.location + i)];
    }
    [self applyResources:values offsets:valueOffsets indexes:indexes];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self validateResource:texture
                     index:index
                      kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE
                    offset:0];
    [self applyResources:@[ (id)texture ?: [NSNull null] ]
                 offsets:@[ @0 ]
                 indexes:@[ @(index) ]];
}
- (void)setTextures:
            (const id<MTLTexture> __nullable __unsafe_unretained[])textures
          withRange:(NSRange)range
{
    if (!textures || range.location > NSUIntegerMax - range.length)
        argumentInvalid(@"The argument texture range is invalid");
    for (NSUInteger i = 0; i < range.length; i++)
        [self validateResource:textures[i]
                         index:range.location + i
                          kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE
                        offset:0];
    NSMutableArray *values = [NSMutableArray arrayWithCapacity:range.length];
    NSMutableArray *valueOffsets =
        [NSMutableArray arrayWithCapacity:range.length];
    NSMutableArray *indexes = [NSMutableArray arrayWithCapacity:range.length];
    for (NSUInteger i = 0; i < range.length; i++) {
        [values addObject:(id)textures[i] ?: [NSNull null]];
        [valueOffsets addObject:@0];
        [indexes addObject:@(range.location + i)];
    }
    [self applyResources:values offsets:valueOffsets indexes:indexes];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self validateResource:sampler
                     index:index
                      kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER
                    offset:0];
    [self applyResources:@[ (id)sampler ?: [NSNull null] ]
                 offsets:@[ @0 ]
                 indexes:@[ @(index) ]];
}
- (void)setSamplerStates:
            (const id<MTLSamplerState> __nullable __unsafe_unretained[])samplers
               withRange:(NSRange)range
{
    if (!samplers || range.location > NSUIntegerMax - range.length)
        argumentInvalid(@"The argument sampler range is invalid");
    for (NSUInteger i = 0; i < range.length; i++)
        [self validateResource:samplers[i]
                         index:range.location + i
                          kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER
                        offset:0];
    NSMutableArray *values = [NSMutableArray arrayWithCapacity:range.length];
    NSMutableArray *valueOffsets =
        [NSMutableArray arrayWithCapacity:range.length];
    NSMutableArray *indexes = [NSMutableArray arrayWithCapacity:range.length];
    for (NSUInteger i = 0; i < range.length; i++) {
        [values addObject:(id)samplers[i] ?: [NSNull null]];
        [valueOffsets addObject:@0];
        [indexes addObject:@(range.location + i)];
    }
    [self applyResources:values offsets:valueOffsets indexes:indexes];
}
- (void *)constantDataAtIndex:(NSUInteger)index
{
    InfernoMetalArgumentMemberLayout *member =
        [self requireMember:index
                       kind:INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT];
    (void)[self materializedRegion];
    return (uint8_t *)_destination.contents + _destinationOffset +
           member.record.byte_offset;
}
- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
    (void)index;
    [self unsupported:@"Nested argument buffers are unsupported"];
    return nil;
}
- (void)unsupported:(NSString *)message
{
    [NSException raise:InfernoMetalUnsupportedException format:@"%@", message];
}

#define INFERNO_UNSUPPORTED_ONE(Selector, Type)                         \
    -(void)Selector : (Type)value atIndex : (NSUInteger)index           \
    {                                                                   \
        (void)value;                                                    \
        (void)index;                                                    \
        [self unsupported:@"This argument member type is unsupported"]; \
    }
#define INFERNO_UNSUPPORTED_MANY(Selector, Type)                        \
    -(void)Selector                                                     \
        : (const Type __nullable __unsafe_unretained[])values withRange \
        : (NSRange)range                                                \
    {                                                                   \
        (void)values;                                                   \
        (void)range;                                                    \
        [self unsupported:@"This argument member type is unsupported"]; \
    }
INFERNO_UNSUPPORTED_ONE(setComputePipelineState, id<MTLComputePipelineState>)
INFERNO_UNSUPPORTED_MANY(setComputePipelineStates, id<MTLComputePipelineState>)
INFERNO_UNSUPPORTED_ONE(setRenderPipelineState, id<MTLRenderPipelineState>)
INFERNO_UNSUPPORTED_MANY(setRenderPipelineStates, id<MTLRenderPipelineState>)
INFERNO_UNSUPPORTED_ONE(setIndirectCommandBuffer, id<MTLIndirectCommandBuffer>)
INFERNO_UNSUPPORTED_MANY(setIndirectCommandBuffers,
                         id<MTLIndirectCommandBuffer>)
INFERNO_UNSUPPORTED_ONE(setVisibleFunctionTable, id<MTLVisibleFunctionTable>)
INFERNO_UNSUPPORTED_MANY(setVisibleFunctionTables, id<MTLVisibleFunctionTable>)
INFERNO_UNSUPPORTED_ONE(setIntersectionFunctionTable,
                        id<MTLIntersectionFunctionTable>)
INFERNO_UNSUPPORTED_MANY(setIntersectionFunctionTables,
                         id<MTLIntersectionFunctionTable>)
INFERNO_UNSUPPORTED_ONE(setAccelerationStructure, id<MTLAccelerationStructure>)
INFERNO_UNSUPPORTED_ONE(setDepthStencilState, id<MTLDepthStencilState>)
INFERNO_UNSUPPORTED_MANY(setDepthStencilStates, id<MTLDepthStencilState>)
#undef INFERNO_UNSUPPORTED_ONE
#undef INFERNO_UNSUPPORTED_MANY

@end
