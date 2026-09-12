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

#include "qemu/osdep.h"
#include "hw/misc/apple-silicon/metal-bridge.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "metal-batch.h"
#include "trace.h"

#import <Metal/Metal.h>

#define METAL_PIPELINE_CACHE_SIZE 8
#define METAL_LIBRARY_CACHE_SIZE 8

struct InfernoMetalBackend {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    /* Oldest first. Only the single outstanding worker accesses the cache. */
    NSMutableArray *pipeline_keys;
    NSMutableArray *pipelines;
    NSMutableArray *pipeline_warnings;
    NSMutableArray *library_keys;
    NSMutableArray *libraries;
    NSMutableArray *library_warnings;
};

InfernoMetalBackend *inferno_metal_backend_new(Error **errp)
{
    @autoreleasepool {
        InfernoMetalBackend *backend = g_new0(InfernoMetalBackend, 1);

        backend->device = MTLCreateSystemDefaultDevice();
        if (backend->device) {
            backend->queue = [backend->device newCommandQueue];
        }
        if (!backend->device || !backend->queue) {
            error_setg(errp,
                       "Inferno Metal bridge cannot create a host GPU queue");
            inferno_metal_backend_free(backend);
            return NULL;
        }
        backend->pipeline_keys = [[NSMutableArray alloc] init];
        backend->pipelines = [[NSMutableArray alloc] init];
        backend->pipeline_warnings = [[NSMutableArray alloc] init];
        backend->library_keys = [[NSMutableArray alloc] init];
        backend->libraries = [[NSMutableArray alloc] init];
        backend->library_warnings = [[NSMutableArray alloc] init];
        if (!backend->pipeline_keys || !backend->pipelines ||
            !backend->pipeline_warnings || !backend->library_keys ||
            !backend->libraries || !backend->library_warnings) {
            error_setg(errp,
                       "Inferno Metal bridge cannot create a pipeline cache");
            inferno_metal_backend_free(backend);
            return NULL;
        }
        return backend;
    }
}

void inferno_metal_backend_free(InfernoMetalBackend *backend)
{
    if (backend) {
        [backend->library_warnings release];
        [backend->libraries release];
        [backend->library_keys release];
        [backend->pipeline_warnings release];
        [backend->pipelines release];
        [backend->pipeline_keys release];
        [backend->queue release];
        [backend->device release];
        g_free(backend);
    }
}

/* Retain across promotion, even when removal drops the cache's last reference.
 */
static id metal_pipeline_lookup(InfernoMetalBackend *backend, NSArray *key,
                                NSError **warning)
{
    NSUInteger index = [backend->pipeline_keys indexOfObject:key];

    if (index == NSNotFound) {
        *warning = nil;
        return nil;
    }
    id pipeline =
        [[[backend->pipelines objectAtIndex:index] retain] autorelease];
    id diagnostic =
        [[[backend->pipeline_warnings objectAtIndex:index] retain] autorelease];
    [backend->pipeline_keys removeObjectAtIndex:index];
    [backend->pipelines removeObjectAtIndex:index];
    [backend->pipeline_warnings removeObjectAtIndex:index];
    [backend->pipeline_keys addObject:key];
    [backend->pipelines addObject:pipeline];
    [backend->pipeline_warnings addObject:diagnostic];
    *warning = diagnostic == [NSNull null] ? nil : diagnostic;
    return pipeline;
}

static void metal_pipeline_insert(InfernoMetalBackend *backend, NSArray *key,
                                  id pipeline, NSError *warning)
{
    if ([backend->pipeline_keys count] == METAL_PIPELINE_CACHE_SIZE) {
        [backend->pipeline_keys removeObjectAtIndex:0];
        [backend->pipelines removeObjectAtIndex:0];
        [backend->pipeline_warnings removeObjectAtIndex:0];
    }
    [backend->pipeline_keys addObject:key];
    [backend->pipelines addObject:pipeline];
    [backend->pipeline_warnings addObject:warning ?: [NSNull null]];
}

static id<MTLLibrary> metal_library_lookup(InfernoMetalBackend *backend,
                                           NSData *key, NSError **warning)
{
    NSUInteger index = [backend->library_keys indexOfObject:key];
    if (index == NSNotFound) {
        *warning = nil;
        return nil;
    }
    id library =
        [[[backend->libraries objectAtIndex:index] retain] autorelease];
    id diagnostic =
        [[[backend->library_warnings objectAtIndex:index] retain] autorelease];
    [backend->library_keys removeObjectAtIndex:index];
    [backend->libraries removeObjectAtIndex:index];
    [backend->library_warnings removeObjectAtIndex:index];
    [backend->library_keys addObject:key];
    [backend->libraries addObject:library];
    [backend->library_warnings addObject:diagnostic];
    *warning = diagnostic == [NSNull null] ? nil : diagnostic;
    return library;
}

static void metal_library_insert(InfernoMetalBackend *backend, NSData *key,
                                 id<MTLLibrary> library, NSError *warning)
{
    if ([backend->library_keys count] == METAL_LIBRARY_CACHE_SIZE) {
        [backend->library_keys removeObjectAtIndex:0];
        [backend->libraries removeObjectAtIndex:0];
        [backend->library_warnings removeObjectAtIndex:0];
    }
    [backend->library_keys addObject:key];
    [backend->libraries addObject:library];
    [backend->library_warnings addObject:warning ?: [NSNull null]];
}

static bool metal_error(char *message, size_t size, const char *operation,
                        NSError *error)
{
    snprintf(message, size, "%s: %s", operation,
             error ? [[error localizedDescription] UTF8String] : "no object");
    return false;
}

