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
#import "InfernoMetalLibrary.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#include <string.h>

@interface InfernoMetalLibrary ()
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, copy) NSData *storedPayload;
@property(nonatomic) uint32_t storedLibraryKind;
@property(nonatomic, copy) NSArray *storedFunctionNames;
@property(nonatomic, copy) NSDictionary *recordsByName;
@property(nonatomic) MTLLibraryType storedType;
@property(nonatomic, copy, nullable) NSString *storedInstallName;
@property(atomic, copy, nullable) NSString *storedLabel;
@property(nonatomic, strong, nullable) NSError *storedWarning;
@end
@implementation InfernoMetalLibrary
static uint32_t recordGet32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
static uint64_t recordGet64(const uint8_t *p)
{
    return recordGet32(p) | (uint64_t)recordGet32(p + 4) << 32;
}
- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                        payload:(NSData *)payload
                           kind:(uint32_t)kind
                         result:(const ImtlCompilerResult *)result
                 compileWarning:(NSError *)warning
                          error:(NSError **)error
{
    if (error)
        *error = nil;
    if (!(self = [super init]) || !context || !device || !payload.length ||
        (kind != INFERNO_METAL_RESOURCE_LIBRARY_SOURCE &&
         kind != INFERNO_METAL_RESOURCE_LIBRARY_METALLIB) ||
        !result)
        return nil;
    _storedContext = context;
    _storedDevice = device;
    _storedPayload = [payload copy];
    _storedLibraryKind = kind;
    _storedType = result->library_type;
    _storedWarning = warning;
    if (result->library_flags &
        INFERNO_METAL_COMPILER_LIBRARY_FLAG_HAS_INSTALL_NAME)
        _storedInstallName =
            [[NSString alloc] initWithBytes:result->library_install_name
                                     length:result->library_install_name_length
                                   encoding:NSUTF8StringEncoding];
    NSMutableArray *names = [NSMutableArray array];
    NSMutableDictionary *records = [NSMutableDictionary dictionary];
    for (uint32_t i = 0; i < result->function_count; i++) {
        ImtlCompilerFunction record;
        if (!imtl_compiler_function_at(result, i, &record)) {
            if (error)
                *error = InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                               @"Malformed function inventory");
            return nil;
        }
        NSString *name = [[NSString alloc] initWithBytes:record.name
                                                  length:record.name_length
                                                encoding:NSUTF8StringEncoding];
        size_t size =
            INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
            (size_t)(record.constant_count + record.vertex_attribute_count +
                     record.stage_input_attribute_count) *
                INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
        if (!name) {
            if (error)
                *error =
                    InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                          @"Cannot copy function metadata");
            return nil;
        }
        [names addObject:name];
        records[name] =
            [NSData dataWithBytes:(const uint8_t *)record.name -
                                  INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET
                           length:size];
    }
    _storedFunctionNames = [names copy];
    _recordsByName = [records copy];
    return self;
}
- (InfernoMetalCompilerContext *)infernoContext
{
    return _storedContext;
}
- (NSData *)infernoPayload
{
    return _storedPayload;
}
- (uint32_t)infernoLibraryKind
{
    return _storedLibraryKind;
}
- (NSError *)compileWarning
{
    return _storedWarning;
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
    return _storedDevice;
}
- (NSArray *)functionNames
{
    return _storedFunctionNames;
}
- (MTLLibraryType)type
{
    return _storedType;
}
- (NSString *)installName
{
    return _storedInstallName;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
{
    NSData *data = _recordsByName[name];
    if (!data)
        return nil;
    const uint8_t *p = data.bytes;
    uint64_t patchBits = recordGet64(
        p + INFERNO_METAL_COMPILER_RECORD_PATCH_CONTROL_POINT_COUNT_OFFSET);
    int64_t patchCount;
    memcpy(&patchCount, &patchBits, sizeof(patchCount));
    ImtlCompilerFunction r = {
        .type = recordGet32(p + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET),
        .name = (const char *)p + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
        .name_length =
            recordGet32(p + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET),
        .patch_type =
            recordGet32(p + INFERNO_METAL_COMPILER_RECORD_PATCH_TYPE_OFFSET),
        .flags = recordGet32(p + INFERNO_METAL_COMPILER_RECORD_FLAGS_OFFSET),
        .patch_control_point_count = patchCount,
        .options =
            recordGet64(p + INFERNO_METAL_COMPILER_RECORD_OPTIONS_OFFSET),
        .constant_count = recordGet32(
            p + INFERNO_METAL_COMPILER_RECORD_CONSTANT_COUNT_OFFSET),
        .vertex_attribute_count = recordGet32(
            p + INFERNO_METAL_COMPILER_RECORD_VERTEX_ATTRIBUTE_COUNT_OFFSET),
        .stage_input_attribute_count = recordGet32(
            p +
            INFERNO_METAL_COMPILER_RECORD_STAGE_INPUT_ATTRIBUTE_COUNT_OFFSET),
        .metadata_records = p + INFERNO_METAL_COMPILER_RECORD_METADATA_OFFSET
    };
    return [[InfernoMetalFunction alloc] initWithLibrary:self record:&r];
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(MTLFunctionConstantValues *)values
                                 error:(NSError **)error
{
    if (error)
        *error =
            InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                  @"Function specialization is unsupported");
    (void)name;
    (void)values;
    return nil;
}
- (void)newFunctionWithName:(NSString *)name
             constantValues:(MTLFunctionConstantValues *)values
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error;
    id value = [self newFunctionWithName:name
                          constantValues:values
                                   error:&error];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
      handler(value, error);
    });
}
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor
                                       error:(NSError **)error
{
    (void)descriptor;
    if (error)
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Function descriptors are unsupported");
    return nil;
}
- (void)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor
                completionHandler:(void (^)(id<MTLFunction>, NSError *))handler
{
    NSError *error;
    id value = [self newFunctionWithDescriptor:descriptor error:&error];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
      handler(value, error);
    });
}
- (id<MTLFunction>)newIntersectionFunctionWithDescriptor:
                       (MTLIntersectionFunctionDescriptor *)descriptor
                                                   error:(NSError **)error
{
    return [self newFunctionWithDescriptor:(id)descriptor error:error];
}
- (void)newIntersectionFunctionWithDescriptor:
            (MTLIntersectionFunctionDescriptor *)descriptor
                            completionHandler:
                                (void (^)(id<MTLFunction>, NSError *))handler
{
    [self newFunctionWithDescriptor:(id)descriptor completionHandler:handler];
}
- (MTLFunctionReflection *)reflectionForFunctionWithName:(NSString *)name
{
    (void)name;
    return nil;
}
@end
