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
#import "InfernoMetalRenderPipelineState.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#import "InfernoMetalLibrary.h"

@interface InfernoMetalRenderPipelineState ()
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, strong) InfernoMetalFunction *storedVertex;
@property(nonatomic, strong) InfernoMetalFunction *storedFragment;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@property(nonatomic) ImtlBatch5RenderPipeline storedRecord;
@property(nonatomic) ImtlCompilerRenderPipeline storedMetadata;
@end
@implementation InfernoMetalRenderPipelineState
- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                     descriptor:(MTLRenderPipelineDescriptor *)descriptor
                         result:(const ImtlCompilerResult *)result
{
    InfernoMetalFunction *vertex = (id)descriptor.vertexFunction;
    InfernoMetalFunction *fragment = (id)descriptor.fragmentFunction;
    if (!(self = [super init]) || !context || !device || !descriptor ||
        !result || !vertex || !fragment)
        return nil;
    _storedContext = context;
    _storedDevice = device;
    _storedVertex = vertex;
    _storedFragment = fragment;
    _storedLabel = [descriptor.label copy];
    MTLRenderPipelineColorAttachmentDescriptor *color =
        descriptor.colorAttachments[0];
    _storedRecord = (ImtlBatch5RenderPipeline){
        .color0_pixel_format = (uint32_t)color.pixelFormat,
        .raster_sample_count = (uint32_t)descriptor.rasterSampleCount,
        .blending_enabled = color.blendingEnabled,
        .source_rgb_blend_factor = (uint32_t)color.sourceRGBBlendFactor,
        .destination_rgb_blend_factor =
            (uint32_t)color.destinationRGBBlendFactor,
        .rgb_blend_operation = (uint32_t)color.rgbBlendOperation,
        .source_alpha_blend_factor = (uint32_t)color.sourceAlphaBlendFactor,
        .destination_alpha_blend_factor =
            (uint32_t)color.destinationAlphaBlendFactor,
        .alpha_blend_operation = (uint32_t)color.alphaBlendOperation,
        .write_mask = (uint32_t)color.writeMask,
        .vertex_function_name = vertex.name.UTF8String,
        .fragment_function_name = fragment.name.UTF8String,
    };
    _storedMetadata = result->render_pipeline;
    return self;
}
- (InfernoMetalCompilerContext *)infernoContext
{
    return _storedContext;
}
- (InfernoMetalFunction *)infernoVertexFunction
{
    return _storedVertex;
}
- (InfernoMetalFunction *)infernoFragmentFunction
{
    return _storedFragment;
}
- (ImtlBatch5RenderPipeline)infernoRecord
{
    ImtlBatch5RenderPipeline result = _storedRecord;
    result.vertex_function_name = _storedVertex.name.UTF8String;
    result.fragment_function_name = _storedFragment.name.UTF8String;
    return result;
}
- (id<MTLDevice>)device
{
    return _storedDevice;
}
- (NSString *)label
{
    return _storedLabel;
}
- (MTLRenderPipelineReflection *)reflection
{
    return nil;
}
- (NSUInteger)allocatedSize
{
    return (NSUInteger)_storedMetadata.allocated_size;
}
- (NSUInteger)imageblockSampleLength
{
    return (NSUInteger)_storedMetadata.imageblock_sample_length;
}
- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return (NSUInteger)_storedMetadata.max_total_threads_per_threadgroup;
}
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup
{
    return (NSUInteger)_storedMetadata.max_total_threads_per_object_threadgroup;
}
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup
{
    return (NSUInteger)_storedMetadata.max_total_threads_per_mesh_threadgroup;
}
- (NSUInteger)objectThreadExecutionWidth
{
    return (NSUInteger)_storedMetadata.object_thread_execution_width;
}
- (NSUInteger)meshThreadExecutionWidth
{
    return (NSUInteger)_storedMetadata.mesh_thread_execution_width;
}
- (NSUInteger)maxTotalThreadgroupsPerMeshGrid
{
    return (NSUInteger)_storedMetadata.max_total_threadgroups_per_mesh_grid;
}
- (MTLShaderValidation)shaderValidation
{
    return (MTLShaderValidation)_storedMetadata.shader_validation;
}
- (MTLSize)requiredThreadsPerTileThreadgroup
{
    return MTLSizeMake(
        _storedMetadata.required_threads_per_tile_threadgroup.width,
        _storedMetadata.required_threads_per_tile_threadgroup.height,
        _storedMetadata.required_threads_per_tile_threadgroup.depth);
}
- (MTLSize)requiredThreadsPerObjectThreadgroup
{
    return MTLSizeMake(
        _storedMetadata.required_threads_per_object_threadgroup.width,
        _storedMetadata.required_threads_per_object_threadgroup.height,
        _storedMetadata.required_threads_per_object_threadgroup.depth);
}
- (MTLSize)requiredThreadsPerMeshThreadgroup
{
    return MTLSizeMake(
        _storedMetadata.required_threads_per_mesh_threadgroup.width,
        _storedMetadata.required_threads_per_mesh_threadgroup.height,
        _storedMetadata.required_threads_per_mesh_threadgroup.depth);
}
- (BOOL)supportIndirectCommandBuffers
{
    return (_storedMetadata.flags &
            INFERNO_METAL_RESOURCE_RENDER_RESULT_SUPPORTS_INDIRECT) != 0;
}
- (BOOL)threadgroupSizeMatchesTileSize
{
    return (_storedMetadata.flags &
            INFERNO_METAL_RESOURCE_RENDER_RESULT_THREADGROUP_MATCHES_TILE) != 0;
}
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)dimensions
{
    (void)dimensions;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Render imageblock dimensions are unsupported"];
    return 0;
}
- (MTLResourceID)gpuResourceID
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"No stable GPU resource ID exists"];
    return (MTLResourceID){ 0 };
}
- (id<MTLFunctionHandle>)functionHandleWithName:(NSString *)name
                                          stage:(MTLRenderStages)stage
{
    (void)name;
    (void)stage;
    return nil;
}
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function
                                              stage:(MTLRenderStages)stage
{
    (void)function;
    (void)stage;
    return nil;
}
- (id<MTLFunctionHandle>)functionHandleWithBinaryFunction:
                             (id<MTL4BinaryFunction>)function
                                                    stage:(MTLRenderStages)stage
{
    (void)function;
    (void)stage;
    return nil;
}
- (id<MTLVisibleFunctionTable>)
    newVisibleFunctionTableWithDescriptor:
        (MTLVisibleFunctionTableDescriptor *)descriptor
                                    stage:(MTLRenderStages)stage
{
    (void)descriptor;
    (void)stage;
    return nil;
}
- (id<MTLIntersectionFunctionTable>)
    newIntersectionFunctionTableWithDescriptor:
        (MTLIntersectionFunctionTableDescriptor *)descriptor
                                         stage:(MTLRenderStages)stage
{
    (void)descriptor;
    (void)stage;
    return nil;
}
- (id<MTLRenderPipelineState>)
    newRenderPipelineStateWithAdditionalBinaryFunctions:
        (MTLRenderPipelineFunctionsDescriptor *)functions
                                                  error:(NSError **)error
{
    (void)functions;
    if (error)
        *error = InfernoMetalMakeError(
            InfernoMetalErrorUnsupported,
            @"Additional binary functions are unsupported");
    return nil;
}
- (id<MTLRenderPipelineState>)
    newRenderPipelineStateWithBinaryFunctions:
        (MTL4RenderPipelineBinaryFunctionsDescriptor *)functions
                                        error:(NSError **)error
{
    (void)functions;
    if (error)
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Binary functions are unsupported");
    return nil;
}
- (MTL4PipelineDescriptor *)newRenderPipelineDescriptorForSpecialization
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Render pipeline specialization is unsupported"];
    return nil;
}
@end