static void metal_phase(const InfernoMetalCommand *command, const char *phase,
                        int64_t *start)
{
    int64_t now;

    if (!trace_event_get_state_backends(TRACE_INFERNO_METAL_BACKEND_PHASE)) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    trace_inferno_metal_backend_phase(command->sequence, command->opcode, phase,
                                      now - *start);
    *start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static bool metal_function_has_constants(id<MTLFunction> function)
{
    NSDictionary *constants = [function functionConstantsDictionary];

    return constants && [constants count] != 0;
}

static bool metal_function_type(uint32_t *result, MTLFunctionType type)
{
    switch (type) {
    case MTLFunctionTypeVertex:
        *result = INFERNO_METAL_FUNCTION_TYPE_VERTEX;
        return true;
    case MTLFunctionTypeFragment:
        *result = INFERNO_METAL_FUNCTION_TYPE_FRAGMENT;
        return true;
    case MTLFunctionTypeKernel:
        *result = INFERNO_METAL_FUNCTION_TYPE_KERNEL;
        return true;
    case MTLFunctionTypeVisible:
        *result = INFERNO_METAL_FUNCTION_TYPE_VISIBLE;
        return true;
    case MTLFunctionTypeIntersection:
        *result = INFERNO_METAL_FUNCTION_TYPE_INTERSECTION;
        return true;
    case MTLFunctionTypeMesh:
        *result = INFERNO_METAL_FUNCTION_TYPE_MESH;
        return true;
    case MTLFunctionTypeObject:
        *result = INFERNO_METAL_FUNCTION_TYPE_OBJECT;
        return true;
    default:
        return false;
    }
}

static bool metal_data_type(MTLDataType type)
{
    return type <= 56 || (type >= 58 && type <= 60) ||
           (type >= 62 && type <= 88) || (type >= 115 && type <= 118) ||
           (type >= 121 && type <= 124) || (type >= 139 && type <= 140);
}

static NSData *metal_bounded_string(NSString *string, NSUInteger capacity,
                                    bool allow_empty)
{
    NSData *data = [string dataUsingEncoding:NSUTF8StringEncoding];

    if (!string || !data || (!allow_empty && ![data length]) ||
        [data length] >= capacity) {
        return nil;
    }
    return data;
}

static NSArray *metal_sorted_constants(NSDictionary *dictionary,
                                       NSString **reason)
{
    NSMutableArray *result = [NSMutableArray array];

    for (id key in dictionary) {
        id constant = [dictionary objectForKey:key];
        if (![key isKindOfClass:[NSString class]] ||
            ![constant isKindOfClass:[MTLFunctionConstant class]] ||
            ![key isEqual:[constant name]]) {
            *reason = @"function constant dictionary key/name mismatch";
            continue;
        }
        NSUInteger insertion = 0;
        while (insertion < [result count]) {
            MTLFunctionConstant *other = [result objectAtIndex:insertion];
            if ([constant index] < [other index] ||
                ([constant index] == [other index] &&
                 [[constant name] compare:[other name]] ==
                     NSOrderedAscending)) {
                break;
            }
            insertion++;
        }
        [result insertObject:constant atIndex:insertion];
    }
    return result;
}

static bool metal_validate_metadata(NSArray *items, bool constant,
                                    NSString **reason)
{
    NSMutableSet *names = [NSMutableSet set];
    NSMutableSet *indices = [NSMutableSet set];

    for (id item in items) {
        NSString *name = [item name];
        MTLDataType type;
        NSUInteger value;
        if (constant) {
            MTLFunctionConstant *metadata = item;
            type = metadata.type;
            value = metadata.index;
        } else {
            MTLAttribute *metadata = item;
            type = metadata.attributeType;
            value = metadata.attributeIndex;
        }
        NSData *bytes = metal_bounded_string(
            name, INFERNO_METAL_COMPILER_METADATA_NAME_SIZE, false);
        NSNumber *index = @(value);
        if (!bytes || !metal_data_type(type) ||
            [indices containsObject:index] ||
            (constant && [names containsObject:name])) {
            *reason = constant ? @"unsupported function constant metadata" :
                                 @"unsupported function attribute metadata";
            return false;
        }
        [indices addObject:index];
        if (constant) {
            [names addObject:name];
        }
    }
    return true;
}

static void metal_write_metadata(uint8_t *record, uint32_t kind, id item)
{
    NSData *name = [[item name] dataUsingEncoding:NSUTF8StringEncoding];
    bool constant = kind == INFERNO_METAL_COMPILER_METADATA_CONSTANT;
    MTLDataType type;
    NSUInteger index;
    if (constant) {
        MTLFunctionConstant *metadata = item;
        type = metadata.type;
        index = metadata.index;
    } else {
        MTLAttribute *metadata = item;
        type = metadata.attributeType;
        index = metadata.attributeIndex;
    }
    uint32_t flags = 0;
    if (constant) {
        if ([item required]) {
            flags |= INFERNO_METAL_COMPILER_CONSTANT_FLAG_REQUIRED;
        }
    } else {
        if ([item isActive]) {
            flags |= INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_ACTIVE;
        }
        if ([item isPatchData]) {
            flags |= INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_PATCH_DATA;
        }
        if ([item isPatchControlPointData]) {
            flags |=
                INFERNO_METAL_COMPILER_ATTRIBUTE_FLAG_PATCH_CONTROL_POINT_DATA;
        }
    }
    stl_le_p(record + INFERNO_METAL_COMPILER_METADATA_KIND_OFFSET, kind);
    stl_le_p(record + INFERNO_METAL_COMPILER_METADATA_DATA_TYPE_OFFSET, type);
    stq_le_p(record + INFERNO_METAL_COMPILER_METADATA_INDEX_OFFSET, index);
    stl_le_p(record + INFERNO_METAL_COMPILER_METADATA_FLAGS_OFFSET, flags);
    stl_le_p(record + INFERNO_METAL_COMPILER_METADATA_NAME_LENGTH_OFFSET,
             [name length]);
    memcpy(record + INFERNO_METAL_COMPILER_METADATA_NAME_OFFSET, [name bytes],
           [name length]);
}

static uint32_t metal_query_string(uint8_t *output, uint32_t offset,
                                   uint32_t capacity, NSString *string,
                                   uint32_t truncated_flag, uint32_t *flags)
{
    NSData *data = [string dataUsingEncoding:NSUTF8StringEncoding];
    const uint8_t *bytes = [data bytes];
    NSUInteger full_length = [data length];
    NSUInteger length = full_length;

    if (length >= capacity) {
        length = capacity - 1;
        while (length && (bytes[length] & 0xc0) == 0x80) {
            length--;
        }
        *flags |= truncated_flag;
    }
    if (length) {
        memcpy(output + offset, bytes, length);
    }
    return length;
}

static void metal_query_initialize(const InfernoMetalCommand *c,
                                   uint8_t *output)
{
    memset(output, 0, c->output_size);
    stl_le_p(output + INFERNO_METAL_COMPILER_VERSION_OFFSET,
             INFERNO_METAL_VERSION);
    stl_le_p(output + INFERNO_METAL_COMPILER_OPCODE_OFFSET, c->opcode);
    stq_le_p(output + INFERNO_METAL_COMPILER_SEQUENCE_OFFSET, c->sequence);
}

static bool metal_query_finish(const InfernoMetalCommand *c, uint8_t *output,
                               uint32_t outcome, uint32_t phase, NSError *error,
                               NSString *explanation, bool warning)
{
    uint32_t flags = 0;

    stl_le_p(output + INFERNO_METAL_COMPILER_OUTCOME_OFFSET, outcome);
    stl_le_p(output + INFERNO_METAL_COMPILER_PHASE_OFFSET, phase);
    if (error) {
        int64_t code = (int64_t)[error code];
        uint32_t domain_length = metal_query_string(
            output, INFERNO_METAL_COMPILER_DOMAIN_OFFSET,
            INFERNO_METAL_COMPILER_DOMAIN_SIZE, [error domain],
            INFERNO_METAL_COMPILER_FLAG_DOMAIN_TRUNCATED, &flags);
        uint32_t description_length = metal_query_string(
            output, INFERNO_METAL_COMPILER_DESCRIPTION_OFFSET,
            INFERNO_METAL_COMPILER_DESCRIPTION_SIZE,
            [error localizedDescription],
            INFERNO_METAL_COMPILER_FLAG_DESCRIPTION_TRUNCATED, &flags);

        stq_le_p(output + INFERNO_METAL_COMPILER_ERROR_CODE_OFFSET,
                 (uint64_t)code);
        stl_le_p(output + INFERNO_METAL_COMPILER_DOMAIN_LENGTH_OFFSET,
                 domain_length);
        stl_le_p(output + INFERNO_METAL_COMPILER_DESCRIPTION_LENGTH_OFFSET,
                 description_length);
        if (warning) {
            flags |= INFERNO_METAL_COMPILER_FLAG_WARNING;
        }
    } else {
        flags |= INFERNO_METAL_COMPILER_FLAG_NO_NSERROR;
        if (explanation) {
            uint32_t description_length = metal_query_string(
                output, INFERNO_METAL_COMPILER_DESCRIPTION_OFFSET,
                INFERNO_METAL_COMPILER_DESCRIPTION_SIZE, explanation,
                INFERNO_METAL_COMPILER_FLAG_DESCRIPTION_TRUNCATED, &flags);
            stl_le_p(output + INFERNO_METAL_COMPILER_DESCRIPTION_LENGTH_OFFSET,
                     description_length);
        }
    }
    stl_le_p(output + INFERNO_METAL_COMPILER_FLAGS_OFFSET, flags);
    trace_inferno_metal_backend_query(c->sequence, c->opcode, outcome, phase);
    return true;
}

static bool metal_query_pipeline_limits(const InfernoMetalCommand *c,
                                        id<MTLComputePipelineState> pipeline,
                                        uint8_t *output, char *message,
                                        size_t message_size)
{
    NSUInteger maximum = [pipeline maxTotalThreadsPerThreadgroup];
    NSUInteger width = [pipeline threadExecutionWidth];
    NSUInteger memory = [pipeline staticThreadgroupMemoryLength];
    NSUInteger allocated = [pipeline allocatedSize];
    MTLSize required;
    NSInteger validation;
    BOOL indirect;

    if (@available(macOS 26.0, *)) {
        required = [pipeline requiredThreadsPerThreadgroup];
    } else {
        snprintf(message, message_size,
                 "requiredThreadsPerThreadgroup is unavailable");
        return false;
    }
    if (@available(macOS 15.0, *)) {
        validation = [pipeline shaderValidation];
    } else {
        snprintf(message, message_size, "shaderValidation is unavailable");
        return false;
    }
    if (@available(macOS 11.0, *)) {
        indirect = [pipeline supportIndirectCommandBuffers];
    } else {
        snprintf(message, message_size,
                 "supportIndirectCommandBuffers is unavailable");
        return false;
    }

    if (!maximum || maximum > UINT32_MAX || !width || width > UINT32_MAX ||
        memory > UINT32_MAX) {
        snprintf(message, message_size,
                 "compute pipeline limits are outside the v3 wire range");
        return false;
    }
    if (validation < MTLShaderValidationDefault ||
        validation > MTLShaderValidationDisabled) {
        snprintf(message, message_size,
                 "compute pipeline shader validation is unknown");
        return false;
    }
    stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_TYPE_OFFSET,
             INFERNO_METAL_FUNCTION_TYPE_KERNEL);
    stl_le_p(output + INFERNO_METAL_COMPILER_MAX_TOTAL_THREADS_OFFSET, maximum);
    stl_le_p(output + INFERNO_METAL_COMPILER_THREAD_EXECUTION_WIDTH_OFFSET,
             width);
    stl_le_p(output + INFERNO_METAL_COMPILER_STATIC_THREADGROUP_MEMORY_OFFSET,
             memory);
    stq_le_p(output + INFERNO_METAL_COMPILER_PIPELINE_ALLOCATED_SIZE_OFFSET,
             allocated);
    stq_le_p(output + INFERNO_METAL_COMPILER_PIPELINE_REQUIRED_WIDTH_OFFSET,
             required.width);
    stq_le_p(output + INFERNO_METAL_COMPILER_PIPELINE_REQUIRED_HEIGHT_OFFSET,
             required.height);
    stq_le_p(output + INFERNO_METAL_COMPILER_PIPELINE_REQUIRED_DEPTH_OFFSET,
             required.depth);
    stq_le_p(output + INFERNO_METAL_COMPILER_PIPELINE_SHADER_VALIDATION_OFFSET,
             (uint64_t)(int64_t)validation);
    if (c->opcode == INFERNO_METAL_QUERY_IMAGEBLOCK) {
        NSUInteger length = [pipeline
            imageblockMemoryLengthForDimensions:MTLSizeMake(c->width, c->height,
                                                            c->depth)];
        stq_le_p(
            output +
                INFERNO_METAL_COMPILER_PIPELINE_IMAGEBLOCK_MEMORY_LENGTH_OFFSET,
            length);
    }
    if (indirect) {
        stl_le_p(
            output + INFERNO_METAL_COMPILER_PIPELINE_FLAGS_OFFSET,
            INFERNO_METAL_COMPILER_PIPELINE_FLAG_SUPPORTS_INDIRECT_COMMAND_BUFFERS);
    }
    return true;
}

