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
#import "InfernoMetalRenderCommandEncoder.h"
#import "InfernoMetalBuffer.h"
#import "InfernoMetalCommandBuffer.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalRenderPipelineState.h"
#import "InfernoMetalSamplerState.h"
#import "InfernoMetalTexture.h"

#include "standard-headers/inferno/metal.h"
#include <math.h>

@implementation InfernoMetalEncodedDraw
@end
@implementation InfernoMetalEncodedRenderPass
@end

@interface InfernoMetalRenderCommandEncoder ()
@property(nonatomic, strong, nullable) InfernoMetalCommandBuffer *commandBuffer;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, strong) InfernoMetalEncodedRenderPass *storedPass;
@property(nonatomic, strong, nullable)
    InfernoMetalRenderPipelineState *pipeline;
@property(nonatomic, strong) NSMutableArray *vertexBufferSlots;
@property(nonatomic, strong) NSMutableArray *vertexTextureSlots;
@property(nonatomic, strong) NSMutableArray *vertexSamplerSlots;
@property(nonatomic, strong) NSMutableArray *fragmentBufferSlots;
@property(nonatomic, strong) NSMutableArray *fragmentTextureSlots;
@property(nonatomic, strong) NSMutableArray *fragmentSamplerSlots;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@property(nonatomic) MTLCullMode cullMode;
@property(nonatomic) MTLWinding winding;
@property(nonatomic) MTLTriangleFillMode fillMode;
@property(nonatomic) BOOL hasScissor;
@property(nonatomic) MTLScissorRect scissor;
@property(nonatomic) BOOL hasViewport;
@property(nonatomic) MTLViewport viewport;
@property(nonatomic) BOOL hasBlendColor;
@property(nonatomic) MTLClearColor blendColor;
@property(nonatomic) BOOL ended;
@end

@implementation InfernoMetalRenderCommandEncoder

static void renderInvalid(NSString *message)
{
    [NSException raise:InfernoMetalInvalidUseException format:@"%@", message];
}

static BOOL emptyPassAttachment(MTLRenderPassAttachmentDescriptor *attachment)
{
    return !attachment.texture && !attachment.resolveTexture;
}

static BOOL finiteColor(MTLClearColor color)
{
    return isfinite(color.red) && isfinite(color.green) &&
           isfinite(color.blue) && isfinite(color.alpha);
}

- (instancetype)initWithCommandBuffer:(InfernoMetalCommandBuffer *)buffer
                               device:(id<MTLDevice>)device
                           descriptor:(MTLRenderPassDescriptor *)descriptor
{
    MTLRenderPassColorAttachmentDescriptor *color =
        descriptor.colorAttachments[0];
    if (!(self = [super init]) || !buffer || !device || !descriptor ||
        ![color.texture isKindOfClass:[InfernoMetalTexture class]] ||
        ((InfernoMetalTexture *)color.texture).infernoContext !=
            buffer.infernoContext ||
        !(color.texture.usage & MTLTextureUsageRenderTarget) || color.level ||
        color.slice || color.depthPlane || color.resolveTexture ||
        color.loadAction > MTLLoadActionClear ||
        color.storeAction != MTLStoreActionStore ||
        color.storeActionOptions != MTLStoreActionOptionNone ||
        !finiteColor(color.clearColor))
        return nil;
    for (NSUInteger i = 1; i < 8; i++) {
        if (!emptyPassAttachment(descriptor.colorAttachments[i]))
            return nil;
    }
    if (!emptyPassAttachment(descriptor.depthAttachment) ||
        !emptyPassAttachment(descriptor.stencilAttachment) ||
        descriptor.visibilityResultBuffer ||
        descriptor.renderTargetArrayLength ||
        descriptor.imageblockSampleLength ||
        descriptor.threadgroupMemoryLength || descriptor.tileWidth ||
        descriptor.tileHeight || descriptor.defaultRasterSampleCount ||
        descriptor.renderTargetWidth || descriptor.renderTargetHeight ||
        descriptor.rasterizationRateMap ||
        descriptor.visibilityResultType != MTLVisibilityResultTypeReset ||
        descriptor.supportColorAttachmentMapping ||
        [descriptor getSamplePositions:NULL count:0])
        return nil;
    for (NSUInteger i = 0; i < 4; i++) {
        if (descriptor.sampleBufferAttachments[i].sampleBuffer)
            return nil;
    }
    _commandBuffer = buffer;
    _storedDevice = device;
    _storedPass = [[InfernoMetalEncodedRenderPass alloc] init];
    _storedPass.attachment = (id)color.texture;
    _storedPass.loadAction = color.loadAction;
    _storedPass.storeAction = color.storeAction;
    _storedPass.clearColor = color.clearColor;
    _storedPass.draws = [NSMutableArray array];
    _vertexBufferSlots = [NSMutableArray arrayWithCapacity:31];
    _vertexTextureSlots = [NSMutableArray arrayWithCapacity:31];
    _fragmentBufferSlots = [NSMutableArray arrayWithCapacity:31];
    _fragmentTextureSlots = [NSMutableArray arrayWithCapacity:31];
    _vertexSamplerSlots = [NSMutableArray arrayWithCapacity:16];
    _fragmentSamplerSlots = [NSMutableArray arrayWithCapacity:16];
    for (NSUInteger i = 0; i < 31; i++) {
        [_vertexBufferSlots addObject:[NSNull null]];
        [_vertexTextureSlots addObject:[NSNull null]];
        [_fragmentBufferSlots addObject:[NSNull null]];
        [_fragmentTextureSlots addObject:[NSNull null]];
    }
    for (NSUInteger i = 0; i < 16; i++) {
        [_vertexSamplerSlots addObject:[NSNull null]];
        [_fragmentSamplerSlots addObject:[NSNull null]];
    }
    _cullMode = MTLCullModeNone;
    _winding = MTLWindingClockwise;
    _fillMode = MTLTriangleFillModeFill;
    return self;
}

