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
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include "../coordinator.h"

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalCompilerContext : NSObject

    /* service is borrowed. The owner is weakly held; each operation takes a
     * strong snapshot and fails if it has disappeared.
     */
    -(nullable instancetype)initWithService : (io_service_t)service owner
        : (id<MTLDevice>)owner timeoutNS : (uint64_t)timeoutNS error
        : (NSError **)error;

    /* Takes ownership of coordinator. This is the fixture/integration seam; the
     * caller may substitute the linked imtl_coordinator_* functions.
     */
    -(nullable instancetype)initWithCoordinator
        : (ImtlCoordinator *)coordinator owner
        : (id<MTLDevice>)owner completionQueue
        : (dispatch_queue_t)completionQueue;

    -(nullable id<MTLLibrary>)newLibraryWithSource : (NSString *)source options
        : (nullable MTLCompileOptions *)options error : (NSError **)error;
    -(void)newLibraryWithSource : (NSString *)source options
        : (nullable MTLCompileOptions *)options completionHandler
        : (MTLNewLibraryCompletionHandler)completionHandler;
    -(nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction
        : (id<MTLFunction>)function error : (NSError **)error;
    -(nullable id<MTLComputePipelineState>)newComputePipelineStateWithFunction
        : (id<MTLFunction>)function options
        : (MTLPipelineOption)options reflection
        : (MTLComputePipelineReflection *_Nullable *_Nullable)reflection error
        : (NSError **)error;
    -(void)newComputePipelineStateWithFunction
        : (id<MTLFunction>)function completionHandler
        : (MTLNewComputePipelineStateCompletionHandler)completionHandler;
    -(void)newComputePipelineStateWithFunction
        : (id<MTLFunction>)function options
        : (MTLPipelineOption)options completionHandler
        : (MTLNewComputePipelineStateWithReflectionCompletionHandler)
              completionHandler;

    -(nullable id<MTLLibrary>)newLibraryWithData : (dispatch_data_t)data error
        : (NSError **)error;
    -(nullable id<MTLLibrary>)newDefaultLibrary;
    -(nullable id<MTLLibrary>)newDefaultLibraryWithBundle
        : (NSBundle *)bundle error : (NSError **)error;
    -(nullable id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor
        : (MTLComputePipelineDescriptor *)descriptor options
        : (MTLPipelineOption)options reflection
        : (MTLComputePipelineReflection *_Nullable *_Nullable)reflection error
        : (NSError **)error;

    -(nullable id<MTLBuffer>)newBufferWithLength : (NSUInteger)length options
        : (MTLResourceOptions)options;
    -(nullable id<MTLBuffer>)newBufferWithBytes : (const void *)pointer length
        : (NSUInteger)length options : (MTLResourceOptions)options;
    -(nullable id<MTLBuffer>)newBufferWithBytesNoCopy : (void *)pointer length
        : (NSUInteger)length options : (MTLResourceOptions)options deallocator
        : (nullable void (^)(void *pointer, NSUInteger length))deallocator;
    -(nullable id<MTLCommandQueue>)newCommandQueue;
    -(nullable id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount
        : (NSUInteger)count;
    -(nullable id<MTLCommandQueue>)newCommandQueueWithDescriptor
        : (MTLCommandQueueDescriptor *)descriptor;

    -(void)invalidate;

    @end

NS_ASSUME_NONNULL_END