bool inferno_metal_backend_query(InfernoMetalBackend *backend,
                                 const InfernoMetalCommand *c,
                                 const uint8_t *source, uint8_t *output,
                                 char *message, size_t message_size)
{
    @autoreleasepool {
        int64_t phase_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        NSError *library_error = nil;
        NSString *text =
            [[[NSString alloc] initWithBytes:source
                                      length:c->source_size
                                    encoding:NSUTF8StringEncoding] autorelease];

        if (!text) {
            return metal_error(message, message_size, "UTF-8 shader", nil);
        }
        metal_query_initialize(c, output);

        NSString *function_name = nil;
        NSArray *pipeline_key = nil;
        id<MTLComputePipelineState> pipeline = nil;
        NSError *pipeline_warning = nil;
        NSData *library_key =
            [NSData dataWithBytes:source length:c->source_size];
        if (c->opcode == INFERNO_METAL_QUERY_PIPELINE ||
            c->opcode == INFERNO_METAL_QUERY_IMAGEBLOCK) {
            function_name = [NSString stringWithUTF8String:c->function];
            if (!function_name) {
                return metal_error(message, message_size, "UTF-8 function name",
                                   nil);
            }
            pipeline_key =
                @[ @(INFERNO_METAL_COMPUTE), library_key, function_name, @"" ];
            pipeline =
                metal_pipeline_lookup(backend, pipeline_key, &pipeline_warning);
            trace_inferno_metal_pipeline_cache(c->sequence, c->opcode,
                                               pipeline != nil,
                                               [backend->pipeline_keys count]);
            if (pipeline) {
                if (!metal_query_pipeline_limits(c, pipeline, output, message,
                                                 message_size)) {
                    return false;
                }
                metal_phase(c, "query-pipeline", &phase_start);
                return metal_query_finish(
                    c, output, INFERNO_METAL_COMPILER_OUTCOME_OK,
                    INFERNO_METAL_COMPILER_PHASE_PIPELINE, pipeline_warning,
                    nil, pipeline_warning != nil);
            }
        }

        id<MTLLibrary> library =
            metal_library_lookup(backend, library_key, &library_error);
        if (!library) {
            library = [[backend->device newLibraryWithSource:text
                                                     options:nil
                                                       error:&library_error]
                autorelease];
            if (library) {
                metal_library_insert(backend, library_key, library,
                                     library_error);
            }
        }
        if (!library) {
            metal_phase(c, "query-library", &phase_start);
            return metal_query_finish(
                c, output, INFERNO_METAL_COMPILER_OUTCOME_COMPILE_FAILED,
                INFERNO_METAL_COMPILER_PHASE_LIBRARY, library_error,
                @"Metal returned no library and no NSError", false);
        }
        metal_phase(c, "query-library", &phase_start);

        if (c->opcode == INFERNO_METAL_QUERY_LIBRARY) {
            NSArray<NSString *> *names = [library functionNames];
            NSUInteger count = [names count];
            NSMutableArray *functions = [NSMutableArray array];
            NSMutableArray *constant_lists = [NSMutableArray array];
            NSMutableArray *vertex_lists = [NSMutableArray array];
            NSMutableArray *stage_lists = [NSMutableArray array];
            NSMutableSet *seen = [NSMutableSet set];
            NSString *reason = nil;
            uint64_t metadata_count = 0;
            MTLFunctionType unused_type;
            (void)unused_type;

            if (count > UINT32_MAX) {
                return metal_error(message, message_size,
                                   "function inventory count exceeds uint32",
                                   nil);
            }
            stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_COUNT_OFFSET,
                     count);
            for (NSString *name in names) {
                NSData *name_data = metal_bounded_string(
                    name, INFERNO_METAL_COMPILER_FUNCTION_NAME_SIZE, false);
                id<MTLFunction> function =
                    [[library newFunctionWithName:name] autorelease];
                uint32_t type;
                NSDictionary *dictionary =
                    [function functionConstantsDictionary];
                NSArray *constants =
                    dictionary ? metal_sorted_constants(dictionary, &reason) :
                                 @[];
                NSArray *vertices = [function vertexAttributes];
                NSArray *stages = [function stageInputAttributes];
                if (!name_data || [seen containsObject:name] || !function ||
                    !metal_function_type(&type, [function functionType]) ||
                    [function patchType] > MTLPatchTypeQuad ||
                    ([function options] & ~0xfUL) ||
                    !metal_validate_metadata(constants, true, &reason) ||
                    (vertices &&
                     !metal_validate_metadata(vertices, false, &reason)) ||
                    (stages &&
                     !metal_validate_metadata(stages, false, &reason))) {
                    reason =
                        reason ?:
                            @"function inventory contains unsupported metadata";
                    break;
                }
                [seen addObject:name];
                [functions addObject:function];
                [constant_lists addObject:constants];
                [vertex_lists addObject:vertices ?: [NSNull null]];
                [stage_lists addObject:stages ?: [NSNull null]];
                metadata_count +=
                    [constants count] + [vertices count] + [stages count];
                if (metadata_count > UINT32_MAX) {
                    reason = @"metadata inventory count exceeds uint32";
                    break;
                }
            }
            stl_le_p(output +
                         INFERNO_METAL_COMPILER_METADATA_RECORD_COUNT_OFFSET,
                     metadata_count);
            if (reason || count > INFERNO_METAL_COMPILER_MAX_FUNCTIONS ||
                metadata_count > INFERNO_METAL_COMPILER_MAX_METADATA_RECORDS) {
                return metal_query_finish(
                    c, output,
                    INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
                    INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                    reason ?: @"function inventory exceeds the v3 maximum",
                    false);
            }
            uint64_t required =
                INFERNO_METAL_COMPILER_MIN_OUTPUT +
                count * INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
                metadata_count * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
            if (required > c->output_size) {
                stl_le_p(output +
                             INFERNO_METAL_COMPILER_REQUIRED_OUTPUT_SIZE_OFFSET,
                         required);
                return metal_query_finish(
                    c, output, INFERNO_METAL_COMPILER_OUTCOME_OUTPUT_TOO_SMALL,
                    INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                    @"output has insufficient inventory capacity", false);
            }

            if (@available(macOS 11.0, *)) {
                MTLLibraryType type = [library type];
                NSString *install_name = [library installName];
                NSData *install_data =
                    install_name ?
                        metal_bounded_string(
                            install_name,
                            INFERNO_METAL_COMPILER_LIBRARY_NAME_SIZE, true) :
                        nil;
                if ((type != MTLLibraryTypeExecutable &&
                     type != MTLLibraryTypeDynamic) ||
                    (install_name && !install_data)) {
                    return metal_query_finish(
                        c, output,
                        INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
                        INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                        @"library metadata is outside the v3 wire range",
                        false);
                }
                stl_le_p(output + INFERNO_METAL_COMPILER_LIBRARY_TYPE_OFFSET,
                         type);
                if (install_name) {
                    stl_le_p(
                        output + INFERNO_METAL_COMPILER_LIBRARY_FLAGS_OFFSET,
                        INFERNO_METAL_COMPILER_LIBRARY_FLAG_HAS_INSTALL_NAME);
                    stl_le_p(
                        output +
                            INFERNO_METAL_COMPILER_LIBRARY_NAME_LENGTH_OFFSET,
                        [install_data length]);
                    memcpy(output + INFERNO_METAL_COMPILER_LIBRARY_NAME_OFFSET,
                           [install_data bytes], [install_data length]);
                }
            } else {
                return metal_query_finish(
                    c, output,
                    INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
                    INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                    @"library metadata is unavailable", false);
            }

            uint8_t *record = output + INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET;
            for (NSUInteger i = 0; i < count; i++) {
                id<MTLFunction> function = [functions objectAtIndex:i];
                NSData *name_data =
                    [[function name] dataUsingEncoding:NSUTF8StringEncoding];
                NSArray *constants = [constant_lists objectAtIndex:i];
                id vertex_value = [vertex_lists objectAtIndex:i];
                id stage_value = [stage_lists objectAtIndex:i];
                NSArray *vertices =
                    vertex_value == [NSNull null] ? nil : vertex_value;
                NSArray *stages =
                    stage_value == [NSNull null] ? nil : stage_value;
                uint32_t type;
                metal_function_type(&type, [function functionType]);
                uint32_t flags =
                    (vertices ?
                         INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_VERTEX_ATTRIBUTES :
                         0) |
                    (stages ?
                         INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_STAGE_INPUT_ATTRIBUTES :
                         0);
                uint32_t records = (uint32_t)(
                    [constants count] + [vertices count] + [stages count]);
                uint32_t record_size =
                    INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
                    records * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
                stl_le_p(record + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET,
                         type);
                stl_le_p(record +
                             INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET,
                         [name_data length]);
                stl_le_p(record +
                             INFERNO_METAL_COMPILER_RECORD_PATCH_TYPE_OFFSET,
                         [function patchType]);
                stl_le_p(record + INFERNO_METAL_COMPILER_RECORD_FLAGS_OFFSET,
                         flags);
                stq_le_p(
                    record +
                        INFERNO_METAL_COMPILER_RECORD_PATCH_CONTROL_POINT_COUNT_OFFSET,
                    (uint64_t)(int64_t)[function patchControlPointCount]);
                stq_le_p(record + INFERNO_METAL_COMPILER_RECORD_OPTIONS_OFFSET,
                         [function options]);
                stl_le_p(
                    record +
                        INFERNO_METAL_COMPILER_RECORD_CONSTANT_COUNT_OFFSET,
                    [constants count]);
                stl_le_p(
                    record +
                        INFERNO_METAL_COMPILER_RECORD_VERTEX_ATTRIBUTE_COUNT_OFFSET,
                    [vertices count]);
                stl_le_p(
                    record +
                        INFERNO_METAL_COMPILER_RECORD_STAGE_INPUT_ATTRIBUTE_COUNT_OFFSET,
                    [stages count]);
                stl_le_p(record + INFERNO_METAL_COMPILER_RECORD_SIZE_OFFSET,
                         record_size);
                memcpy(record + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
                       [name_data bytes], [name_data length]);
                uint8_t *metadata =
                    record + INFERNO_METAL_COMPILER_RECORD_METADATA_OFFSET;
                for (id item in constants) {
                    metal_write_metadata(
                        metadata, INFERNO_METAL_COMPILER_METADATA_CONSTANT,
                        item);
                    metadata += INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
                }
                for (id item in vertices) {
                    metal_write_metadata(
                        metadata,
                        INFERNO_METAL_COMPILER_METADATA_VERTEX_ATTRIBUTE, item);
                    metadata += INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
                }
                for (id item in stages) {
                    metal_write_metadata(
                        metadata,
                        INFERNO_METAL_COMPILER_METADATA_STAGE_INPUT_ATTRIBUTE,
                        item);
                    metadata += INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
                }
                record += record_size;
            }
            metal_phase(c, "query-inventory", &phase_start);
            return metal_query_finish(c, output,
                                      INFERNO_METAL_COMPILER_OUTCOME_OK,
                                      INFERNO_METAL_COMPILER_PHASE_INVENTORY,
                                      library_error, nil, library_error != nil);
        }

        id<MTLFunction> function =
            [[library newFunctionWithName:function_name] autorelease];
        if (!function) {
            return metal_query_finish(
                c, output, INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_NOT_FOUND,
                INFERNO_METAL_COMPILER_PHASE_FUNCTION, nil,
                @"library has no function with the requested name", false);
        }
        uint32_t type;
        if (!metal_function_type(&type, [function functionType])) {
            return metal_error(message, message_size,
                               "function has an unknown Metal type", nil);
        }
        if (type != INFERNO_METAL_FUNCTION_TYPE_KERNEL) {
            stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_TYPE_OFFSET,
                     type);
            return metal_query_finish(
                c, output,
                INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_TYPE_MISMATCH,
                INFERNO_METAL_COMPILER_PHASE_FUNCTION, nil,
                @"requested function is not a compute kernel", false);
        }
        if (metal_function_has_constants(function)) {
            stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_TYPE_OFFSET,
                     INFERNO_METAL_FUNCTION_TYPE_KERNEL);
            return metal_query_finish(
                c, output,
                INFERNO_METAL_COMPILER_OUTCOME_SPECIALIZATION_REQUIRED,
                INFERNO_METAL_COMPILER_PHASE_FUNCTION, nil,
                @"compute function requires specialization", false);
        }

        NSError *pipeline_error = nil;
        pipeline = [[backend->device
            newComputePipelineStateWithFunction:function
                                          error:&pipeline_error] autorelease];
        if (!pipeline) {
            metal_phase(c, "query-pipeline", &phase_start);
            return metal_query_finish(
                c, output, INFERNO_METAL_COMPILER_OUTCOME_COMPILE_FAILED,
                INFERNO_METAL_COMPILER_PHASE_PIPELINE, pipeline_error,
                @"Metal returned no pipeline and no NSError", false);
        }
        if (!metal_query_pipeline_limits(c, pipeline, output, message,
                                         message_size)) {
            return false;
        }
        NSError *diagnostic = pipeline_error ?: library_error;
        metal_pipeline_insert(backend, pipeline_key, pipeline, diagnostic);
        metal_phase(c, "query-pipeline", &phase_start);
        return metal_query_finish(c, output, INFERNO_METAL_COMPILER_OUTCOME_OK,
                                  INFERNO_METAL_COMPILER_PHASE_PIPELINE,
                                  diagnostic, nil, diagnostic != nil);
    }
}