- (void)dealloc
{
    if (!_ended && _commandBuffer)
        [_commandBuffer infernoEncoderEnded:self];
}
- (InfernoMetalEncodedRenderPass *)infernoPass
{
    return _storedPass;
}
- (void)requireActive
{
    if (_ended || !_commandBuffer)
        renderInvalid(@"The render encoder has ended");
}
- (void)unsupported
{
    [self requireActive];
    [NSException raise:InfernoMetalUnsupportedException
                format:@"This render encoder operation is unsupported"];
}
- (id<MTLDevice>)device
{
    return _storedDevice;
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
    [self unsupported];
}
- (void)endEncoding
{
    [self requireActive];
    InfernoMetalCommandBuffer *buffer = _commandBuffer;
    _ended = YES;
    _commandBuffer = nil;
    [buffer infernoEncoderEnded:self];
}
- (NSUInteger)tileWidth
{
    [self unsupported];
    return 0;
}
- (NSUInteger)tileHeight
{
    [self unsupported];
    return 0;
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline
{
    [self requireActive];
    if (![pipeline isKindOfClass:[InfernoMetalRenderPipelineState class]] ||
        ((InfernoMetalRenderPipelineState *)pipeline).infernoContext !=
            _commandBuffer.infernoContext)
        renderInvalid(@"The render pipeline belongs to another Metal context");
    _pipeline = (id)pipeline;
}

- (void)requireBufferIndex:(NSUInteger)index
{
    if (index > 30)
        renderInvalid(@"A buffer binding index must be at most 30");
}
- (void)requireTextureIndex:(NSUInteger)index
{
    if (index > 30)
        renderInvalid(@"A texture binding index must be at most 30");
}
- (void)requireSamplerIndex:(NSUInteger)index
{
    if (index > 15)
        renderInvalid(@"A sampler binding index must be at most 15");
}
- (void)setBytes:(const void *)bytes
          length:(NSUInteger)length
           index:(NSUInteger)index
           slots:(NSMutableArray *)slots
{
    [self requireActive];
    [self requireBufferIndex:index];
    if (!bytes || !length || length > INFERNO_METAL_RESOURCE_MAX_INLINE_BINDING)
        renderInvalid(@"Inline buffer data must contain 1 through 4096 bytes");
    slots[index] = @{
        @"kind" : @(INFERNO_METAL_RESOURCE_BINDING_INLINE),
        @"data" : [NSData dataWithBytes:bytes length:length]
    };
}
- (void)setBuffer:(id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
            index:(NSUInteger)index
            slots:(NSMutableArray *)slots
{
    [self requireActive];
    [self requireBufferIndex:index];
    if (!buffer) {
        slots[index] = [NSNull null];
        return;
    }
    if (![buffer isKindOfClass:[InfernoMetalBuffer class]] ||
        ((InfernoMetalBuffer *)buffer).infernoContext !=
            _commandBuffer.infernoContext ||
        (offset & 3) || offset >= buffer.length)
        renderInvalid(
            @"The buffer binding is foreign, misaligned, or out of range");
    slots[index] = @{
        @"kind" : @(INFERNO_METAL_RESOURCE_BINDING_BUFFER),
        @"buffer" : buffer,
        @"offset" : @(offset)
    };
}
- (void)setBufferOffset:(NSUInteger)offset
                  index:(NSUInteger)index
                  slots:(NSMutableArray *)slots
{
    [self requireActive];
    [self requireBufferIndex:index];
    id value = slots[index];
    if (![value isKindOfClass:[NSDictionary class]] ||
        [value[@"kind"] unsignedIntValue] !=
            INFERNO_METAL_RESOURCE_BINDING_BUFFER)
        renderInvalid(@"setBufferOffset requires an existing buffer binding");
    [self setBuffer:value[@"buffer"] offset:offset index:index slots:slots];
}
- (void)setTexture:(id<MTLTexture>)texture
             index:(NSUInteger)index
             slots:(NSMutableArray *)slots
{
    [self requireActive];
    [self requireTextureIndex:index];
    if (!texture) {
        slots[index] = [NSNull null];
        return;
    }
    if (![texture isKindOfClass:[InfernoMetalTexture class]] ||
        ((InfernoMetalTexture *)texture).infernoContext !=
            _commandBuffer.infernoContext ||
        !(texture.usage &
          (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)))
        renderInvalid(@"The texture binding is foreign or lacks shader usage");
    slots[index] = @{
        @"kind" : @(INFERNO_METAL_RESOURCE_BINDING_TEXTURE),
        @"texture" : texture
    };
}
- (void)setSampler:(id<MTLSamplerState>)sampler
             index:(NSUInteger)index
             slots:(NSMutableArray *)slots
{
    [self requireActive];
    [self requireSamplerIndex:index];
    if (!sampler) {
        slots[index] = [NSNull null];
        return;
    }
    if (![sampler isKindOfClass:[InfernoMetalSamplerState class]] ||
        ((InfernoMetalSamplerState *)sampler).infernoContext !=
            _commandBuffer.infernoContext)
        renderInvalid(@"The sampler binding belongs to another Metal context");
    slots[index] = @{
        @"kind" : @(INFERNO_METAL_RESOURCE_BINDING_SAMPLER),
        @"sampler" : sampler
    };
}
#define STAGE_METHODS(Prefix, BufferSlots, TextureSlots, SamplerSlots)        \
    -(void)set##Prefix##Bytes : (const void *)bytes length                    \
        : (NSUInteger)length atIndex : (NSUInteger)index                      \
    {                                                                         \
        [self setBytes:bytes length:length index:index slots:BufferSlots];    \
    }                                                                         \
    -(void)set##Prefix##Buffer : (id<MTLBuffer>)buffer offset                 \
        : (NSUInteger)offset atIndex : (NSUInteger)index                      \
    {                                                                         \
        [self setBuffer:buffer offset:offset index:index slots:BufferSlots];  \
    }                                                                         \
    -(void)set##Prefix##BufferOffset : (NSUInteger)offset atIndex             \
        : (NSUInteger)index                                                   \
    {                                                                         \
        [self setBufferOffset:offset index:index slots:BufferSlots];          \
    }                                                                         \
    -(void)set##Prefix##Buffers : (const id<MTLBuffer>[])buffers offsets      \
        : (const NSUInteger[])offsets withRange : (NSRange)range              \
    {                                                                         \
        [self requireActive];                                                 \
        if (!buffers || !offsets || range.location > 31 ||                    \
            range.length > 31 - range.location)                               \
            renderInvalid(@"The buffer binding range is invalid");            \
        for (NSUInteger i = 0; i < range.length; i++)                         \
            [self setBuffer:buffers[i]                                        \
                     offset:offsets[i]                                        \
                      index:range.location + i                                \
                      slots:BufferSlots];                                     \
    }                                                                         \
    -(void)set##Prefix##Texture : (id<MTLTexture>)texture atIndex             \
        : (NSUInteger)index                                                   \
    {                                                                         \
        [self setTexture:texture index:index slots:TextureSlots];             \
    }                                                                         \
    -(void)set##Prefix##Textures : (const id<MTLTexture>[])textures withRange \
        : (NSRange)range                                                      \
    {                                                                         \
        [self requireActive];                                                 \
        if (!textures || range.location > 31 ||                               \
            range.length > 31 - range.location)                               \
            renderInvalid(@"The texture binding range is invalid");           \
        for (NSUInteger i = 0; i < range.length; i++)                         \
            [self setTexture:textures[i]                                      \
                       index:range.location + i                               \
                       slots:TextureSlots];                                   \
    }                                                                         \
    -(void)set##Prefix##SamplerState : (id<MTLSamplerState>)sampler atIndex   \
        : (NSUInteger)index                                                   \
    {                                                                         \
        [self setSampler:sampler index:index slots:SamplerSlots];             \
    }                                                                         \
    -(void)set##Prefix##SamplerStates                                         \
        : (const id<MTLSamplerState>[])samplers withRange : (NSRange)range    \
    {                                                                         \
        [self requireActive];                                                 \
        if (!samplers || range.location > 16 ||                               \
            range.length > 16 - range.location)                               \
            renderInvalid(@"The sampler binding range is invalid");           \
        for (NSUInteger i = 0; i < range.length; i++)                         \
            [self setSampler:samplers[i]                                      \
                       index:range.location + i                               \
                       slots:SamplerSlots];                                   \
    }
STAGE_METHODS(Vertex, _vertexBufferSlots, _vertexTextureSlots,
              _vertexSamplerSlots)
STAGE_METHODS(Fragment, _fragmentBufferSlots, _fragmentTextureSlots,
              _fragmentSamplerSlots)

- (void)setViewport:(MTLViewport)value
{
    [self requireActive];
    if (!isfinite(value.originX) || !isfinite(value.originY) ||
        !isfinite(value.width) || !isfinite(value.height) ||
        !isfinite(value.znear) || !isfinite(value.zfar) || value.originX < 0 ||
        value.originY < 0 || value.width < 0 || value.height < 0 ||
        value.originX + value.width > _storedPass.attachment.width ||
        value.originY + value.height > _storedPass.attachment.height ||
        value.znear < 0 || value.znear > value.zfar || value.zfar > 1)
        renderInvalid(@"The viewport is invalid or outside the attachment");
    _viewport = value;
    _hasViewport = YES;
}
- (void)setScissorRect:(MTLScissorRect)value
{
    [self requireActive];
    if (!value.width || !value.height ||
        value.x > _storedPass.attachment.width ||
        value.width > _storedPass.attachment.width - value.x ||
        value.y > _storedPass.attachment.height ||
        value.height > _storedPass.attachment.height - value.y)
        renderInvalid(@"The scissor rectangle is outside the attachment");
    _scissor = value;
    _hasScissor = YES;
}
- (void)setCullMode:(MTLCullMode)value
{
    [self requireActive];
    if (value > MTLCullModeBack)
        renderInvalid(@"Invalid cull mode");
    _cullMode = value;
}
- (void)setFrontFacingWinding:(MTLWinding)value
{
    [self requireActive];
    if (value > MTLWindingCounterClockwise)
        renderInvalid(@"Invalid winding");
    _winding = value;
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)value
{
    [self requireActive];
    if (value > MTLTriangleFillModeLines)
        renderInvalid(@"Invalid fill mode");
    _fillMode = value;
}
- (void)setBlendColorRed:(float)red
                   green:(float)green
                    blue:(float)blue
                   alpha:(float)alpha
{
    [self requireActive];
    if (!isfinite(red) || !isfinite(green) || !isfinite(blue) ||
        !isfinite(alpha))
        renderInvalid(@"Blend color must be finite");
    _blendColor = MTLClearColorMake(red, green, blue, alpha);
    _hasBlendColor = YES;
}
- (void)recordDraw:(MTLPrimitiveType)type
             start:(NSUInteger)start
             count:(NSUInteger)count
         instances:(NSUInteger)instances
              base:(NSUInteger)base
{
    [self requireActive];
    if (!_pipeline || type > MTLPrimitiveTypeTriangleStrip || !count ||
        !instances || base || count > INFERNO_METAL_MAX_THREADS ||
        instances > INFERNO_METAL_MAX_THREADS ||
        count > INFERNO_METAL_MAX_THREADS / instances)
        renderInvalid(@"The draw parameters or pipeline are invalid");
    for (id slots in @[ _vertexTextureSlots, _fragmentTextureSlots ])
        for (id value in slots) {
            if ([value isKindOfClass:[NSDictionary class]] &&
                [value[@"kind"] unsignedIntValue] ==
                    INFERNO_METAL_RESOURCE_BINDING_TEXTURE &&
                value[@"texture"] == _storedPass.attachment)
                renderInvalid(
                    @"A render attachment cannot be sampled in the same pass");
        }
    InfernoMetalEncodedDraw *draw = [[InfernoMetalEncodedDraw alloc] init];
    draw.pipeline = _pipeline;
    draw.vertexBindings = [_vertexBufferSlots copy];
    draw.vertexTextureBindings = [_vertexTextureSlots copy];
    draw.vertexSamplerBindings = [_vertexSamplerSlots copy];
    draw.fragmentBindings = [_fragmentBufferSlots copy];
    draw.fragmentTextureBindings = [_fragmentTextureSlots copy];
    draw.fragmentSamplerBindings = [_fragmentSamplerSlots copy];
    draw.primitiveType = type;
    draw.vertexStart = start;
    draw.vertexCount = count;
    draw.instanceCount = instances;
    draw.baseInstance = base;
    draw.cullMode = _cullMode;
    draw.winding = _winding;
    draw.fillMode = _fillMode;
    draw.hasScissor = _hasScissor;
    draw.scissor = _scissor;
    draw.hasViewport = _hasViewport;
    draw.viewport = _viewport;
    draw.hasBlendColor = _hasBlendColor;
    draw.blendColor = _blendColor;
    [_storedPass.draws addObject:draw];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
{
    [self recordDraw:type start:start count:count instances:1 base:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
{
    [self recordDraw:type start:start count:count instances:instances base:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)base
{
    [self recordDraw:type
               start:start
               count:count
           instances:instances
                base:base];
}
- (void)textureBarrier
{
    [self unsupported];
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (void)dispatchThreadsPerTile:(MTLSize)threadsPerTile
{
    (void)threadsPerTile;
    [self unsupported];
}

- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints
                 patchIndexBuffer:
                     (id<MTLBuffer> _Nullable __strong)patchIndexBuffer
           patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
          controlPointIndexBuffer:
              (id<MTLBuffer> _Nonnull __strong)controlPointIndexBuffer
    controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset
                   indirectBuffer:
                       (id<MTLBuffer> _Nonnull __strong)indirectBuffer
             indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
    (void)numberOfPatchControlPoints;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)controlPointIndexBuffer;
    (void)controlPointIndexBufferOffset;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    [self unsupported];
}

- (void)drawIndexedPatches:(NSUInteger)numberOfPatchControlPoints
                       patchStart:(NSUInteger)patchStart
                       patchCount:(NSUInteger)patchCount
                 patchIndexBuffer:
                     (id<MTLBuffer> _Nullable __strong)patchIndexBuffer
           patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
          controlPointIndexBuffer:
              (id<MTLBuffer> _Nonnull __strong)controlPointIndexBuffer
    controlPointIndexBufferOffset:(NSUInteger)controlPointIndexBufferOffset
                    instanceCount:(NSUInteger)instanceCount
                     baseInstance:(NSUInteger)baseInstance
{
    (void)numberOfPatchControlPoints;
    (void)patchStart;
    (void)patchCount;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)controlPointIndexBuffer;
    (void)controlPointIndexBufferOffset;
    (void)instanceCount;
    (void)baseInstance;
    [self unsupported];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer> _Nonnull __strong)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
{
    (void)primitiveType;
    (void)indexCount;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    [self unsupported];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer> _Nonnull __strong)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
                instanceCount:(NSUInteger)instanceCount
{
    (void)primitiveType;
    (void)indexCount;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    (void)instanceCount;
    [self unsupported];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                   indexCount:(NSUInteger)indexCount
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer> _Nonnull __strong)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
                instanceCount:(NSUInteger)instanceCount
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance
{
    (void)primitiveType;
    (void)indexCount;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    (void)instanceCount;
    (void)baseVertex;
    (void)baseInstance;
    [self unsupported];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)primitiveType
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer> _Nonnull __strong)indexBuffer
            indexBufferOffset:(NSUInteger)indexBufferOffset
               indirectBuffer:(id<MTLBuffer> _Nonnull __strong)indirectBuffer
         indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
    (void)primitiveType;
    (void)indexType;
    (void)indexBuffer;
    (void)indexBufferOffset;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    [self unsupported];
}

- (void)drawMeshThreadgroups:(MTLSize)threadgroupsPerGrid
    threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
      threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup
{
    (void)threadgroupsPerGrid;
    (void)threadsPerObjectThreadgroup;
    (void)threadsPerMeshThreadgroup;
    [self unsupported];
}

- (void)
    drawMeshThreadgroupsWithIndirectBuffer:
        (id<MTLBuffer> _Nonnull __strong)indirectBuffer
                      indirectBufferOffset:(NSUInteger)indirectBufferOffset
               threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
                 threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup
{
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    (void)threadsPerObjectThreadgroup;
    (void)threadsPerMeshThreadgroup;
    [self unsupported];
}

- (void)drawMeshThreads:(MTLSize)threadsPerGrid
    threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
      threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup
{
    (void)threadsPerGrid;
    (void)threadsPerObjectThreadgroup;
    (void)threadsPerMeshThreadgroup;
    [self unsupported];
}

- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints
          patchIndexBuffer:(id<MTLBuffer> _Nullable __strong)patchIndexBuffer
    patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
            indirectBuffer:(id<MTLBuffer> _Nonnull __strong)indirectBuffer
      indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
    (void)numberOfPatchControlPoints;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    [self unsupported];
}

- (void)drawPatches:(NSUInteger)numberOfPatchControlPoints
                patchStart:(NSUInteger)patchStart
                patchCount:(NSUInteger)patchCount
          patchIndexBuffer:(id<MTLBuffer> _Nullable __strong)patchIndexBuffer
    patchIndexBufferOffset:(NSUInteger)patchIndexBufferOffset
             instanceCount:(NSUInteger)instanceCount
              baseInstance:(NSUInteger)baseInstance
{
    (void)numberOfPatchControlPoints;
    (void)patchStart;
    (void)patchCount;
    (void)patchIndexBuffer;
    (void)patchIndexBufferOffset;
    (void)instanceCount;
    (void)baseInstance;
    [self unsupported];
}

- (void)drawPrimitives:(MTLPrimitiveType)primitiveType
          indirectBuffer:(id<MTLBuffer> _Nonnull __strong)indirectBuffer
    indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
    (void)primitiveType;
    (void)indirectBuffer;
    (void)indirectBufferOffset;
    [self unsupported];
}

