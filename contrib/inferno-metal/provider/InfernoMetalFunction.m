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
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#import "InfernoMetalLibrary.h"
#import "InfernoMetalReflectionObjects.h"

@interface InfernoMetalFunction ()
@property(nonatomic, strong) InfernoMetalLibrary *storedLibrary;
@property(nonatomic, copy) NSString *storedName;
@property(atomic, copy, nullable) NSString *storedLabel;
@property(nonatomic) MTLFunctionType storedType;
@property(nonatomic) MTLPatchType storedPatchType;
@property(nonatomic) NSInteger storedPatchCount;
@property(nonatomic) MTLFunctionOptions storedOptions;
@property(nonatomic, copy) NSDictionary *storedConstants;
@property(nonatomic, copy, nullable) NSArray *storedVertex;
@property(nonatomic, copy, nullable) NSArray *storedStage;
@end
@implementation InfernoMetalFunction
- (instancetype)initWithLibrary:(InfernoMetalLibrary *)library
                         record:(const ImtlCompilerFunction *)record
{
    if (!(self = [super init]) || !library || !record)
        return nil;
    _storedLibrary = library;
    _storedName = [[NSString alloc] initWithBytes:record->name
                                           length:record->name_length
                                         encoding:NSUTF8StringEncoding];
    _storedType = record->type;
    _storedPatchType = record->patch_type;
    _storedPatchCount = (NSInteger)record->patch_control_point_count;
    _storedOptions = record->options;
    NSMutableDictionary *constants = [NSMutableDictionary dictionary];
    NSMutableArray *vertex = [NSMutableArray array];
    NSMutableArray *stage = [NSMutableArray array];
    uint32_t total = record->constant_count + record->vertex_attribute_count +
                     record->stage_input_attribute_count;
    for (uint32_t i = 0; i < total; i++) {
        ImtlCompilerMetadata m;
        if (!imtl_compiler_metadata_at(record, i, &m))
            return nil;
        NSString *name = [[NSString alloc] initWithBytes:m.name
                                                  length:m.name_length
                                                encoding:NSUTF8StringEncoding];
        if (!name)
            return nil;
        if (m.kind == INFERNO_METAL_COMPILER_METADATA_CONSTANT)
            constants[name] = [[InfernoMetalFunctionConstant alloc]
                initWithName:name
                        type:m.data_type
                       index:(NSUInteger)m.index
                    required:(m.flags &
                              INFERNO_METAL_COMPILER_CONSTANT_FLAG_REQUIRED) !=
                             0];
        else {
            BOOL
                active = (m.flags &
                          INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_ACTIVE) != 0,
                patch = (m.flags &
                         INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_PATCH_DATA) != 0,
                control =
                    (m.flags &
                     INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_PATCH_CONTROL_POINT_DATA) !=
                    0;
            id value =
                m.kind == INFERNO_METAL_COMPILER_METADATA_VERTEX_ATTRIBUTE ?
                    [[InfernoMetalVertexAttribute alloc]
                                 initWithName:name
                                         type:m.data_type
                                        index:(NSUInteger)m.index
                                       active:active
                                    patchData:patch
                        patchControlPointData:control] :
                    [[InfernoMetalAttribute alloc]
                                 initWithName:name
                                         type:m.data_type
                                        index:(NSUInteger)m.index
                                       active:active
                                    patchData:patch
                        patchControlPointData:control];
            [m.kind == INFERNO_METAL_COMPILER_METADATA_VERTEX_ATTRIBUTE ?
                    vertex :
                    stage addObject:value];
        }
    }
    _storedConstants = [constants copy];
    _storedVertex =
        record->flags &
                INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_VERTEX_ATTRIBUTES ?
            [vertex copy] :
            nil;
    _storedStage =
        record->flags &
                INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_STAGE_INPUT_ATTRIBUTES ?
            [stage copy] :
            nil;
    return self;
}
- (InfernoMetalLibrary *)infernoLibrary
{
    return _storedLibrary;
}
- (NSString *)label
{
    return self.storedLabel;
}
- (void)setLabel:(NSString *)label
{
    self.storedLabel = label;
}
- (id<MTLDevice>)device
{
    return _storedLibrary.device;
}
- (NSString *)name
{
    return _storedName;
}
- (MTLFunctionType)functionType
{
    return _storedType;
}
- (MTLPatchType)patchType
{
    return _storedPatchType;
}
- (NSInteger)patchControlPointCount
{
    return _storedPatchCount;
}
- (MTLFunctionOptions)options
{
    return _storedOptions;
}
- (NSDictionary *)functionConstantsDictionary
{
    return _storedConstants;
}
- (NSArray *)vertexAttributes
{
    return _storedVertex;
}
- (NSArray *)stageInputAttributes
{
    return _storedStage;
}
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index
{
    if (index > 30)
        [NSException raise:InfernoMetalInvalidUseException
                    format:@"An argument buffer index must be at most 30"];
    return [_storedLibrary.infernoContext newArgumentEncoderForFunction:self
                                                            bufferIndex:index];
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
- (id<MTLArgumentEncoder>)
    newArgumentEncoderWithBufferIndex:(NSUInteger)index
                           reflection:(MTLAutoreleasedArgument *)reflection
{
    if (reflection)
        *reflection = nil;
    return [self newArgumentEncoderWithBufferIndex:index];
}
#pragma clang diagnostic pop
@end