bool inferno_metal_backend_execute(InfernoMetalBackend *backend,
                                   const InfernoMetalCommand *c,
                                   const uint8_t *source, const uint8_t *input,
                                   uint8_t *output, char *message,
                                   size_t message_size)
{
    @autoreleasepool {
        int64_t phase_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        NSError *error = nil;
        id<MTLDevice> device = backend->device;
        id<MTLLibrary> library = nil;
        NSError *library_warning = nil;
        NSArray *pipeline_key = nil;
        id cached_pipeline = nil;
        NSString *function_name = nil;
        NSString *fragment_name = nil;
        id<MTLBuffer> input_buffer = nil;
        id<MTLBuffer> output_buffer = nil;
        id<MTLTexture> texture = nil;
        id<MTLCommandBuffer> command_buffer = [backend->queue commandBuffer];

        if (!command_buffer) {
            return metal_error(message, message_size, "command buffer", nil);
        }
        metal_phase(c, "prepare", &phase_start);
        if (c->opcode != INFERNO_METAL_CLEAR) {
            function_name = [NSString stringWithUTF8String:c->function];
            if (c->opcode == INFERNO_METAL_RENDER) {
                fragment_name = [NSString stringWithUTF8String:c->fragment];
            }
            if (!function_name ||
                (c->opcode == INFERNO_METAL_RENDER && !fragment_name)) {
                return metal_error(message, message_size, "UTF-8 function name",
                                   nil);
            }
            NSString *text = [[[NSString alloc]
                initWithBytes:source
                       length:c->source_size
                     encoding:NSUTF8StringEncoding] autorelease];
            if (!text) {
                return metal_error(message, message_size, "UTF-8 shader", nil);
            }
            /* Copy exact source bytes; never retain a guest or work pointer.
             * Protocol v2 fixes all other pipeline state (including BGRA8).
             */
            pipeline_key = @[
                @(c->opcode),
                [NSData dataWithBytes:source length:c->source_size],
                function_name, fragment_name ?: @""
            ];
            NSError *cached_warning = nil;
            cached_pipeline =
                metal_pipeline_lookup(backend, pipeline_key, &cached_warning);
            (void)cached_warning;
            trace_inferno_metal_pipeline_cache(c->sequence, c->opcode,
                                               cached_pipeline != nil,
                                               [backend->pipeline_keys count]);
            if (!cached_pipeline) {
                NSData *library_key =
                    [NSData dataWithBytes:source length:c->source_size];
                library = metal_library_lookup(backend, library_key,
                                               &library_warning);
                if (!library) {
                    library =
                        [[device newLibraryWithSource:text
                                              options:nil
                                                error:&error] autorelease];
                    library_warning = error;
                    if (library) {
                        metal_library_insert(backend, library_key, library,
                                             library_warning);
                    }
                }
                if (!library) {
                    return metal_error(message, message_size, "shader library",
                                       error);
                }
            }
            metal_phase(c, "library", &phase_start);
        }
        if (c->input_size) {
            input_buffer = [[device
                newBufferWithBytes:input
                            length:c->input_size
                           options:MTLResourceStorageModeShared] autorelease];
            if (!input_buffer) {
                return metal_error(message, message_size, "input buffer", nil);
            }
        }
        metal_phase(c, "input", &phase_start);
        if (c->opcode == INFERNO_METAL_COMPUTE) {
            id<MTLComputePipelineState> pipeline = cached_pipeline;
            if (!pipeline) {
                id<MTLFunction> function =
                    [[library newFunctionWithName:function_name] autorelease];
                if (!function ||
                    function.functionType != MTLFunctionTypeKernel) {
                    return metal_error(message, message_size,
                                       "compute function", nil);
                }
                if (metal_function_has_constants(function)) {
                    return metal_error(
                        message, message_size,
                        "compute function requires specialization", nil);
                }
                pipeline = [[device newComputePipelineStateWithFunction:function
                                                                  error:&error]
                    autorelease];
                if (!pipeline) {
                    return metal_error(message, message_size,
                                       "compute pipeline", error);
                }
                if (![pipeline maxTotalThreadsPerThreadgroup] ||
                    [pipeline maxTotalThreadsPerThreadgroup] > UINT32_MAX ||
                    ![pipeline threadExecutionWidth] ||
                    [pipeline threadExecutionWidth] > UINT32_MAX ||
                    [pipeline staticThreadgroupMemoryLength] > UINT32_MAX) {
                    return metal_error(message, message_size,
                                       "compute pipeline limits", nil);
                }
                metal_pipeline_insert(backend, pipeline_key, pipeline,
                                      error ?: library_warning);
            }
            metal_phase(c, "compute-pipeline", &phase_start);
            output_buffer = [[device
                newBufferWithLength:c->output_size
                            options:MTLResourceStorageModeShared] autorelease];
            if (!output_buffer) {
                return metal_error(message, message_size, "output buffer", nil);
            }
            memset([output_buffer contents], 0, c -> output_size);
            id<MTLComputeCommandEncoder> encoder =
                [command_buffer computeCommandEncoder];
            if (!encoder) {
                return metal_error(message, message_size, "compute encoder",
                                   nil);
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:input_buffer offset:0 atIndex:0];
            [encoder setBuffer:output_buffer offset:0 atIndex:1];
            NSUInteger group_width =
                MIN(c->width, MIN([pipeline threadExecutionWidth],
                                  [pipeline maxTotalThreadsPerThreadgroup]));
            [encoder dispatchThreads:MTLSizeMake(c->width, c->height, c->depth)
                threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
            [encoder endEncoding];
            metal_phase(c, "compute-encode", &phase_start);
        } else {
            MTLTextureDescriptor *description = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                             width:c->width
                                            height:c->height
                                         mipmapped:NO];
            description.storageMode = MTLStorageModeShared;
            description.usage = MTLTextureUsageRenderTarget;
            texture =
                [[device newTextureWithDescriptor:description] autorelease];
            if (!texture) {
                return metal_error(message, message_size, "render texture",
                                   nil);
            }
            MTLRenderPassDescriptor *pass =
                [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            MTLClearColor color = MTLClearColorMake(0, 0, 0, 0);
            if (c->opcode == INFERNO_METAL_CLEAR) {
                /* Wire colors are four little-endian IEEE float32 values. */
                float rgba[4];
                for (unsigned i = 0; i < 4; i++) {
                    uint32_t bits = ldl_le_p(input + i * 4);
                    memcpy(&rgba[i], &bits, sizeof(bits));
                }
                color = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
            }
            pass.colorAttachments[0].clearColor = color;
            id<MTLRenderCommandEncoder> encoder =
                [command_buffer renderCommandEncoderWithDescriptor:pass];
            if (!encoder) {
                return metal_error(message, message_size, "render encoder",
                                   nil);
            }
            metal_phase(c, "render-target", &phase_start);
            if (c->opcode == INFERNO_METAL_RENDER) {
                id<MTLRenderPipelineState> pipeline = cached_pipeline;
                if (!pipeline) {
                    MTLRenderPipelineDescriptor *pipeline_description =
                        [[[MTLRenderPipelineDescriptor alloc] init]
                            autorelease];
                    id<MTLFunction> vertex = [[library
                        newFunctionWithName:function_name] autorelease];
                    id<MTLFunction> fragment = [[library
                        newFunctionWithName:fragment_name] autorelease];
                    if (!vertex || !fragment ||
                        vertex.functionType != MTLFunctionTypeVertex ||
                        fragment.functionType != MTLFunctionTypeFragment) {
                        [encoder endEncoding];
                        return metal_error(message, message_size,
                                           "render functions", nil);
                    }
                    pipeline_description.vertexFunction = vertex;
                    pipeline_description.fragmentFunction = fragment;
                    pipeline_description.colorAttachments[0].pixelFormat =
                        MTLPixelFormatBGRA8Unorm;
                    pipeline =
                        [[device newRenderPipelineStateWithDescriptor:
                                     pipeline_description
                                                                error:&error]
                            autorelease];
                    if (!pipeline) {
                        [encoder endEncoding];
                        return metal_error(message, message_size,
                                           "render pipeline", error);
                    }
                    metal_pipeline_insert(backend, pipeline_key, pipeline,
                                          error ?: library_warning);
                }
                metal_phase(c, "render-pipeline", &phase_start);
                [encoder setRenderPipelineState:pipeline];
                [encoder setVertexBuffer:input_buffer offset:0 atIndex:0];
                [encoder setFragmentBuffer:input_buffer offset:0 atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:0
                            vertexCount:c->depth];
            }
            [encoder endEncoding];
            metal_phase(c, "render-encode", &phase_start);
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        metal_phase(c, "gpu-wait", &phase_start);
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return metal_error(message, message_size, "GPU execution",
                               command_buffer.error);
        }
        if (output_buffer) {
            memcpy(output, [output_buffer contents], c -> output_size);
        } else {
            [texture getBytes:output
                  bytesPerRow:(NSUInteger)c->width * 4
                   fromRegion:MTLRegionMake2D(0, 0, c->width, c->height)
                  mipmapLevel:0];
        }
        metal_phase(c, "readback", &phase_start);
        return true;
    }
}

static void metal_batch_initialize(const InfernoMetalCommand *c,
                                   uint8_t *output)
{
    memset(output, 0, c->output_size);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_VERSION_OFFSET,
             INFERNO_METAL_VERSION);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_OPCODE_OFFSET,
             INFERNO_METAL_BATCH);
    stq_le_p(output + INFERNO_METAL_BATCH_RESULT_SEQUENCE_OFFSET, c->sequence);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FAILED_KIND_OFFSET,
             INFERNO_METAL_BATCH_RECORD_UNKNOWN);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FAILED_INDEX_OFFSET,
             INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN);
}