- (void)
    executeCommandsInBuffer:
        (id<MTLIndirectCommandBuffer> _Nonnull __strong)indirectCommandbuffer
             indirectBuffer:(id<MTLBuffer> _Nonnull __strong)indirectRangeBuffer
       indirectBufferOffset:(NSUInteger)indirectBufferOffset
{
    (void)indirectCommandbuffer;
    (void)indirectRangeBuffer;
    (void)indirectBufferOffset;
    [self unsupported];
}

- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer> _Nonnull __strong)
                                    indirectCommandBuffer
                      withRange:(NSRange)executionRange
{
    (void)indirectCommandBuffer;
    (void)executionRange;
    [self unsupported];
}

- (void)memoryBarrierWithResources:
            (id<MTLResource> _Nonnull const __unsafe_unretained *_Nonnull)
                resources
                             count:(NSUInteger)count
                       afterStages:(MTLRenderStages)after
                      beforeStages:(MTLRenderStages)before
{
    (void)resources;
    (void)count;
    (void)after;
    (void)before;
    [self unsupported];
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
                   afterStages:(MTLRenderStages)after
                  beforeStages:(MTLRenderStages)before
{
    (void)scope;
    (void)after;
    (void)before;
    [self unsupported];
}

- (void)sampleCountersInBuffer:
            (id<MTLCounterSampleBuffer> _Nonnull __strong)sampleBuffer
                 atSampleIndex:(NSUInteger)sampleIndex
                   withBarrier:(BOOL)barrier
{
    (void)sampleBuffer;
    (void)sampleIndex;
    (void)barrier;
    [self unsupported];
}

- (void)setColorAttachmentMap:
    (MTLLogicalToPhysicalColorAttachmentMap *_Nullable __strong)mapping
{
    (void)mapping;
    [self unsupported];
}

- (void)setColorStoreAction:(MTLStoreAction)storeAction
                    atIndex:(NSUInteger)colorAttachmentIndex
{
    (void)storeAction;
    (void)colorAttachmentIndex;
    [self unsupported];
}

- (void)setColorStoreActionOptions:(MTLStoreActionOptions)storeActionOptions
                           atIndex:(NSUInteger)colorAttachmentIndex
{
    (void)storeActionOptions;
    (void)colorAttachmentIndex;
    [self unsupported];
}

- (void)setDepthBias:(float)depthBias
          slopeScale:(float)slopeScale
               clamp:(float)clamp
{
    (void)depthBias;
    (void)slopeScale;
    (void)clamp;
    [self unsupported];
}

- (void)setDepthClipMode:(MTLDepthClipMode)depthClipMode
{
    (void)depthClipMode;
    [self unsupported];
}

- (void)setDepthStencilState:
    (id<MTLDepthStencilState> _Nullable __strong)depthStencilState
{
    (void)depthStencilState;
    [self unsupported];
}

- (void)setDepthStoreAction:(MTLStoreAction)storeAction
{
    (void)storeAction;
    [self unsupported];
}

- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)storeActionOptions
{
    (void)storeActionOptions;
    [self unsupported];
}

