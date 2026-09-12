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
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class InfernoMetalCommandBuffer;
@class InfernoMetalRenderPipelineState;
@class InfernoMetalTexture;

NS_ASSUME_NONNULL_BEGIN
    @interface InfernoMetalEncodedDraw : NSObject
    @property(nonatomic, strong) InfernoMetalRenderPipelineState *pipeline;
    @property(nonatomic, copy) NSArray *vertexBindings;
    @property(nonatomic, copy) NSArray *vertexTextureBindings;
    @property(nonatomic, copy) NSArray *vertexSamplerBindings;
    @property(nonatomic, copy) NSArray *fragmentBindings;
    @property(nonatomic, copy) NSArray *fragmentTextureBindings;
    @property(nonatomic, copy) NSArray *fragmentSamplerBindings;
    @property(nonatomic) MTLPrimitiveType primitiveType;
    @property(nonatomic) NSUInteger vertexStart;
    @property(nonatomic) NSUInteger vertexCount;
    @property(nonatomic) NSUInteger instanceCount;
    @property(nonatomic) NSUInteger baseInstance;
    @property(nonatomic) MTLCullMode cullMode;
    @property(nonatomic) MTLWinding winding;
    @property(nonatomic) MTLTriangleFillMode fillMode;
    @property(nonatomic) BOOL hasScissor;
    @property(nonatomic) MTLScissorRect scissor;
    @property(nonatomic) BOOL hasViewport;
    @property(nonatomic) MTLViewport viewport;
    @property(nonatomic) BOOL hasBlendColor;
    @property(nonatomic) MTLClearColor blendColor;
    @end

    @interface InfernoMetalEncodedRenderPass : NSObject
    @property(nonatomic, strong) InfernoMetalTexture *attachment;
    @property(nonatomic) MTLLoadAction loadAction;
    @property(nonatomic) MTLStoreAction storeAction;
    @property(nonatomic) MTLClearColor clearColor;
    @property(nonatomic, strong)
        NSMutableArray<InfernoMetalEncodedDraw *> *draws;
    @end

    @interface InfernoMetalRenderCommandEncoder
        : NSObject <MTLRenderCommandEncoder>
    -(nullable instancetype)initWithCommandBuffer
        : (InfernoMetalCommandBuffer *)commandBuffer device
        : (id<MTLDevice>)device descriptor
        : (MTLRenderPassDescriptor *)descriptor;
    @property(nonatomic, readonly) InfernoMetalEncodedRenderPass *infernoPass;
    @end
NS_ASSUME_NONNULL_END