static bool metal_batch_finish(const InfernoMetalCommand *c, uint8_t *output,
                               uint32_t outcome, uint32_t phase,
                               uint32_t record_kind, uint32_t record_index,
                               NSError *error, NSString *explanation,
                               bool scheduled)
{
    uint32_t flags = scheduled ? INFERNO_METAL_BATCH_FLAG_SCHEDULED : 0;

    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_OUTCOME_OFFSET, outcome);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_PHASE_OFFSET, phase);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FAILED_KIND_OFFSET,
             record_kind);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FAILED_INDEX_OFFSET,
             record_index);
    if (error) {
        int64_t code = (int64_t)[error code];
        uint32_t domain_length = metal_query_string(
            output, INFERNO_METAL_BATCH_RESULT_DOMAIN_OFFSET,
            INFERNO_METAL_COMPILER_DOMAIN_SIZE, [error domain],
            INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED, &flags);
        uint32_t description_length = metal_query_string(
            output, INFERNO_METAL_BATCH_RESULT_DESCRIPTION_OFFSET,
            INFERNO_METAL_COMPILER_DESCRIPTION_SIZE,
            [error localizedDescription],
            INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED, &flags);

        stq_le_p(output + INFERNO_METAL_BATCH_RESULT_ERROR_CODE_OFFSET,
                 (uint64_t)code);
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_DOMAIN_LENGTH_OFFSET,
                 domain_length);
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_DESCRIPTION_LENGTH_OFFSET,
                 description_length);
    } else {
        flags |= INFERNO_METAL_BATCH_FLAG_NO_NSERROR;
        if (explanation) {
            uint32_t description_length = metal_query_string(
                output, INFERNO_METAL_BATCH_RESULT_DESCRIPTION_OFFSET,
                INFERNO_METAL_COMPILER_DESCRIPTION_SIZE, explanation,
                INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED, &flags);
            stl_le_p(output +
                         INFERNO_METAL_BATCH_RESULT_DESCRIPTION_LENGTH_OFFSET,
                     description_length);
        }
    }
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FLAGS_OFFSET, flags);
    trace_inferno_metal_batch_outcome(c->sequence, outcome, phase,
                                      record_index);
    return true;
}