- (void)setDepthTestMinBound:(float)minBound maxBound:(float)maxBound
{
    (void)minBound;
    (void)maxBound;
    [self unsupported];
}

- (void)setFragmentAccelerationStructure:
            (id<MTLAccelerationStructure> _Nullable __strong)
                accelerationStructure
                           atBufferIndex:(NSUInteger)bufferIndex
{
    (void)accelerationStructure;
    (void)bufferIndex;
    [self unsupported];
}

- (void)setFragmentIntersectionFunctionTable:
            (id<MTLIntersectionFunctionTable> _Nullable __strong)
                intersectionFunctionTable
                               atBufferIndex:(NSUInteger)bufferIndex
{
    (void)intersectionFunctionTable;
    (void)bufferIndex;
    [self unsupported];
}

- (void)
    setFragmentIntersectionFunctionTables:
        (id<MTLIntersectionFunctionTable> _Nullable const __unsafe_unretained
             *_Nonnull)intersectionFunctionTables
                          withBufferRange:(NSRange)range
{
    (void)intersectionFunctionTables;
    (void)range;
    [self unsupported];
}

- (void)setFragmentSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                    lodMinClamp:(float)lodMinClamp
                    lodMaxClamp:(float)lodMaxClamp
                        atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    [self unsupported];
}

