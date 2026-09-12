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
#include "../batch-wire.h"
#include "../compiler-client.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@class InfernoMetalCompilerContext;
@class InfernoMetalFunction;
NS_ASSUME_NONNULL_BEGIN
    @interface InfernoMetalRenderPipelineState
        : NSObject <MTLRenderPipelineState>
    -(nullable instancetype)initWithContext
        : (InfernoMetalCompilerContext *)context device
        : (id<MTLDevice>)device descriptor
        : (MTLRenderPipelineDescriptor *)descriptor result
        : (const ImtlCompilerResult *)result;
    @property(nonatomic, readonly) InfernoMetalCompilerContext *infernoContext;
    @property(nonatomic, readonly) InfernoMetalFunction *infernoVertexFunction;
    @property(nonatomic, readonly)
        InfernoMetalFunction *infernoFragmentFunction;
    @property(nonatomic, readonly) ImtlBatch5RenderPipeline infernoRecord;
    @end
NS_ASSUME_NONNULL_END