static bool metal_batch_failure(const InfernoMetalCommand *c, uint8_t *output,
                                const InfernoMetalBatchView *view,
                                uint32_t outcome, uint32_t phase, uint32_t kind,
                                uint32_t index, NSError *error,
                                NSString *explanation)
{
    if (view) {
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_BUFFER_COUNT_OFFSET,
                 view->buffer_count);
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_IMAGES_SIZE_OFFSET,
                 view->images_size);
    }
    return metal_batch_finish(c, output, outcome, phase, kind, index, error,
                              explanation, false);
}

static id<MTLComputePipelineState>
metal_batch_pipeline(InfernoMetalBackend *backend, const InfernoMetalCommand *c,
                     const InfernoMetalBatchView *view, uint32_t index,
                     uint8_t *output, bool *delivered, char *message,
                     size_t message_size)
{
    const uint8_t *record = inferno_metal_batch_pipeline(view, index);
    uint32_t source_offset =
        ldl_le_p(record + INFERNO_METAL_BATCH_PIPELINE_SOURCE_OFFSET);
    uint32_t source_size =
        ldl_le_p(record + INFERNO_METAL_BATCH_PIPELINE_SOURCE_SIZE_OFFSET);
    const uint8_t *source = view->bytes + view->source_offset + source_offset;
    NSString *name = [NSString
        stringWithUTF8String:(const char *)record +
                             INFERNO_METAL_BATCH_PIPELINE_NAME_OFFSET];
    NSData *source_data = [NSData dataWithBytes:source length:source_size];
    NSArray *key = @[ @(INFERNO_METAL_COMPUTE), source_data, name, @"" ];
    NSError *warning = nil;
    id<MTLComputePipelineState> pipeline =
        metal_pipeline_lookup(backend, key, &warning);

    trace_inferno_metal_pipeline_cache(c->sequence, INFERNO_METAL_BATCH,
                                       pipeline != nil,
                                       [backend->pipeline_keys count]);
    if (pipeline) {
        return pipeline;
    }
    NSError *library_error = nil;
    id<MTLLibrary> library =
        metal_library_lookup(backend, source_data, &library_error);
    if (!library) {
        NSString *text =
            [[[NSString alloc] initWithBytes:source
                                      length:source_size
                                    encoding:NSUTF8StringEncoding] autorelease];
        library =
            [[backend->device newLibraryWithSource:text
                                           options:nil
                                             error:&library_error] autorelease];
        if (library) {
            metal_library_insert(backend, source_data, library, library_error);
        }
    }
    if (!library) {
        *delivered = metal_batch_failure(
            c, output, view, INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED,
            INFERNO_METAL_BATCH_PHASE_LIBRARY,
            INFERNO_METAL_BATCH_RECORD_PIPELINE, index, library_error,
            @"Metal returned no library and no NSError");
        return nil;
    }
    id<MTLFunction> function = [[library newFunctionWithName:name] autorelease];
    if (!function) {
        *delivered = metal_batch_failure(
            c, output, view, INFERNO_METAL_BATCH_OUTCOME_FUNCTION_NOT_FOUND,
            INFERNO_METAL_BATCH_PHASE_FUNCTION,
            INFERNO_METAL_BATCH_RECORD_PIPELINE, index, nil,
            @"library has no function with the requested name");
        return nil;
    }
    uint32_t type;
    if (!metal_function_type(&type, [function functionType])) {
        snprintf(message, message_size, "function has unknown Metal type");
        return nil;
    }
    if (type != INFERNO_METAL_FUNCTION_TYPE_KERNEL) {
        *delivered = metal_batch_failure(
            c, output, view, INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH,
            INFERNO_METAL_BATCH_PHASE_FUNCTION,
            INFERNO_METAL_BATCH_RECORD_PIPELINE, index, nil,
            @"requested function is not a compute kernel");
        return nil;
    }
    if (metal_function_has_constants(function)) {
        *delivered = metal_batch_failure(
            c, output, view,
            INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED,
            INFERNO_METAL_BATCH_PHASE_FUNCTION,
            INFERNO_METAL_BATCH_RECORD_PIPELINE, index, nil,
            @"compute function requires specialization");
        return nil;
    }
    NSError *pipeline_error = nil;
    pipeline = [[backend->device
        newComputePipelineStateWithFunction:function
                                      error:&pipeline_error] autorelease];
    if (!pipeline) {
        *delivered = metal_batch_failure(
            c, output, view, INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED,
            INFERNO_METAL_BATCH_PHASE_PIPELINE,
            INFERNO_METAL_BATCH_RECORD_PIPELINE, index, pipeline_error,
            @"Metal returned no pipeline and no NSError");
        return nil;
    }
    if (![pipeline maxTotalThreadsPerThreadgroup] ||
        [pipeline maxTotalThreadsPerThreadgroup] > UINT32_MAX ||
        ![pipeline threadExecutionWidth] ||
        [pipeline threadExecutionWidth] > UINT32_MAX ||
        [pipeline staticThreadgroupMemoryLength] > UINT32_MAX) {
        snprintf(message, message_size,
                 "compute pipeline limits are outside the v4 wire range");
        return nil;
    }
    metal_pipeline_insert(backend, key, pipeline,
                          pipeline_error ?: library_error);
    return pipeline;
}