- (void)setFragmentSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                    lodMinClamps:(const float *_Nonnull)lodMinClamps
                    lodMaxClamps:(const float *_Nonnull)lodMaxClamps
                       withRange:(NSRange)range
{
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    [self unsupported];
}

- (void)setFragmentVisibleFunctionTable:
            (id<MTLVisibleFunctionTable> _Nullable __strong)functionTable
                          atBufferIndex:(NSUInteger)bufferIndex
{
    (void)functionTable;
    (void)bufferIndex;
    [self unsupported];
}

- (void)setFragmentVisibleFunctionTables:
            (id<MTLVisibleFunctionTable> _Nullable const __unsafe_unretained
                 *_Nonnull)functionTables
                         withBufferRange:(NSRange)range
{
    (void)functionTables;
    (void)range;
    [self unsupported];
}

- (void)setMeshBuffer:(id<MTLBuffer> _Nullable __strong)buffer
               offset:(NSUInteger)offset
              atIndex:(NSUInteger)index
{
    (void)buffer;
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setMeshBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setMeshBuffers:
            (id<MTLBuffer> _Nullable const __unsafe_unretained *_Nonnull)buffers
               offsets:(const NSUInteger *_Nonnull)offsets
             withRange:(NSRange)range
{
    (void)buffers;
    (void)offsets;
    (void)range;
    [self unsupported];
}

- (void)setMeshBytes:(const void *_Nonnull)bytes
              length:(NSUInteger)length
             atIndex:(NSUInteger)index
{
    (void)bytes;
    (void)length;
    (void)index;
    [self unsupported];
}

- (void)setMeshSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                    atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)index;
    [self unsupported];
}

