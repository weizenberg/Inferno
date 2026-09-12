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

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalEncodedDispatch : NSObject
    @property(nonatomic, strong) id<MTLComputePipelineState> pipeline;
    @property(nonatomic, copy) NSArray *bindings;
    @property(nonatomic, copy) NSArray<NSNumber *> *threadgroupLengths;
    @property(nonatomic) uint32_t mode;
    @property(nonatomic) MTLSize grid;
    @property(nonatomic) MTLSize group;
    @end

    @interface InfernoMetalComputeCommandEncoder
        : NSObject <MTLComputeCommandEncoder>

    -(nullable instancetype)initWithCommandBuffer
        : (InfernoMetalCommandBuffer *)commandBuffer device
        : (id<MTLDevice>)device;

    @end

NS_ASSUME_NONNULL_END