static bool metal_product_within(uint32_t a, uint32_t b, uint32_t d,
                                 NSUInteger limit)
{
    if (!a || !b || !d || a > limit || b > limit / a) {
        return false;
    }
    NSUInteger ab = (NSUInteger)a * b;
    return d <= limit / ab;
}

@interface InfernoMetalScheduledObserver : NSObject {
  @public
    QemuMutex lock;
    InfernoMetalProgressFn callback;
    void *opaque;
    bool armed;
    bool scheduled;
    dispatch_semaphore_t completed;
}
- (instancetype)initWithCallback:(InfernoMetalProgressFn)callback
                          opaque:(void *)opaque;
@end

@implementation InfernoMetalScheduledObserver
- (instancetype)initWithCallback:(InfernoMetalProgressFn)new_callback
                          opaque:(void *)new_opaque
{
    self = [super init];
    if (self) {
        qemu_mutex_init(&lock);
        completed = dispatch_semaphore_create(0);
        if (!completed) {
            [self release];
            return nil;
        }
        callback = new_callback;
        opaque = new_opaque;
        armed = true;
    }
    return self;
}

- (void)dealloc
{
    g_assert(!armed);
    g_assert(!callback);
    g_assert(!opaque);
    qemu_mutex_destroy(&lock);
    if (completed)
        dispatch_release(completed);
    [super dealloc];
}
@end