- (void)setMeshSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                lodMinClamp:(float)lodMinClamp
                lodMaxClamp:(float)lodMaxClamp
                    atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    [self unsupported];
}

- (void)setMeshSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                lodMinClamps:(const float *_Nonnull)lodMinClamps
                lodMaxClamps:(const float *_Nonnull)lodMaxClamps
                   withRange:(NSRange)range
{
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    [self unsupported];
}

- (void)setMeshSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                   withRange:(NSRange)range
{
    (void)samplers;
    (void)range;
    [self unsupported];
}

- (void)setMeshTexture:(id<MTLTexture> _Nullable __strong)texture
               atIndex:(NSUInteger)index
{
    (void)texture;
    (void)index;
    [self unsupported];
}

- (void)setMeshTextures:
            (id<MTLTexture> _Nullable const __unsafe_unretained *_Nonnull)
                textures
              withRange:(NSRange)range
{
    (void)textures;
    (void)range;
    [self unsupported];
}

- (void)setObjectBuffer:(id<MTLBuffer> _Nullable __strong)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index
{
    (void)buffer;
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setObjectBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setObjectBuffers:
            (id<MTLBuffer> _Nullable const __unsafe_unretained *_Nonnull)buffers
                 offsets:(const NSUInteger *_Nonnull)offsets
               withRange:(NSRange)range
{
    (void)buffers;
    (void)offsets;
    (void)range;
    [self unsupported];
}

- (void)setObjectBytes:(const void *_Nonnull)bytes
                length:(NSUInteger)length
               atIndex:(NSUInteger)index
{
    (void)bytes;
    (void)length;
    (void)index;
    [self unsupported];
}

- (void)setObjectSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                      atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)index;
    [self unsupported];
}

- (void)setObjectSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                  lodMinClamp:(float)lodMinClamp
                  lodMaxClamp:(float)lodMaxClamp
                      atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    [self unsupported];
}

- (void)setObjectSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                  lodMinClamps:(const float *_Nonnull)lodMinClamps
                  lodMaxClamps:(const float *_Nonnull)lodMaxClamps
                     withRange:(NSRange)range
{
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    [self unsupported];
}

- (void)setObjectSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                     withRange:(NSRange)range
{
    (void)samplers;
    (void)range;
    [self unsupported];
}

- (void)setObjectTexture:(id<MTLTexture> _Nullable __strong)texture
                 atIndex:(NSUInteger)index
{
    (void)texture;
    (void)index;
    [self unsupported];
}

- (void)setObjectTextures:
            (id<MTLTexture> _Nullable const __unsafe_unretained *_Nonnull)
                textures
                withRange:(NSRange)range
{
    (void)textures;
    (void)range;
    [self unsupported];
}

- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length
                                 atIndex:(NSUInteger)index
{
    (void)length;
    (void)index;
    [self unsupported];
}

- (void)setScissorRects:(const MTLScissorRect *_Nonnull)scissorRects
                  count:(NSUInteger)count
{
    (void)scissorRects;
    (void)count;
    [self unsupported];
}

- (void)setStencilFrontReferenceValue:(uint32_t)frontReferenceValue
                   backReferenceValue:(uint32_t)backReferenceValue
{
    (void)frontReferenceValue;
    (void)backReferenceValue;
    [self unsupported];
}

- (void)setStencilReferenceValue:(uint32_t)referenceValue
{
    (void)referenceValue;
    [self unsupported];
}

- (void)setStencilStoreAction:(MTLStoreAction)storeAction
{
    (void)storeAction;
    [self unsupported];
}

- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)storeActionOptions
{
    (void)storeActionOptions;
    [self unsupported];
}

- (void)setTessellationFactorBuffer:(id<MTLBuffer> _Nullable __strong)buffer
                             offset:(NSUInteger)offset
                     instanceStride:(NSUInteger)instanceStride
{
    (void)buffer;
    (void)offset;
    (void)instanceStride;
    [self unsupported];
}

- (void)setTessellationFactorScale:(float)scale
{
    (void)scale;
    [self unsupported];
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length
                            offset:(NSUInteger)offset
                           atIndex:(NSUInteger)index
{
    (void)length;
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setTileAccelerationStructure:
            (id<MTLAccelerationStructure> _Nullable __strong)
                accelerationStructure
                       atBufferIndex:(NSUInteger)bufferIndex
{
    (void)accelerationStructure;
    (void)bufferIndex;
    [self unsupported];
}

- (void)setTileBuffer:(id<MTLBuffer> _Nullable __strong)buffer
               offset:(NSUInteger)offset
              atIndex:(NSUInteger)index
{
    (void)buffer;
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setTileBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    (void)offset;
    (void)index;
    [self unsupported];
}

- (void)setTileBuffers:
            (id<MTLBuffer> _Nullable const __unsafe_unretained *_Nonnull)buffers
               offsets:(const NSUInteger *_Nonnull)offsets
             withRange:(NSRange)range
{
    (void)buffers;
    (void)offsets;
    (void)range;
    [self unsupported];
}

- (void)setTileBytes:(const void *_Nonnull)bytes
              length:(NSUInteger)length
             atIndex:(NSUInteger)index
{
    (void)bytes;
    (void)length;
    (void)index;
    [self unsupported];
}

- (void)setTileIntersectionFunctionTable:
            (id<MTLIntersectionFunctionTable> _Nullable __strong)
                intersectionFunctionTable
                           atBufferIndex:(NSUInteger)bufferIndex
{
    (void)intersectionFunctionTable;
    (void)bufferIndex;
    [self unsupported];
}

- (void)
    setTileIntersectionFunctionTables:
        (id<MTLIntersectionFunctionTable> _Nullable const __unsafe_unretained
             *_Nonnull)intersectionFunctionTables
                      withBufferRange:(NSRange)range
{
    (void)intersectionFunctionTables;
    (void)range;
    [self unsupported];
}

- (void)setTileSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                    atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)index;
    [self unsupported];
}

- (void)setTileSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                lodMinClamp:(float)lodMinClamp
                lodMaxClamp:(float)lodMaxClamp
                    atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    [self unsupported];
}

- (void)setTileSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                lodMinClamps:(const float *_Nonnull)lodMinClamps
                lodMaxClamps:(const float *_Nonnull)lodMaxClamps
                   withRange:(NSRange)range
{
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    [self unsupported];
}

- (void)setTileSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                   withRange:(NSRange)range
{
    (void)samplers;
    (void)range;
    [self unsupported];
}

- (void)setTileTexture:(id<MTLTexture> _Nullable __strong)texture
               atIndex:(NSUInteger)index
{
    (void)texture;
    (void)index;
    [self unsupported];
}

- (void)setTileTextures:
            (id<MTLTexture> _Nullable const __unsafe_unretained *_Nonnull)
                textures
              withRange:(NSRange)range
{
    (void)textures;
    (void)range;
    [self unsupported];
}

- (void)setTileVisibleFunctionTable:
            (id<MTLVisibleFunctionTable> _Nullable __strong)functionTable
                      atBufferIndex:(NSUInteger)bufferIndex
{
    (void)functionTable;
    (void)bufferIndex;
    [self unsupported];
}

- (void)setTileVisibleFunctionTables:
            (id<MTLVisibleFunctionTable> _Nullable const __unsafe_unretained
                 *_Nonnull)functionTables
                     withBufferRange:(NSRange)range
{
    (void)functionTables;
    (void)range;
    [self unsupported];
}

- (void)setVertexAccelerationStructure:
            (id<MTLAccelerationStructure> _Nullable __strong)
                accelerationStructure
                         atBufferIndex:(NSUInteger)bufferIndex
{
    (void)accelerationStructure;
    (void)bufferIndex;
    [self unsupported];
}

- (void)setVertexAmplificationCount:(NSUInteger)count
                       viewMappings:
                           (const MTLVertexAmplificationViewMapping *_Nullable)
                               viewMappings
{
    (void)count;
    (void)viewMappings;
    [self unsupported];
}

