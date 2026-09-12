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
#import "InfernoMetalComputePipelineState.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"

@interface InfernoMetalCompilerContext (Imageblock)
- (BOOL)queryImageblockForFunction:(InfernoMetalFunction *)function
                        dimensions:(MTLSize)dimensions
                            length:(NSUInteger *)length
                             error:(NSError **)error;
@end
@interface InfernoMetalComputePipelineState ()
@property(nonatomic, strong) InfernoMetalCompilerContext *context;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, strong) InfernoMetalFunction *function;
@property(nonatomic) NSUInteger storedAllocated, storedMaximum, storedWidth,
    storedStatic;
@property(nonatomic) MTLSize storedRequired;
@property(nonatomic) MTLShaderValidation storedValidation;
@property(nonatomic) BOOL storedIndirect;
@property(nonatomic, strong) NSMutableDictionary *imageblockCache;
@end
@implementation InfernoMetalComputePipelineState
- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                       function:(InfernoMetalFunction *)function
                         result:(const ImtlCompilerResult *)result
{
    if (!(self = [super init]) || !context || !device || !function || !result)
        return nil;
    _context = context;
    _storedDevice = device;
    _function = function;
    _storedAllocated = result->allocated_size;
    _storedMaximum = result->max_total_threads_per_threadgroup;
    _storedWidth = result->thread_execution_width;
    _storedStatic = result->static_threadgroup_memory_length;
    _storedRequired = MTLSizeMake(result->required_threads_width,
                                  result->required_threads_height,
                                  result->required_threads_depth);
    _storedValidation = result->shader_validation;
    _storedIndirect =
        (result->pipeline_flags &
         INFERNO_METAL_COMPILER_PIPELINE_FLAG_SUPPORTS_INDIRECT_COMMAND_BUFFERS) !=
        0;
    _imageblockCache = [NSMutableDictionary dictionary];
    return self;
}
- (id<MTLDevice>)device
{
    return _storedDevice;
}
- (NSString *)label
{
    return nil;
}
- (MTLComputePipelineReflection *)reflection
{
    return nil;
}
- (NSUInteger)allocatedSize
{
    return _storedAllocated;
}
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return _storedMaximum;
}
- (NSUInteger)threadExecutionWidth
{
    return _storedWidth;
}
- (NSUInteger)staticThreadgroupMemoryLength
{
    return _storedStatic;
}
- (BOOL)supportIndirectCommandBuffers
{
    return _storedIndirect;
}
- (MTLShaderValidation)shaderValidation
{
    return _storedValidation;
}
- (MTLSize)requiredThreadsPerThreadgroup
{
    return _storedRequired;
}
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d
{
    NSString *key = [NSString
        stringWithFormat:@"%lux%lux%lu", (unsigned long)d.width,
                         (unsigned long)d.height, (unsigned long)d.depth];
    @synchronized(_imageblockCache) {
        NSNumber *cached = _imageblockCache[key];
        if (cached)
            return cached.unsignedIntegerValue;
    }
    NSUInteger length = 0;
    NSError *error = nil;
    if (![_context queryImageblockForFunction:_function
                                   dimensions:d
                                       length:&length
                                        error:&error])
        [NSException raise:InfernoMetalTransportException
                    format:@"%@", error.localizedDescription];
    @synchronized(_imageblockCache) {
        _imageblockCache[key] = @(length);
    }
    return length;
}
- (MTLResourceID)gpuResourceID
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"No pinned host GPU resource ID exists"];
    return (MTLResourceID){ 0 };
}
- (id<MTLFunctionHandle>)functionHandleWithName:(NSString *)name
{
    (void)name;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Function handles are unsupported"];
    return nil;
}
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function
{
    (void)function;
    return [self functionHandleWithName:@""];
}
- (id<MTLFunctionHandle>)functionHandleWithBinaryFunction:
    (id<MTL4BinaryFunction>)function
{
    (void)function;
    return [self functionHandleWithName:@""];
}
- (id<MTLComputePipelineState>)
    newComputePipelineStateWithBinaryFunctions:
        (NSArray<id<MTL4BinaryFunction>> *)functions
                                         error:(NSError **)error
{
    (void)functions;
    if (error)
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Binary functions are unsupported");
    return nil;
}
- (id<MTLComputePipelineState>)
    newComputePipelineStateWithAdditionalBinaryFunctions:
        (NSArray<id<MTLFunction>> *)functions
                                                   error:(NSError **)error
{
    (void)functions;
    if (error)
        *error = InfernoMetalMakeError(
            InfernoMetalErrorUnsupported,
            @"Additional binary functions are unsupported");
    return nil;
}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:
    (MTLVisibleFunctionTableDescriptor *)descriptor
{
    (void)descriptor;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Visible function tables require native resources"];
    return nil;
}
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:
    (MTLIntersectionFunctionTableDescriptor *)descriptor
{
    (void)descriptor;
    [NSException
         raise:InfernoMetalUnsupportedException
        format:@"Intersection function tables require native resources"];
    return nil;
}
@end