bool inferno_metal_backend_batch(InfernoMetalBackend *backend,
                                 const InfernoMetalCommand *c,
                                 const uint8_t *input, uint8_t *output,
                                 InfernoMetalProgressFn progress, void *opaque,
                                 char *message, size_t message_size)
{
    @autoreleasepool {
        InfernoMetalBatchView view;
        InfernoMetalBatchParseError parse_error;
        metal_batch_initialize(c, output);
        if (!inferno_metal_batch_parse(input, c->input_size, c->output_size,
                                       &view, &parse_error)) {
            return metal_batch_finish(
                c, output, INFERNO_METAL_BATCH_OUTCOME_MALFORMED,
                INFERNO_METAL_BATCH_PHASE_PARSE, parse_error.record_kind,
                parse_error.record_index, nil, @"malformed batch manifest",
                false);
        }
        if (![backend->device supportsFamily:MTLGPUFamilyApple1]) {
            return metal_batch_failure(
                c, output, &view, INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST,
                INFERNO_METAL_BATCH_PHASE_VALIDATE,
                INFERNO_METAL_BATCH_RECORD_HEADER, 0, nil,
                @"host GPU family does not support the batch alignment policy");
        }
        NSMutableArray *pipelines =
            [NSMutableArray arrayWithCapacity:view.pipeline_count];
        for (uint32_t i = 0; i < view.pipeline_count; i++) {
            bool delivered = false;
            id pipeline =
                metal_batch_pipeline(backend, c, &view, i, output, &delivered,
                                     message, message_size);
            if (!pipeline) {
                return delivered;
            }
            [pipelines addObject:pipeline];
        }
        MTLSize device_max = [backend->device maxThreadsPerThreadgroup];
        NSUInteger device_memory = [backend->device maxThreadgroupMemoryLength];
        for (uint32_t i = 0; i < view.dispatch_count; i++) {
            const uint8_t *d = inferno_metal_batch_dispatch(&view, i);
            id<MTLComputePipelineState> pipeline = [pipelines
                objectAtIndex:
                    ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_PIPELINE_OFFSET)];
            uint32_t gw =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GROUP_WIDTH_OFFSET);
            uint32_t gh =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GROUP_HEIGHT_OFFSET);
            uint32_t gd =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GROUP_DEPTH_OFFSET);
            NSUInteger maximum = [pipeline maxTotalThreadsPerThreadgroup];
            MTLSize required = [pipeline requiredThreadsPerThreadgroup];
            if (!metal_product_within(gw, gh, gd, maximum) ||
                gw > device_max.width || gh > device_max.height ||
                gd > device_max.depth ||
                !metal_product_within(gw, gh, gd, NSUIntegerMax) ||
                (required.width &&
                 (gw != required.width || gh != required.height ||
                  gd != required.depth))) {
                return metal_batch_failure(
                    c, output, &view,
                    INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH,
                    INFERNO_METAL_BATCH_PHASE_VALIDATE,
                    INFERNO_METAL_BATCH_RECORD_DISPATCH, i, nil,
                    @"threadgroup dimensions exceed pipeline or device limits");
            }
            uint64_t memory = [pipeline staticThreadgroupMemoryLength];
            if (memory > device_memory) {
                return metal_batch_failure(
                    c, output, &view,
                    INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH,
                    INFERNO_METAL_BATCH_PHASE_VALIDATE,
                    INFERNO_METAL_BATCH_RECORD_DISPATCH, i, nil,
                    @"static threadgroup memory exceeds the device limit");
            }
            uint32_t start =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_BINDING_START_OFFSET);
            uint32_t count =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_BINDING_COUNT_OFFSET);
            for (uint32_t j = 0; j < count; j++) {
                const uint8_t *b =
                    inferno_metal_batch_binding(&view, start + j);
                if (ldl_le_p(b + INFERNO_METAL_BATCH_BINDING_KIND_OFFSET) ==
                    INFERNO_METAL_BATCH_BINDING_THREADGROUP) {
                    memory +=
                        ldl_le_p(b + INFERNO_METAL_BATCH_BINDING_LENGTH_OFFSET);
                    if (memory > device_memory) {
                        return metal_batch_failure(
                            c, output, &view,
                            INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH,
                            INFERNO_METAL_BATCH_PHASE_VALIDATE,
                            INFERNO_METAL_BATCH_RECORD_BINDING, start + j, nil,
                            @"threadgroup memory exceeds the device limit");
                    }
                }
            }
        }

        NSMutableArray *buffers =
            [NSMutableArray arrayWithCapacity:view.buffer_count];
        size_t image_cursor = 0;
        for (uint32_t i = 0; i < view.buffer_count; i++) {
            const uint8_t *record = inferno_metal_batch_buffer(&view, i);
            uint32_t length =
                ldl_le_p(record + INFERNO_METAL_BATCH_BUFFER_LENGTH_OFFSET);
            id<MTLBuffer> buffer = [[backend->device
                newBufferWithBytes:input + view.images_offset + image_cursor
                            length:length
                           options:MTLResourceStorageModeShared |
                                   MTLResourceHazardTrackingModeTracked]
                autorelease];
            if (!buffer) {
                return metal_error(message, message_size, "batch buffer", nil);
            }
            [buffers addObject:buffer];
            image_cursor += length;
        }
        MTLCommandBufferDescriptor *descriptor =
            [[[MTLCommandBufferDescriptor alloc] init] autorelease];
        descriptor.errorOptions =
            MTLCommandBufferErrorOptionEncoderExecutionStatus;
        id<MTLCommandBuffer> command_buffer =
            [backend->queue commandBufferWithDescriptor:descriptor];
        if (!command_buffer) {
            return metal_error(message, message_size, "batch command buffer",
                               nil);
        }
        for (uint32_t i = 0; i < view.dispatch_count; i++) {
            const uint8_t *d = inferno_metal_batch_dispatch(&view, i);
            id<MTLComputeCommandEncoder> encoder = [command_buffer
                computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
            if (!encoder) {
                return metal_error(message, message_size,
                                   "batch compute encoder", nil);
            }
            encoder.label =
                [NSString stringWithFormat:@"Inferno dispatch %u", i];
            uint32_t pipeline_id =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_PIPELINE_OFFSET);
            [encoder
                setComputePipelineState:[pipelines objectAtIndex:pipeline_id]];
            uint32_t start =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_BINDING_START_OFFSET);
            uint32_t count =
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_BINDING_COUNT_OFFSET);
            for (uint32_t j = 0; j < count; j++) {
                const uint8_t *b =
                    inferno_metal_batch_binding(&view, start + j);
                uint32_t kind =
                    ldl_le_p(b + INFERNO_METAL_BATCH_BINDING_KIND_OFFSET);
                uint32_t index =
                    ldl_le_p(b + INFERNO_METAL_BATCH_BINDING_INDEX_OFFSET);
                uint32_t resource =
                    ldl_le_p(b + INFERNO_METAL_BATCH_BINDING_RESOURCE_OFFSET);
                uint32_t length =
                    ldl_le_p(b + INFERNO_METAL_BATCH_BINDING_LENGTH_OFFSET);
                uint64_t binding_offset =
                    ldq_le_p(b + INFERNO_METAL_BATCH_BINDING_OFFSET_OFFSET);
                if (kind == INFERNO_METAL_BATCH_BINDING_BUFFER) {
                    [encoder setBuffer:[buffers objectAtIndex:resource]
                                offset:(NSUInteger)binding_offset
                               atIndex:index];
                } else if (kind == INFERNO_METAL_BATCH_BINDING_INLINE) {
                    [encoder setBytes:input + view.inline_offset + resource
                               length:length
                              atIndex:index];
                } else {
                    [encoder setThreadgroupMemoryLength:length atIndex:index];
                }
            }
            MTLSize grid = MTLSizeMake(
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GRID_WIDTH_OFFSET),
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GRID_HEIGHT_OFFSET),
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GRID_DEPTH_OFFSET));
            MTLSize group = MTLSizeMake(
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GROUP_WIDTH_OFFSET),
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GROUP_HEIGHT_OFFSET),
                ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_GROUP_DEPTH_OFFSET));
            if (ldl_le_p(d + INFERNO_METAL_BATCH_DISPATCH_MODE_OFFSET) ==
                INFERNO_METAL_BATCH_DISPATCH_THREADS) {
                [encoder dispatchThreads:grid threadsPerThreadgroup:group];
            } else {
                [encoder dispatchThreadgroups:grid threadsPerThreadgroup:group];
            }
            [encoder endEncoding];
        }

        InfernoMetalScheduledObserver *observer =
            [[InfernoMetalScheduledObserver alloc] initWithCallback:progress
                                                             opaque:opaque];
        if (!observer) {
            return metal_error(message, message_size, "batch observer", nil);
        }
        [command_buffer addScheduledHandler:^(id<MTLCommandBuffer> ignored) {
          (void)ignored;
          qemu_mutex_lock(&observer->lock);
          if (observer->armed) {
              observer->scheduled = true;
              if (observer->callback) {
                  observer->callback(observer->opaque,
                                     INFERNO_METAL_PROGRESS_SCHEDULED);
              }
          }
          qemu_mutex_unlock(&observer->lock);
        }];
        [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> ignored) {
          (void)ignored;
          dispatch_semaphore_signal(observer->completed);
        }];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        dispatch_semaphore_wait(observer->completed, DISPATCH_TIME_FOREVER);
        qemu_mutex_lock(&observer->lock);
        bool scheduled = observer->scheduled;
        observer->armed = false;
        observer->callback = NULL;
        observer->opaque = NULL;
        qemu_mutex_unlock(&observer->lock);
        [observer release];

        MTLCommandBufferStatus status = command_buffer.status;
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_BUFFER_COUNT_OFFSET,
                 view.buffer_count);
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_IMAGES_SIZE_OFFSET,
                 view.images_size);
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_HOST_STATUS_OFFSET,
                 status);
        if (status != MTLCommandBufferStatusCompleted) {
            uint32_t failed_index = INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN;
            NSArray *infos = [command_buffer.error.userInfo
                objectForKey:MTLCommandBufferEncoderInfoErrorKey];
            for (unsigned pass = 0;
                 pass < 2 &&
                 failed_index == INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN;
                 pass++) {
                MTLCommandEncoderErrorState wanted =
                    pass ? MTLCommandEncoderErrorStateAffected :
                           MTLCommandEncoderErrorStateFaulted;
                for (id info in infos) {
                    if ([info errorState] != wanted) {
                        continue;
                    }
                    NSString *label = [info label];
                    if ([label hasPrefix:@"Inferno dispatch "]) {
                        unsigned long value =
                            strtoul([[label substringFromIndex:17] UTF8String],
                                    NULL, 10);
                        if (value < view.dispatch_count) {
                            failed_index = (uint32_t)value;
                        }
                    }
                    break;
                }
            }
            return metal_batch_finish(
                c, output, INFERNO_METAL_BATCH_OUTCOME_EXECUTION_FAILED,
                INFERNO_METAL_BATCH_PHASE_EXECUTE,
                failed_index == INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN ?
                    INFERNO_METAL_BATCH_RECORD_UNKNOWN :
                    INFERNO_METAL_BATCH_RECORD_DISPATCH,
                failed_index, command_buffer.error,
                @"Metal execution failed without an NSError", scheduled);
        }
        if (!scheduled) {
            return metal_error(message, message_size,
                               "batch completed without scheduled observation",
                               nil);
        }
        image_cursor = 0;
        for (uint32_t i = 0; i < view.buffer_count; i++) {
            id<MTLBuffer> buffer = [buffers objectAtIndex:i];
            uint32_t length =
                ldl_le_p(inferno_metal_batch_buffer(&view, i) +
                         INFERNO_METAL_BATCH_BUFFER_LENGTH_OFFSET);
            memcpy(output + INFERNO_METAL_BATCH_RESULT_IMAGES_OFFSET +
                       image_cursor,
                   [buffer contents], length);
            image_cursor += length;
        }
        return metal_batch_finish(c, output, INFERNO_METAL_BATCH_OUTCOME_OK,
                                  INFERNO_METAL_BATCH_PHASE_EXECUTE,
                                  INFERNO_METAL_BATCH_RECORD_UNKNOWN,
                                  INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN, nil,
                                  nil, true);
    }
}