- (void)setVertexBuffer:(id<MTLBuffer> _Nullable __strong)buffer
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

- (void)setVertexBufferOffset:(NSUInteger)offset
              attributeStride:(NSUInteger)stride
                      atIndex:(NSUInteger)index
{
    (void)offset;
    (void)stride;
    (void)index;
    [self unsupported];
}

- (void)setVertexBuffers:
            (id<MTLBuffer> _Nullable const __unsafe_unretained *_Nonnull)buffers
                 offsets:(const NSUInteger *_Nonnull)offsets
        attributeStrides:(const NSUInteger *_Nonnull)strides
               withRange:(NSRange)range
{
    (void)buffers;
    (void)offsets;
    (void)strides;
    (void)range;
    [self unsupported];
}

- (void)setVertexBytes:(const void *_Nonnull)bytes
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

- (void)setVertexIntersectionFunctionTable:
            (id<MTLIntersectionFunctionTable> _Nullable __strong)
                intersectionFunctionTable
                             atBufferIndex:(NSUInteger)bufferIndex
{
    (void)intersectionFunctionTable;
    (void)bufferIndex;
    [self unsupported];
}

- (void)
    setVertexIntersectionFunctionTables:
        (id<MTLIntersectionFunctionTable> _Nullable const __unsafe_unretained
             *_Nonnull)intersectionFunctionTables
                        withBufferRange:(NSRange)range
{
    (void)intersectionFunctionTables;
    (void)range;
    [self unsupported];
}

- (void)setVertexSamplerState:(id<MTLSamplerState> _Nullable __strong)sampler
                  lodMinClamp:(float)lodMinClamp
                  lodMaxClamp:(float)lodMaxClamp
                      atIndex:(NSUInteger)index
{
    (void)sampler;
    (void)lodMinClamp;
    (void)lodMaxClamp;
    (void)index;
    [self unsupported];
}

- (void)setVertexSamplerStates:
            (id<MTLSamplerState> _Nullable const __unsafe_unretained *_Nonnull)
                samplers
                  lodMinClamps:(const float *_Nonnull)lodMinClamps
                  lodMaxClamps:(const float *_Nonnull)lodMaxClamps
                     withRange:(NSRange)range
{
    (void)samplers;
    (void)lodMinClamps;
    (void)lodMaxClamps;
    (void)range;
    [self unsupported];
}

- (void)setVertexVisibleFunctionTable:
            (id<MTLVisibleFunctionTable> _Nullable __strong)functionTable
                        atBufferIndex:(NSUInteger)bufferIndex
{
    (void)functionTable;
    (void)bufferIndex;
    [self unsupported];
}

- (void)setVertexVisibleFunctionTables:
            (id<MTLVisibleFunctionTable> _Nullable const __unsafe_unretained
                 *_Nonnull)functionTables
                       withBufferRange:(NSRange)range
{
    (void)functionTables;
    (void)range;
    [self unsupported];
}

- (void)setViewports:(const MTLViewport *_Nonnull)viewports
               count:(NSUInteger)count
{
    (void)viewports;
    (void)count;
    [self unsupported];
}

- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode
                         offset:(NSUInteger)offset
{
    (void)mode;
    (void)offset;
    [self unsupported];
}

- (void)updateFence:(id<MTLFence> _Nonnull __strong)fence
        afterStages:(MTLRenderStages)stages
{
    (void)fence;
    (void)stages;
    [self unsupported];
}

- (void)useHeap:(id<MTLHeap> _Nonnull __strong)heap
{
    (void)heap;
    [self unsupported];
}

- (void)useHeap:(id<MTLHeap> _Nonnull __strong)heap
         stages:(MTLRenderStages)stages
{
    (void)heap;
    (void)stages;
    [self unsupported];
}

- (void)useHeaps:(id<MTLHeap> _Nonnull const __unsafe_unretained *_Nonnull)heaps
           count:(NSUInteger)count
{
    (void)heaps;
    (void)count;
    [self unsupported];
}

- (void)useHeaps:(id<MTLHeap> _Nonnull const __unsafe_unretained *_Nonnull)heaps
           count:(NSUInteger)count
          stages:(MTLRenderStages)stages
{
    (void)heaps;
    (void)count;
    (void)stages;
    [self unsupported];
}

- (void)useResource:(id<MTLResource> _Nonnull __strong)resource
              usage:(MTLResourceUsage)usage
{
    (void)resource;
    (void)usage;
    [self unsupported];
}

- (void)useResource:(id<MTLResource> _Nonnull __strong)resource
              usage:(MTLResourceUsage)usage
             stages:(MTLRenderStages)stages
{
    (void)resource;
    (void)usage;
    (void)stages;
    [self unsupported];
}

- (void)useResources:
            (id<MTLResource> _Nonnull const __unsafe_unretained *_Nonnull)
                resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
{
    (void)resources;
    (void)count;
    (void)usage;
    [self unsupported];
}

- (void)useResources:
            (id<MTLResource> _Nonnull const __unsafe_unretained *_Nonnull)
                resources
               count:(NSUInteger)count
               usage:(MTLResourceUsage)usage
              stages:(MTLRenderStages)stages
{
    (void)resources;
    (void)count;
    (void)usage;
    (void)stages;
    [self unsupported];
}

- (void)waitForFence:(id<MTLFence> _Nonnull __strong)fence
        beforeStages:(MTLRenderStages)stages
{
    (void)fence;
    (void)stages;
    [self unsupported];
}
#pragma clang diagnostic pop
@end
