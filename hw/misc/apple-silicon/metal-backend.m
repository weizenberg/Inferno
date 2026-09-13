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

#define METAL_PIPELINE_CACHE_SIZE 32
#define METAL_LIBRARY_CACHE_SIZE 32
#define METAL_ARGUMENT_CACHE_SIZE 32

struct InfernoMetalBackend {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    /* Oldest first. Only the single outstanding worker accesses the cache. */
    NSMutableArray *pipeline_keys;
    NSMutableArray *pipelines;
    NSMutableArray *pipeline_warnings;
    NSMutableArray *pipeline_reflections;
    NSMutableArray *argument_keys;
    NSMutableArray *argument_layouts;
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
        backend->pipeline_reflections = [[NSMutableArray alloc] init];
        backend->argument_keys = [[NSMutableArray alloc] init];
        backend->argument_layouts = [[NSMutableArray alloc] init];
        backend->library_keys = [[NSMutableArray alloc] init];
        backend->libraries = [[NSMutableArray alloc] init];
        backend->library_warnings = [[NSMutableArray alloc] init];
        if (!backend->pipeline_keys || !backend->pipelines ||
            !backend->pipeline_warnings || !backend->pipeline_reflections ||
            !backend->argument_keys || !backend->argument_layouts ||
            !backend->library_keys || !backend->libraries ||
            !backend->library_warnings) {
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
        [backend->argument_layouts release];
        [backend->argument_keys release];
        [backend->pipeline_warnings release];
        [backend->pipeline_reflections release];
        [backend->pipelines release];
        [backend->pipeline_keys release];
        [backend->queue release];
        [backend->device release];
        g_free(backend);
    }
}

static id metal_argument_lookup(InfernoMetalBackend *backend, id key)
{
    NSUInteger index = [backend->argument_keys indexOfObject:key];

    if (index == NSNotFound) {
        return nil;
    }
    id layout =
        [[[backend->argument_layouts objectAtIndex:index] retain] autorelease];
    [backend->argument_keys removeObjectAtIndex:index];
    [backend->argument_layouts removeObjectAtIndex:index];
    [backend->argument_keys addObject:key];
    [backend->argument_layouts addObject:layout];
    return layout;
}

static void metal_argument_insert(InfernoMetalBackend *backend, id key,
                                  id layout)
{
    if ([backend->argument_keys count] == METAL_ARGUMENT_CACHE_SIZE) {
        [backend->argument_keys removeObjectAtIndex:0];
        [backend->argument_layouts removeObjectAtIndex:0];
    }
    [backend->argument_keys addObject:key];
    [backend->argument_layouts addObject:layout];
}

/* Retain across promotion, even when removal drops the cache's last reference.
 */
static id metal_pipeline_lookup_full(InfernoMetalBackend *backend, id key,
                                     NSError **warning, id *reflection)
{
    NSUInteger index = [backend->pipeline_keys indexOfObject:key];

    if (index == NSNotFound) {
        *warning = nil;
        if (reflection) {
            *reflection = nil;
        }
        return nil;
    }
    id pipeline =
        [[[backend->pipelines objectAtIndex:index] retain] autorelease];
    id diagnostic =
        [[[backend->pipeline_warnings objectAtIndex:index] retain] autorelease];
    [backend->pipeline_keys removeObjectAtIndex:index];
    [backend->pipelines removeObjectAtIndex:index];
    [backend->pipeline_warnings removeObjectAtIndex:index];
    id reflected = [[[backend->pipeline_reflections objectAtIndex:index] retain]
        autorelease];
    [backend->pipeline_reflections removeObjectAtIndex:index];
    [backend->pipeline_keys addObject:key];
    [backend->pipelines addObject:pipeline];
    [backend->pipeline_warnings addObject:diagnostic];
    [backend->pipeline_reflections addObject:reflected];
    *warning = diagnostic == [NSNull null] ? nil : diagnostic;
    if (reflection) {
        *reflection = reflected == [NSNull null] ? nil : reflected;
    }
    return pipeline;
}

static id metal_pipeline_lookup(InfernoMetalBackend *backend, id key,
                                NSError **warning)
{
    return metal_pipeline_lookup_full(backend, key, warning, NULL);
}

static void metal_pipeline_insert_full(InfernoMetalBackend *backend, id key,
                                       id pipeline, NSError *warning,
                                       id reflection)
{
    if ([backend->pipeline_keys count] == METAL_PIPELINE_CACHE_SIZE) {
        [backend->pipeline_keys removeObjectAtIndex:0];
        [backend->pipelines removeObjectAtIndex:0];
        [backend->pipeline_warnings removeObjectAtIndex:0];
        [backend->pipeline_reflections removeObjectAtIndex:0];
    }
    [backend->pipeline_keys addObject:key];
    [backend->pipelines addObject:pipeline];
    [backend->pipeline_warnings addObject:warning ?: [NSNull null]];
    [backend->pipeline_reflections addObject:reflection ?: [NSNull null]];
}

static void metal_pipeline_insert(InfernoMetalBackend *backend, id key,
                                  id pipeline, NSError *warning)
{
    metal_pipeline_insert_full(backend, key, pipeline, warning, nil);
}

static id<MTLLibrary> metal_library_lookup(InfernoMetalBackend *backend, id key,
                                           NSError **warning)
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

static void metal_library_insert(InfernoMetalBackend *backend, id key,
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

static bool metal_commit_wait(id<MTLCommandBuffer> command_buffer,
                              InfernoMetalProgressFn progress, void *opaque,
                              bool *scheduled, char *message,
                              size_t message_size)
{
    InfernoMetalScheduledObserver *observer =
        [[InfernoMetalScheduledObserver alloc] initWithCallback:progress
                                                         opaque:opaque];
    if (!observer) {
        return metal_error(message, message_size, "Metal observer", nil);
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
    *scheduled = observer->scheduled;
    observer->armed = false;
    observer->callback = NULL;
    observer->opaque = NULL;
    qemu_mutex_unlock(&observer->lock);
    [observer release];
    return true;
}

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

        bool scheduled = false;
        if (!metal_commit_wait(command_buffer, progress, opaque, &scheduled,
                               message, message_size)) {
            return false;
        }

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

/* Version-5 resource executors are callable before the outer protocol exposes
 * them. Keep every SDK value represented on the wire pinned here. */
_Static_assert(MTLPixelFormatRGBA8Unorm ==
                   INFERNO_METAL_RESOURCE_PIXEL_FORMAT_RGBA8_UNORM,
               "RGBA8Unorm wire value");
_Static_assert(MTLPixelFormatRGBA8Unorm_sRGB ==
                   INFERNO_METAL_RESOURCE_PIXEL_FORMAT_RGBA8_UNORM_SRGB,
               "RGBA8Unorm_sRGB wire value");
_Static_assert(MTLPixelFormatRGBA8Snorm ==
                   INFERNO_METAL_RESOURCE_PIXEL_FORMAT_RGBA8_SNORM,
               "RGBA8Snorm wire value");
_Static_assert(MTLPixelFormatBGRA8Unorm ==
                   INFERNO_METAL_RESOURCE_PIXEL_FORMAT_BGRA8_UNORM,
               "BGRA8Unorm wire value");
_Static_assert(MTLPixelFormatBGRA8Unorm_sRGB ==
                   INFERNO_METAL_RESOURCE_PIXEL_FORMAT_BGRA8_UNORM_SRGB,
               "BGRA8Unorm_sRGB wire value");
_Static_assert(
    MTLColorWriteMaskRed == INFERNO_METAL_RESOURCE_WRITE_MASK_RED &&
        MTLColorWriteMaskGreen == INFERNO_METAL_RESOURCE_WRITE_MASK_GREEN &&
        MTLColorWriteMaskBlue == INFERNO_METAL_RESOURCE_WRITE_MASK_BLUE &&
        MTLColorWriteMaskAlpha == INFERNO_METAL_RESOURCE_WRITE_MASK_ALPHA,
    "Metal color write masks");
_Static_assert(MTLLoadActionDontCare == INFERNO_METAL_RESOURCE_LOAD_DONT_CARE &&
                   MTLLoadActionLoad == INFERNO_METAL_RESOURCE_LOAD_LOAD &&
                   MTLLoadActionClear == INFERNO_METAL_RESOURCE_LOAD_CLEAR &&
                   MTLStoreActionStore == INFERNO_METAL_RESOURCE_STORE_STORE,
               "Metal attachment action values");
_Static_assert(MTLPrimitiveTypePoint ==
                       INFERNO_METAL_RESOURCE_PRIMITIVE_POINT &&
                   MTLPrimitiveTypeTriangleStrip ==
                       INFERNO_METAL_RESOURCE_PRIMITIVE_TRIANGLE_STRIP,
               "Metal primitive values");
_Static_assert(
    MTLCullModeNone == INFERNO_METAL_RESOURCE_CULL_NONE &&
        MTLCullModeBack == INFERNO_METAL_RESOURCE_CULL_BACK &&
        MTLWindingClockwise == INFERNO_METAL_RESOURCE_WINDING_CLOCKWISE &&
        MTLWindingCounterClockwise ==
            INFERNO_METAL_RESOURCE_WINDING_COUNTER_CLOCKWISE &&
        MTLTriangleFillModeFill == INFERNO_METAL_RESOURCE_FILL_MODE_FILL &&
        MTLTriangleFillModeLines == INFERNO_METAL_RESOURCE_FILL_MODE_LINES,
    "Metal raster values");
_Static_assert(
    MTLBlendFactorZero == INFERNO_METAL_RESOURCE_BLEND_FACTOR_ZERO &&
        MTLBlendFactorOneMinusSource1Alpha ==
            INFERNO_METAL_RESOURCE_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA &&
        MTLBlendOperationAdd == INFERNO_METAL_RESOURCE_BLEND_OPERATION_ADD &&
        MTLBlendOperationMax == INFERNO_METAL_RESOURCE_BLEND_OPERATION_MAX,
    "Metal blend values");
_Static_assert(
    MTLSamplerMinMagFilterNearest == INFERNO_METAL_RESOURCE_FILTER_NEAREST &&
        MTLSamplerMinMagFilterLinear == INFERNO_METAL_RESOURCE_FILTER_LINEAR &&
        MTLSamplerMipFilterNotMipmapped ==
            INFERNO_METAL_RESOURCE_MIP_FILTER_NOT_MIPMAPPED &&
        MTLSamplerMipFilterLinear == INFERNO_METAL_RESOURCE_MIP_FILTER_LINEAR &&
        MTLSamplerAddressModeClampToEdge ==
            INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE &&
        MTLSamplerAddressModeClampToBorderColor ==
            INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR &&
        MTLCompareFunctionNever == INFERNO_METAL_RESOURCE_COMPARE_NEVER &&
        MTLCompareFunctionAlways == INFERNO_METAL_RESOURCE_COMPARE_ALWAYS,
    "Metal sampler values");
_Static_assert(MTLResourceUsageRead == INFERNO_METAL_RESOURCE_USAGE_READ &&
                   MTLResourceUsageWrite == INFERNO_METAL_RESOURCE_USAGE_WRITE,
               "Metal resource usage values");

typedef struct MetalV5Failure {
    uint32_t outcome;
    uint32_t phase;
    uint32_t kind;
    uint32_t index;
    uint32_t stage;
    uint32_t function_type;
    NSError *error;
    NSString *explanation;
} MetalV5Failure;

static NSData *metal_v5_bytes(const uint8_t *bytes, size_t size)
{
    return [NSData dataWithBytes:bytes length:size];
}

static NSArray *metal_v5_library_key(uint32_t kind, NSData *bytes)
{
    return @[ @"v5-library", @(kind), bytes ];
}

static NSString *metal_v5_name(const uint8_t *record, size_t offset)
{
    return [NSString stringWithUTF8String:(const char *)record + offset];
}

static id<MTLLibrary>
metal_v5_library(InfernoMetalBackend *backend, const InfernoMetalCommand *c,
                 const uint8_t *record, const uint8_t *payload,
                 NSError **warning, MetalV5Failure *failure, uint32_t index)
{
    uint32_t kind =
        ldl_le_p(record + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET);
    uint32_t offset =
        ldl_le_p(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET);
    uint32_t size =
        ldl_le_p(record + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET);
    NSData *bytes = metal_v5_bytes(payload + offset, size);
    NSArray *key = metal_v5_library_key(kind, bytes);
    id<MTLLibrary> library = metal_library_lookup(backend, key, warning);

    trace_inferno_metal_typed_library_cache(c->sequence, c->opcode, kind,
                                            library != nil,
                                            [backend->library_keys count]);
    if (library) {
        return library;
    }

    NSError *error = nil;
    if (kind == INFERNO_METAL_RESOURCE_LIBRARY_SOURCE) {
        NSString *source =
            [[[NSString alloc] initWithData:bytes
                                   encoding:NSUTF8StringEncoding] autorelease];
        library = [[backend->device newLibraryWithSource:source
                                                 options:nil
                                                   error:&error] autorelease];
    } else {
        void *copy = g_memdup2([bytes bytes], [bytes length]);
        dispatch_data_t data = dispatch_data_create(
            copy, [bytes length], dispatch_get_global_queue(0, 0),
            DISPATCH_DATA_DESTRUCTOR_FREE);
        if (data) {
            library = [[backend->device newLibraryWithData:data
                                                     error:&error] autorelease];
            dispatch_release(data);
        } else {
            g_free(copy);
        }
    }
    if (!library) {
        failure->outcome = INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED;
        failure->phase = INFERNO_METAL_BATCH_PHASE_LIBRARY;
        failure->kind = INFERNO_METAL_RESOURCE_RECORD_LIBRARY;
        failure->index = index;
        failure->error = error;
        failure->explanation = @"Metal returned no library and no NSError";
        return nil;
    }
    metal_library_insert(backend, key, library, error);
    *warning = error;
    return library;
}

static bool metal_v5_function(id<MTLLibrary> library, NSString *name,
                              MTLFunctionType wanted, id<MTLFunction> *result,
                              MetalV5Failure *failure, uint32_t kind,
                              uint32_t index, uint32_t stage)
{
    id<MTLFunction> function = [[library newFunctionWithName:name] autorelease];
    if (!function) {
        failure->outcome = INFERNO_METAL_BATCH_OUTCOME_FUNCTION_NOT_FOUND;
        failure->phase = INFERNO_METAL_BATCH_PHASE_FUNCTION;
        failure->kind = kind;
        failure->index = index;
        failure->stage = stage;
        failure->explanation =
            @"library has no function with the requested name";
        return false;
    }
    if ([function functionType] != wanted) {
        metal_function_type(&failure->function_type, [function functionType]);
        failure->outcome = INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH;
        failure->phase = INFERNO_METAL_BATCH_PHASE_FUNCTION;
        failure->kind = kind;
        failure->index = index;
        failure->stage = stage;
        failure->explanation = @"function has the wrong stage";
        return false;
    }
    if (metal_function_has_constants(function)) {
        metal_function_type(&failure->function_type, [function functionType]);
        failure->outcome = INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED;
        failure->phase = INFERNO_METAL_BATCH_PHASE_FUNCTION;
        failure->kind = kind;
        failure->index = index;
        failure->stage = stage;
        failure->explanation = @"function requires specialization";
        return false;
    }
    *result = function;
    return true;
}

static NSArray *metal_v5_compute_key(const uint8_t *pipeline,
                                     const uint8_t *library,
                                     const uint8_t *payload)
{
    uint32_t kind =
        ldl_le_p(library + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET);
    uint32_t offset =
        ldl_le_p(library + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET);
    uint32_t size =
        ldl_le_p(library + INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET);
    return @[
        @"v5-compute", @(kind), metal_v5_bytes(payload + offset, size),
        metal_v5_name(pipeline,
                      INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_NAME_OFFSET)
    ];
}

static NSArray *metal_v5_render_key(const uint8_t *pipeline,
                                    const uint8_t *vertex_library,
                                    const uint8_t *fragment_library,
                                    const uint8_t *payload)
{
    uint32_t vk =
        ldl_le_p(vertex_library + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET);
    uint32_t vo = ldl_le_p(vertex_library +
                           INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET);
    uint32_t vs = ldl_le_p(vertex_library +
                           INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET);
    uint32_t fk =
        ldl_le_p(fragment_library + INFERNO_METAL_RESOURCE_LIBRARY_KIND_OFFSET);
    uint32_t fo = ldl_le_p(fragment_library +
                           INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_OFFSET);
    uint32_t fs = ldl_le_p(fragment_library +
                           INFERNO_METAL_RESOURCE_LIBRARY_PAYLOAD_SIZE_OFFSET);
    NSData *state = metal_v5_bytes(
        pipeline + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_PIXEL_FORMAT_OFFSET,
        56);
    return @[
        @"v5-render", @(vk), metal_v5_bytes(payload + vo, vs),
        metal_v5_name(
            pipeline,
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_NAME_OFFSET),
        @(fk), metal_v5_bytes(payload + fo, fs),
        metal_v5_name(
            pipeline,
            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_NAME_OFFSET),
        state
    ];
}

static id<MTLComputePipelineState> metal_v5_compute_pipeline(
    InfernoMetalBackend *backend, const InfernoMetalCommand *c,
    const uint8_t *record, const uint8_t *library_record,
    id<MTLLibrary> library, const uint8_t *payload, NSError *library_warning,
    MetalV5Failure *failure, uint32_t index,
    MTLComputePipelineReflection **reflection_out, NSError **diagnostic_out)
{
    NSArray *key = metal_v5_compute_key(record, library_record, payload);
    NSError *warning = nil;
    id cached_reflection = nil;
    id<MTLComputePipelineState> pipeline =
        metal_pipeline_lookup_full(backend, key, &warning, &cached_reflection);
    trace_inferno_metal_pipeline_cache(c->sequence, c->opcode, pipeline != nil,
                                       [backend->pipeline_keys count]);
    if (pipeline) {
        *reflection_out = cached_reflection;
        *diagnostic_out = warning;
        return pipeline;
    }
    id<MTLFunction> function = nil;
    if (!metal_v5_function(
            library,
            metal_v5_name(record,
                          INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_NAME_OFFSET),
            MTLFunctionTypeKernel, &function, failure,
            INFERNO_METAL_BATCH_RECORD_PIPELINE, index,
            INFERNO_METAL_RESOURCE_COMPILER_STAGE_NONE)) {
        return nil;
    }
    NSError *error = nil;
    MTLComputePipelineReflection *reflection = nil;
    pipeline = [[backend->device
        newComputePipelineStateWithFunction:function
                                    options:MTLPipelineOptionBindingInfo |
                                            MTLPipelineOptionBufferTypeInfo
                                 reflection:&reflection
                                      error:&error] autorelease];
    if (!pipeline || !reflection) {
        failure->outcome = INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED;
        failure->phase = INFERNO_METAL_BATCH_PHASE_PIPELINE;
        failure->kind = INFERNO_METAL_BATCH_RECORD_PIPELINE;
        failure->index = index;
        failure->error = error;
        failure->explanation = pipeline ?
                                   @"Metal returned no reflection" :
                                   @"Metal returned no pipeline and no NSError";
        return nil;
    }
    metal_pipeline_insert_full(backend, key, pipeline, error ?: library_warning,
                               reflection);
    *reflection_out = reflection;
    *diagnostic_out = error ?: library_warning;
    return pipeline;
}

static id<MTLBinding> metal_v5_reflection_binding(NSArray<id<MTLBinding>> *list,
                                                  MTLBindingType type,
                                                  NSUInteger index);

static uint32_t metal_v5_argument_constant_size(MTLDataType type)
{
    uint32_t first;
    uint32_t component;

    if (type >= MTLDataTypeFloat && type <= MTLDataTypeFloat4) {
        first = MTLDataTypeFloat;
        component = 4;
    } else if (type >= MTLDataTypeHalf && type <= MTLDataTypeHalf4) {
        first = MTLDataTypeHalf;
        component = 2;
    } else if (type >= MTLDataTypeInt && type <= MTLDataTypeInt4) {
        first = MTLDataTypeInt;
        component = 4;
    } else if (type >= MTLDataTypeUInt && type <= MTLDataTypeUInt4) {
        first = MTLDataTypeUInt;
        component = 4;
    } else if (type >= MTLDataTypeShort && type <= MTLDataTypeShort4) {
        first = MTLDataTypeShort;
        component = 2;
    } else if (type >= MTLDataTypeUShort && type <= MTLDataTypeUShort4) {
        first = MTLDataTypeUShort;
        component = 2;
    } else if (type >= MTLDataTypeChar && type <= MTLDataTypeChar4) {
        first = MTLDataTypeChar;
        component = 1;
    } else if (type >= MTLDataTypeUChar && type <= MTLDataTypeUChar4) {
        first = MTLDataTypeUChar;
        component = 1;
    } else if (type >= MTLDataTypeBool && type <= MTLDataTypeBool4) {
        first = MTLDataTypeBool;
        component = 1;
    } else {
        return 0;
    }
    return component * ((uint32_t)type - first + 1);
}

static NSDictionary *metal_v5_argument_member(MTLStructMember *member,
                                              NSString **reason)
{
    MTLDataType data_type = [member dataType];
    uint32_t kind = 0;
    uint32_t access = 0;
    uint32_t constant_size = 0;
    uint32_t texture_type = 0;
    uint32_t texture_data_type = 0;
    uint32_t depth = 0;
    uint32_t array_length = 0;
    uint32_t index_stride = 0;
    uint32_t element_data_type = 0;
    uint32_t pointer_alignment = 0;

    if (data_type == MTLDataTypePointer) {
        MTLPointerType *pointer = [member pointerType];
        if (!pointer || [pointer elementIsArgumentBuffer] ||
            [pointer access] > MTLBindingAccessReadWrite ||
            ![pointer alignment] || [pointer alignment] > UINT32_MAX) {
            *reason = @"argument buffer has an unsupported pointer member";
            return nil;
        }
        kind = INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER;
        access = [pointer access];
        pointer_alignment = (uint32_t)[pointer alignment];
    } else if (data_type == MTLDataTypeTexture) {
        MTLTextureReferenceType *texture = [member textureReferenceType];
        if (!texture || [texture textureType] != MTLTextureType2D ||
            [texture isDepthTexture] ||
            ([texture textureDataType] != MTLDataTypeFloat &&
             [texture textureDataType] != MTLDataTypeHalf) ||
            [texture access] != MTLBindingAccessReadOnly) {
            *reason = @"argument buffer has an unsupported texture member";
            return nil;
        }
        kind = INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE;
        access = [texture access];
        texture_type = [texture textureType];
        texture_data_type = [texture textureDataType];
        depth = [texture isDepthTexture];
    } else if (data_type == MTLDataTypeSampler) {
        kind = INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER;
    } else if (data_type == MTLDataTypeArray) {
        MTLArrayType *array = [member arrayType];
        MTLTextureReferenceType *texture = [array elementTextureReferenceType];
        if (!array || [array elementType] != MTLDataTypeTexture ||
            [array arrayLength] < 2 || [array arrayLength] > UINT32_MAX ||
            ![array argumentIndexStride] ||
            [array argumentIndexStride] > UINT32_MAX || !texture ||
            [texture textureType] != MTLTextureType2D ||
            [texture isDepthTexture] ||
            ([texture textureDataType] != MTLDataTypeFloat &&
             [texture textureDataType] != MTLDataTypeHalf) ||
            [texture access] != MTLBindingAccessReadOnly) {
            *reason = @"argument buffer has an unsupported array member";
            return nil;
        }
        kind = INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE;
        access = [texture access];
        texture_type = [texture textureType];
        texture_data_type = [texture textureDataType];
        depth = [texture isDepthTexture];
        array_length = (uint32_t)[array arrayLength];
        index_stride = (uint32_t)[array argumentIndexStride];
        element_data_type = [array elementType];
    } else {
        constant_size = metal_v5_argument_constant_size(data_type);
        if (!constant_size) {
            *reason = @"argument buffer has an unsupported constant member";
            return nil;
        }
        kind = INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT;
    }
    if ([member argumentIndex] > UINT32_MAX) {
        *reason = @"argument member metadata exceeds the wire range";
        return nil;
    }
    return @{
        @"id" : @((uint32_t)[member argumentIndex]),
        @"kind" : @(kind),
        @"data_type" : @((uint32_t)data_type),
        @"access" : @(access),
        @"byte_offset" : @((uint64_t)[member offset]),
        @"constant_size" : @(constant_size),
        @"texture_type" : @(texture_type),
        @"texture_data_type" : @(texture_data_type),
        @"depth" : @(depth),
        @"array_length" : @(array_length),
        @"index_stride" : @(index_stride),
        @"element_data_type" : @(element_data_type),
        @"pointer_alignment" : @(pointer_alignment),
    };
}

static NSArray *metal_v5_argument_members(NSArray<MTLStructMember *> *members,
                                          NSUInteger encoded_length,
                                          NSString **reason)
{
    if (!members || ![members count] ||
        [members count] > INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS) {
        *reason = @"argument buffer has no supported member graph";
        return nil;
    }
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:[members count]];
    NSMutableSet *ids = [NSMutableSet set];
    uint32_t prior_id = 0;
    uint32_t expanded_count = 0;
    for (NSUInteger i = 0; i < [members count]; i++) {
        NSDictionary *descriptor =
            metal_v5_argument_member([members objectAtIndex:i], reason);
        if (!descriptor) {
            return nil;
        }
        uint32_t base = [descriptor[@"id"] unsignedIntValue];
        uint32_t length = [descriptor[@"array_length"] unsignedIntValue];
        uint32_t stride = [descriptor[@"index_stride"] unsignedIntValue];
        uint32_t count = length ? length : 1;
        if ((i && base <= prior_id) ||
            count >
                INFERNO_METAL_ARGUMENT_MAX_LAYOUT_MEMBERS - expanded_count) {
            *reason = @"argument member ids are not canonical";
            return nil;
        }
        for (uint32_t j = 0; j < count; j++) {
            if (j && stride > (UINT32_MAX - base) / j) {
                *reason = @"argument array member id overflows";
                return nil;
            }
            NSNumber *member_id = @(base + j * stride);
            if ([ids containsObject:member_id]) {
                *reason = @"argument member ids overlap";
                return nil;
            }
            [ids addObject:member_id];
        }
        uint32_t constant_size =
            [descriptor[@"constant_size"] unsignedIntValue];
        uint64_t byte_offset =
            [descriptor[@"byte_offset"] unsignedLongLongValue];
        if (byte_offset >= encoded_length) {
            *reason = @"argument member is outside the encoded backing";
            return nil;
        }
        if (constant_size && (byte_offset > encoded_length ||
                              constant_size > encoded_length - byte_offset)) {
            *reason = @"argument constant is outside the encoded backing";
            return nil;
        }
        if (constant_size) {
            for (NSDictionary *prior in result) {
                uint32_t prior_size =
                    [prior[@"constant_size"] unsignedIntValue];
                uint64_t prior_offset =
                    [prior[@"byte_offset"] unsignedLongLongValue];
                if (prior_size && byte_offset < prior_offset + prior_size &&
                    prior_offset < byte_offset + constant_size) {
                    *reason = @"argument constants overlap";
                    return nil;
                }
            }
        }
        [result addObject:descriptor];
        expanded_count += count;
        prior_id = base;
    }
    return result;
}

static id<MTLBufferBinding>
metal_v5_argument_root_binding(NSArray<id<MTLBinding>> *bindings,
                               NSUInteger slot, NSString **reason)
{
    id<MTLBinding> binding =
        metal_v5_reflection_binding(bindings, MTLBindingTypeBuffer, slot);
    if (!binding || ![binding isUsed] ||
        ![binding conformsToProtocol:@protocol(MTLBufferBinding)]) {
        *reason = @"argument buffer slot is absent from final reflection";
        return nil;
    }
    id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)binding;
    MTLPointerType *pointer = [buffer bufferPointerType];
    if (!pointer) {
        *reason = @"argument buffer root pointer metadata is unavailable";
        return nil;
    }
    if (![pointer elementIsArgumentBuffer]) {
        *reason = @"buffer slot is an ordinary buffer, not an argument buffer";
        return nil;
    }
    if ([buffer bufferDataType] != MTLDataTypeStruct ||
        ![buffer bufferStructType]) {
        *reason = @"argument buffer root struct metadata is unavailable";
        return nil;
    }
    return buffer;
}

static NSDictionary *metal_v5_argument_layout(
    InfernoMetalBackend *backend, const uint8_t *pipeline_record,
    const uint8_t *library_record, id<MTLLibrary> library,
    const uint8_t *payload, MTLComputePipelineReflection *reflection,
    uint32_t slot, MetalV5Failure *failure, uint32_t failure_kind,
    uint32_t failure_index, NSString **reason, bool *resource_failed)
{
    NSArray *key = [metal_v5_compute_key(pipeline_record, library_record,
                                         payload) arrayByAddingObject:@(slot)];
    NSDictionary *cached = metal_argument_lookup(backend, key);
    if (cached) {
        return cached;
    }
    id<MTLBufferBinding> root =
        metal_v5_argument_root_binding([reflection bindings], slot, reason);
    if (!root) {
        return nil;
    }
    id<MTLFunction> function = nil;
    if (!metal_v5_function(
            library,
            metal_v5_name(pipeline_record,
                          INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_NAME_OFFSET),
            MTLFunctionTypeKernel, &function, failure, failure_kind,
            failure_index, INFERNO_METAL_RESOURCE_COMPILER_STAGE_NONE)) {
        return nil;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    MTLArgument *argument = nil;
    id<MTLArgumentEncoder> encoder =
        [function newArgumentEncoderWithBufferIndex:slot reflection:&argument];
#pragma clang diagnostic pop
    if (!encoder) {
        *reason = @"Metal returned no function argument encoder";
        *resource_failed = true;
        return nil;
    }
    NSUInteger encoded_length = [encoder encodedLength];
    NSUInteger alignment = [encoder alignment];
    NSArray *graph_a = argument ? [[argument bufferStructType] members] : nil;
    NSArray *graph_b = [[root bufferStructType] members];
    NSArray *members_a = nil;
    NSArray *members_b = nil;
    if (!encoded_length ||
        encoded_length > INFERNO_METAL_ARGUMENT_MAX_ENCODED_LENGTH ||
        !alignment || alignment > 4096 || (alignment & (alignment - 1))) {
        *reason = @"argument encoder layout exceeds the wire range";
    } else if (!argument || ![argument bufferStructType]) {
        *reason = @"function argument reflection is unavailable";
    } else {
        members_a = metal_v5_argument_members(graph_a, encoded_length, reason);
        if (members_a) {
            members_b =
                metal_v5_argument_members(graph_b, encoded_length, reason);
        }
        if (members_a && members_b && ![members_a isEqualToArray:members_b]) {
            *reason = @"function and pipeline argument layouts disagree";
            members_b = nil;
        }
    }
    if (!members_a || !members_b) {
        [encoder release];
        return nil;
    }
    NSDictionary *layout = @{
        @"encoder" : encoder,
        @"members" : members_a,
        @"encoded_length" : @(encoded_length),
        @"alignment" : @(alignment),
    };
    [encoder release];
    metal_argument_insert(backend, key, layout);
    return layout;
}

static void metal_v5_render_descriptor(MTLRenderPipelineDescriptor *d,
                                       const uint8_t *record,
                                       id<MTLFunction> vertex,
                                       id<MTLFunction> fragment)
{
    d.vertexFunction = vertex;
    d.fragmentFunction = fragment;
    d.rasterSampleCount = ldl_le_p(
        record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SAMPLE_COUNT_OFFSET);
    MTLRenderPipelineColorAttachmentDescriptor *color = d.colorAttachments[0];
    color.pixelFormat = ldl_le_p(
        record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_PIXEL_FORMAT_OFFSET);
    color.blendingEnabled = ldl_le_p(
        record +
        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_BLENDING_ENABLED_OFFSET);
    color.sourceRGBBlendFactor = ldl_le_p(
        record +
        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SOURCE_RGB_FACTOR_OFFSET);
    color.destinationRGBBlendFactor = ldl_le_p(
        record +
        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_DESTINATION_RGB_FACTOR_OFFSET);
    color.rgbBlendOperation = ldl_le_p(
        record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_RGB_OPERATION_OFFSET);
    color.sourceAlphaBlendFactor = ldl_le_p(
        record +
        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_SOURCE_ALPHA_FACTOR_OFFSET);
    color.destinationAlphaBlendFactor = ldl_le_p(
        record +
        INFERNO_METAL_RESOURCE_RENDER_PIPELINE_DESTINATION_ALPHA_FACTOR_OFFSET);
    color.alphaBlendOperation = ldl_le_p(
        record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_ALPHA_OPERATION_OFFSET);
    color.writeMask = ldl_le_p(
        record + INFERNO_METAL_RESOURCE_RENDER_PIPELINE_WRITE_MASK_OFFSET);
    d.alphaToCoverageEnabled = NO;
    d.alphaToOneEnabled = NO;
    d.rasterizationEnabled = YES;
    d.maxVertexAmplificationCount = 1;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
    d.vertexDescriptor = nil;
    d.depthAttachmentPixelFormat = MTLPixelFormatInvalid;
    d.stencilAttachmentPixelFormat = MTLPixelFormatInvalid;
    d.supportIndirectCommandBuffers = NO;
    d.binaryArchives = @[];
    d.vertexPreloadedLibraries = @[];
    d.fragmentPreloadedLibraries = @[];
    d.vertexLinkedFunctions = nil;
    d.fragmentLinkedFunctions = nil;
    d.supportAddingVertexBinaryFunctions = NO;
    d.supportAddingFragmentBinaryFunctions = NO;
    d.maxVertexCallStackDepth = 1;
    d.maxFragmentCallStackDepth = 1;
    d.shaderValidation = MTLShaderValidationDefault;
    for (NSUInteger i = 0; i < 31; i++) {
        [d.vertexBuffers[i] setMutability:MTLMutabilityDefault];
        [d.fragmentBuffers[i] setMutability:MTLMutabilityDefault];
    }
    for (NSUInteger i = 1; i < 8; i++) {
        d.colorAttachments[i].pixelFormat = MTLPixelFormatInvalid;
    }
}

static id<MTLRenderPipelineState> metal_v5_render_pipeline(
    InfernoMetalBackend *backend, const InfernoMetalCommand *c,
    const uint8_t *record, const uint8_t *vertex_library_record,
    const uint8_t *fragment_library_record, id<MTLLibrary> vertex_library,
    id<MTLLibrary> fragment_library, const uint8_t *payload,
    NSError *library_warning, MetalV5Failure *failure, uint32_t index,
    MTLRenderPipelineReflection **reflection_out, NSError **diagnostic_out)
{
    NSArray *key = metal_v5_render_key(record, vertex_library_record,
                                       fragment_library_record, payload);
    NSError *warning = nil;
    id reflection = nil;
    id<MTLRenderPipelineState> pipeline =
        metal_pipeline_lookup_full(backend, key, &warning, &reflection);
    trace_inferno_metal_pipeline_cache(c->sequence, c->opcode, pipeline != nil,
                                       [backend->pipeline_keys count]);
    if (pipeline) {
        *reflection_out = reflection;
        *diagnostic_out = warning;
        return pipeline;
    }
    id<MTLFunction> vertex = nil;
    id<MTLFunction> fragment = nil;
    if (!metal_v5_function(
            vertex_library,
            metal_v5_name(
                record,
                INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_NAME_OFFSET),
            MTLFunctionTypeVertex, &vertex, failure,
            INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE, index,
            INFERNO_METAL_RESOURCE_COMPILER_STAGE_VERTEX) ||
        !metal_v5_function(
            fragment_library,
            metal_v5_name(
                record,
                INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_NAME_OFFSET),
            MTLFunctionTypeFragment, &fragment, failure,
            INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE, index,
            INFERNO_METAL_RESOURCE_COMPILER_STAGE_FRAGMENT)) {
        return nil;
    }
    MTLRenderPipelineDescriptor *descriptor =
        [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
    metal_v5_render_descriptor(descriptor, record, vertex, fragment);
    NSError *error = nil;
    MTLRenderPipelineReflection *created_reflection = nil;
    pipeline = [[backend->device
        newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionBindingInfo |
                                             MTLPipelineOptionBufferTypeInfo
                                  reflection:&created_reflection
                                       error:&error] autorelease];
    if (!pipeline || !created_reflection) {
        failure->outcome = INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED;
        failure->phase = INFERNO_METAL_BATCH_PHASE_PIPELINE;
        failure->kind = INFERNO_METAL_RESOURCE_RECORD_RENDER_PIPELINE;
        failure->index = index;
        failure->error = error;
        failure->explanation = pipeline ?
                                   @"Metal returned no reflection" :
                                   @"Metal returned no pipeline and no NSError";
        return nil;
    }
    metal_pipeline_insert_full(backend, key, pipeline, error ?: library_warning,
                               created_reflection);
    *reflection_out = created_reflection;
    *diagnostic_out = error ?: library_warning;
    return pipeline;
}

static void metal_v5_query_initialize(const InfernoMetalCommand *c,
                                      uint8_t *output)
{
    memset(output, 0, c->output_size);
    stl_le_p(output + INFERNO_METAL_COMPILER_VERSION_OFFSET,
             INFERNO_METAL_RESOURCE_VERSION);
    stl_le_p(output + INFERNO_METAL_COMPILER_OPCODE_OFFSET, c->opcode);
    stq_le_p(output + INFERNO_METAL_COMPILER_SEQUENCE_OFFSET, c->sequence);
}

static bool metal_v5_query_finish(const InfernoMetalCommand *c, uint8_t *output,
                                  MetalV5Failure *failure)
{
    uint32_t outcome;
    uint32_t phase;

    switch (failure->outcome) {
    case INFERNO_METAL_BATCH_OUTCOME_COMPILE_FAILED:
        outcome = INFERNO_METAL_COMPILER_OUTCOME_COMPILE_FAILED;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_NOT_FOUND:
        outcome = INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_NOT_FOUND;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH:
        outcome = INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_TYPE_MISMATCH;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED:
        outcome = INFERNO_METAL_COMPILER_OUTCOME_SPECIALIZATION_REQUIRED;
        break;
    default:
        g_assert_not_reached();
    }
    switch (failure->phase) {
    case INFERNO_METAL_BATCH_PHASE_LIBRARY:
        phase = INFERNO_METAL_COMPILER_PHASE_LIBRARY;
        break;
    case INFERNO_METAL_BATCH_PHASE_FUNCTION:
        phase = INFERNO_METAL_COMPILER_PHASE_FUNCTION;
        break;
    case INFERNO_METAL_BATCH_PHASE_PIPELINE:
        phase = INFERNO_METAL_COMPILER_PHASE_PIPELINE;
        break;
    default:
        g_assert_not_reached();
    }
    stl_le_p(output + INFERNO_METAL_RESOURCE_COMPILER_STAGE_OFFSET,
             failure->stage);
    stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_TYPE_OFFSET,
             failure->function_type);
    return metal_query_finish(c, output, outcome, phase, failure->error,
                              failure->explanation, false);
}

static bool metal_v5_argument_query_finish(const InfernoMetalCommand *c,
                                           uint8_t *output,
                                           NSDictionary *layout,
                                           NSError *diagnostic,
                                           NSString *reason)
{
    if (!layout) {
        return metal_query_finish(
            c, output, INFERNO_METAL_COMPILER_OUTCOME_ARGUMENT_UNSUPPORTED,
            INFERNO_METAL_COMPILER_PHASE_ARGUMENT, nil,
            reason ?: @"argument layout is unsupported", false);
    }
    uint8_t *header = output + INFERNO_METAL_ARGUMENT_LAYOUT_HEADER_OFFSET;
    NSArray *members = layout[@"members"];
    stq_le_p(header + INFERNO_METAL_ARGUMENT_LAYOUT_ENCODED_LENGTH_OFFSET,
             [layout[@"encoded_length"] unsignedLongLongValue]);
    stq_le_p(header + INFERNO_METAL_ARGUMENT_LAYOUT_ALIGNMENT_OFFSET,
             [layout[@"alignment"] unsignedLongLongValue]);
    stl_le_p(header + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_COUNT_OFFSET,
             (uint32_t)[members count]);
    for (NSUInteger i = 0; i < [members count]; i++) {
        NSDictionary *member = [members objectAtIndex:i];
        uint8_t *record = output +
                          INFERNO_METAL_ARGUMENT_LAYOUT_MEMBERS_OFFSET +
                          i * INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_SIZE;
        stl_le_p(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ID_OFFSET,
                 [member[@"id"] unsignedIntValue]);
        stl_le_p(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_KIND_OFFSET,
                 [member[@"kind"] unsignedIntValue]);
        stl_le_p(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_DATA_TYPE_OFFSET,
                 [member[@"data_type"] unsignedIntValue]);
        stl_le_p(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ACCESS_OFFSET,
                 [member[@"access"] unsignedIntValue]);
        stq_le_p(record +
                     INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_BYTE_OFFSET_OFFSET,
                 [member[@"byte_offset"] unsignedLongLongValue]);
        stl_le_p(record +
                     INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_CONSTANT_SIZE_OFFSET,
                 [member[@"constant_size"] unsignedIntValue]);
        stl_le_p(record +
                     INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_TEXTURE_TYPE_OFFSET,
                 [member[@"texture_type"] unsignedIntValue]);
        stl_le_p(
            record +
                INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_TEXTURE_DATA_TYPE_OFFSET,
            [member[@"texture_data_type"] unsignedIntValue]);
        stl_le_p(record + INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_DEPTH_OFFSET,
                 [member[@"depth"] unsignedIntValue]);
        stl_le_p(record +
                     INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ARRAY_LENGTH_OFFSET,
                 [member[@"array_length"] unsignedIntValue]);
        stl_le_p(
            record +
                INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ARGUMENT_INDEX_STRIDE_OFFSET,
            [member[@"index_stride"] unsignedIntValue]);
        stl_le_p(
            record +
                INFERNO_METAL_ARGUMENT_LAYOUT_MEMBER_ELEMENT_DATA_TYPE_OFFSET,
            [member[@"element_data_type"] unsignedIntValue]);
    }
    return metal_query_finish(c, output, INFERNO_METAL_COMPILER_OUTCOME_OK,
                              INFERNO_METAL_COMPILER_PHASE_ARGUMENT, diagnostic,
                              nil, diagnostic != nil);
}

static bool metal_v5_library_inventory(const InfernoMetalCommand *c,
                                       id<MTLLibrary> library, NSError *warning,
                                       uint8_t *output, char *message,
                                       size_t message_size)
{
    NSArray<NSString *> *names = [library functionNames];
    NSMutableArray *functions = [NSMutableArray array];
    NSMutableArray *constants = [NSMutableArray array];
    NSMutableArray *vertices = [NSMutableArray array];
    NSMutableArray *stages = [NSMutableArray array];
    NSMutableSet *seen = [NSMutableSet set];
    NSString *reason = nil;
    uint64_t metadata_count = 0;

    if ([names count] > UINT32_MAX) {
        return metal_error(message, message_size,
                           "function inventory count exceeds uint32", nil);
    }
    stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_COUNT_OFFSET,
             [names count]);
    for (NSString *name in names) {
        id<MTLFunction> function =
            [[library newFunctionWithName:name] autorelease];
        uint32_t type;
        NSDictionary *dictionary = [function functionConstantsDictionary];
        NSArray *constant_list =
            dictionary ? metal_sorted_constants(dictionary, &reason) : @[];
        NSArray *vertex_list = [function vertexAttributes];
        NSArray *stage_list = [function stageInputAttributes];
        if (!metal_bounded_string(
                name, INFERNO_METAL_COMPILER_FUNCTION_NAME_SIZE, false) ||
            [seen containsObject:name] || !function ||
            !metal_function_type(&type, [function functionType]) ||
            [function patchType] > MTLPatchTypeQuad ||
            ([function options] & ~0xfUL) ||
            !metal_validate_metadata(constant_list, true, &reason) ||
            (vertex_list &&
             !metal_validate_metadata(vertex_list, false, &reason)) ||
            (stage_list &&
             !metal_validate_metadata(stage_list, false, &reason))) {
            reason = reason ?: @"function inventory has unsupported metadata";
            break;
        }
        [seen addObject:name];
        [functions addObject:function];
        [constants addObject:constant_list];
        [vertices addObject:vertex_list ?: [NSNull null]];
        [stages addObject:stage_list ?: [NSNull null]];
        metadata_count +=
            [constant_list count] + [vertex_list count] + [stage_list count];
        if (metadata_count > UINT32_MAX) {
            reason = @"metadata inventory count exceeds uint32";
            break;
        }
    }
    stl_le_p(output + INFERNO_METAL_COMPILER_METADATA_RECORD_COUNT_OFFSET,
             metadata_count);
    if (reason || [names count] > INFERNO_METAL_COMPILER_MAX_FUNCTIONS ||
        metadata_count > INFERNO_METAL_COMPILER_MAX_METADATA_RECORDS) {
        return metal_query_finish(
            c, output, INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
            INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
            reason ?: @"function inventory exceeds the v5 maximum", false);
    }
    uint64_t required =
        INFERNO_METAL_COMPILER_MIN_OUTPUT +
        [names count] * INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
        metadata_count * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
    if (required > c->output_size) {
        stl_le_p(output + INFERNO_METAL_COMPILER_REQUIRED_OUTPUT_SIZE_OFFSET,
                 required);
        return metal_query_finish(
            c, output, INFERNO_METAL_COMPILER_OUTCOME_OUTPUT_TOO_SMALL,
            INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
            @"output has insufficient inventory capacity", false);
    }
    MTLLibraryType library_type = [library type];
    NSString *install_name = [library installName];
    NSData *install_data =
        install_name ?
            metal_bounded_string(
                install_name, INFERNO_METAL_COMPILER_LIBRARY_NAME_SIZE, true) :
            nil;
    if ((library_type != MTLLibraryTypeExecutable &&
         library_type != MTLLibraryTypeDynamic) ||
        (install_name && !install_data)) {
        return metal_query_finish(
            c, output, INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
            INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
            @"library metadata is outside the v5 wire range", false);
    }
    stl_le_p(output + INFERNO_METAL_COMPILER_LIBRARY_TYPE_OFFSET, library_type);
    if (install_name) {
        stl_le_p(output + INFERNO_METAL_COMPILER_LIBRARY_FLAGS_OFFSET,
                 INFERNO_METAL_COMPILER_LIBRARY_FLAG_HAS_INSTALL_NAME);
        stl_le_p(output + INFERNO_METAL_COMPILER_LIBRARY_NAME_LENGTH_OFFSET,
                 [install_data length]);
        memcpy(output + INFERNO_METAL_COMPILER_LIBRARY_NAME_OFFSET,
               [install_data bytes], [install_data length]);
    }
    uint8_t *wire = output + INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET;
    for (NSUInteger i = 0; i < [functions count]; i++) {
        id<MTLFunction> function = [functions objectAtIndex:i];
        NSData *name = [[function name] dataUsingEncoding:NSUTF8StringEncoding];
        NSArray *constant_list = [constants objectAtIndex:i];
        id vv = [vertices objectAtIndex:i];
        id sv = [stages objectAtIndex:i];
        NSArray *vertex_list = vv == [NSNull null] ? nil : vv;
        NSArray *stage_list = sv == [NSNull null] ? nil : sv;
        uint32_t type;
        metal_function_type(&type, [function functionType]);
        uint32_t flags =
            (vertex_list ?
                 INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_VERTEX_ATTRIBUTES :
                 0) |
            (stage_list ?
                 INFERNO_METAL_COMPILER_FUNCTION_FLAG_HAS_STAGE_INPUT_ATTRIBUTES :
                 0);
        uint32_t count = (uint32_t)([constant_list count] +
                                    [vertex_list count] + [stage_list count]);
        uint32_t size = INFERNO_METAL_COMPILER_FUNCTION_RECORD_BASE_SIZE +
                        count * INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
        stl_le_p(wire + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET, type);
        stl_le_p(wire + INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET,
                 [name length]);
        stl_le_p(wire + INFERNO_METAL_COMPILER_RECORD_PATCH_TYPE_OFFSET,
                 [function patchType]);
        stl_le_p(wire + INFERNO_METAL_COMPILER_RECORD_FLAGS_OFFSET, flags);
        stq_le_p(
            wire +
                INFERNO_METAL_COMPILER_RECORD_PATCH_CONTROL_POINT_COUNT_OFFSET,
            (uint64_t)(int64_t)[function patchControlPointCount]);
        stq_le_p(wire + INFERNO_METAL_COMPILER_RECORD_OPTIONS_OFFSET,
                 [function options]);
        stl_le_p(wire + INFERNO_METAL_COMPILER_RECORD_CONSTANT_COUNT_OFFSET,
                 [constant_list count]);
        stl_le_p(
            wire + INFERNO_METAL_COMPILER_RECORD_VERTEX_ATTRIBUTE_COUNT_OFFSET,
            [vertex_list count]);
        stl_le_p(
            wire +
                INFERNO_METAL_COMPILER_RECORD_STAGE_INPUT_ATTRIBUTE_COUNT_OFFSET,
            [stage_list count]);
        stl_le_p(wire + INFERNO_METAL_COMPILER_RECORD_SIZE_OFFSET, size);
        memcpy(wire + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET, [name bytes],
               [name length]);
        uint8_t *metadata =
            wire + INFERNO_METAL_COMPILER_RECORD_METADATA_OFFSET;
        for (id item in constant_list) {
            metal_write_metadata(
                metadata, INFERNO_METAL_COMPILER_METADATA_CONSTANT, item);
            metadata += INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
        }
        for (id item in vertex_list) {
            metal_write_metadata(
                metadata, INFERNO_METAL_COMPILER_METADATA_VERTEX_ATTRIBUTE,
                item);
            metadata += INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
        }
        for (id item in stage_list) {
            metal_write_metadata(
                metadata, INFERNO_METAL_COMPILER_METADATA_STAGE_INPUT_ATTRIBUTE,
                item);
            metadata += INFERNO_METAL_COMPILER_METADATA_RECORD_SIZE;
        }
        wire += size;
    }
    return metal_query_finish(c, output, INFERNO_METAL_COMPILER_OUTCOME_OK,
                              INFERNO_METAL_COMPILER_PHASE_INVENTORY, warning,
                              nil, warning != nil);
}

static bool metal_v5_render_metadata(const InfernoMetalCommand *c,
                                     id<MTLRenderPipelineState> pipeline,
                                     uint8_t *output, char *message,
                                     size_t message_size)
{
    if (!@available(macOS 26.0, *)) {
        return metal_error(message, message_size,
                           "render pipeline metadata is unavailable", nil);
    }
    uint8_t *r = output + INFERNO_METAL_RESOURCE_COMPILER_RENDER_RESULT_OFFSET;
    stq_le_p(r + INFERNO_METAL_RESOURCE_RENDER_RESULT_ALLOCATED_SIZE_OFFSET,
             [pipeline allocatedSize]);
    stq_le_p(
        r + INFERNO_METAL_RESOURCE_RENDER_RESULT_IMAGEBLOCK_SAMPLE_LENGTH_OFFSET,
        [pipeline imageblockSampleLength]);
    stq_le_p(r + INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_TOTAL_THREADS_OFFSET,
             [pipeline maxTotalThreadsPerThreadgroup]);
    stq_le_p(r + INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_OBJECT_THREADS_OFFSET,
             [pipeline maxTotalThreadsPerObjectThreadgroup]);
    stq_le_p(r + INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_MESH_THREADS_OFFSET,
             [pipeline maxTotalThreadsPerMeshThreadgroup]);
    stq_le_p(
        r + INFERNO_METAL_RESOURCE_RENDER_RESULT_OBJECT_EXECUTION_WIDTH_OFFSET,
        [pipeline objectThreadExecutionWidth]);
    stq_le_p(
        r + INFERNO_METAL_RESOURCE_RENDER_RESULT_MESH_EXECUTION_WIDTH_OFFSET,
        [pipeline meshThreadExecutionWidth]);
    stq_le_p(
        r + INFERNO_METAL_RESOURCE_RENDER_RESULT_MAX_MESH_THREADGROUPS_OFFSET,
        [pipeline maxTotalThreadgroupsPerMeshGrid]);
    stq_le_p(r + INFERNO_METAL_RESOURCE_RENDER_RESULT_SHADER_VALIDATION_OFFSET,
             (uint64_t)(int64_t)[pipeline shaderValidation]);
    MTLSize sizes[] = { [pipeline requiredThreadsPerTileThreadgroup],
                        [pipeline requiredThreadsPerObjectThreadgroup],
                        [pipeline requiredThreadsPerMeshThreadgroup] };
    size_t offsets[] = {
        INFERNO_METAL_RESOURCE_RENDER_RESULT_TILE_THREADS_OFFSET,
        INFERNO_METAL_RESOURCE_RENDER_RESULT_OBJECT_THREADS_OFFSET,
        INFERNO_METAL_RESOURCE_RENDER_RESULT_MESH_THREADS_OFFSET,
    };
    for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
        stq_le_p(r + offsets[i], sizes[i].width);
        stq_le_p(r + offsets[i] + 8, sizes[i].height);
        stq_le_p(r + offsets[i] + 16, sizes[i].depth);
    }
    uint32_t flags =
        ([pipeline supportIndirectCommandBuffers] ?
             INFERNO_METAL_RESOURCE_RENDER_RESULT_SUPPORTS_INDIRECT :
             0) |
        ([pipeline threadgroupSizeMatchesTileSize] ?
             INFERNO_METAL_RESOURCE_RENDER_RESULT_THREADGROUP_MATCHES_TILE :
             0);
    stl_le_p(r + INFERNO_METAL_RESOURCE_RENDER_RESULT_FLAGS_OFFSET, flags);
    return true;
}

bool inferno_metal_backend_typed_query(InfernoMetalBackend *backend,
                                       const InfernoMetalCommand *c,
                                       const uint8_t *input, uint8_t *output,
                                       char *message, size_t message_size)
{
    @autoreleasepool {
        InfernoMetalTypedQueryView view;
        InfernoMetalBatchParseError parse_error;
        if (!backend || !c || !input || !output ||
            c->opcode < INFERNO_METAL_QUERY_LIBRARY_TYPED ||
            c->opcode > INFERNO_METAL_QUERY_ARGUMENT_LAYOUT ||
            c->input_size < INFERNO_METAL_RESOURCE_QUERY_HEADER_SIZE ||
            c->input_size > INFERNO_METAL_MAX_BUFFER ||
            c->output_size < INFERNO_METAL_COMPILER_MIN_OUTPUT ||
            c->output_size > INFERNO_METAL_COMPILER_MAX_OUTPUT ||
            (c->opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT &&
             c->output_size != INFERNO_METAL_ARGUMENT_LAYOUT_OUTPUT_SIZE)) {
            snprintf(message, message_size,
                     "typed query buffers are outside the v5 contract");
            return false;
        }
        metal_v5_query_initialize(c, output);
        if (!inferno_metal_typed_query_parse(c->opcode, input, c->input_size,
                                             &view, &parse_error)) {
            return metal_error(message, message_size,
                               "malformed typed query manifest", nil);
        }
        NSMutableArray *libraries =
            [NSMutableArray arrayWithCapacity:view.library_count];
        NSMutableArray *warnings =
            [NSMutableArray arrayWithCapacity:view.library_count];
        MetalV5Failure failure = { 0 };
        for (uint32_t i = 0; i < view.library_count; i++) {
            const uint8_t *record = inferno_metal_typed_query_library(&view, i);
            NSError *warning = nil;
            id library = metal_v5_library(backend, c, record,
                                          input + view.payload_offset, &warning,
                                          &failure, i);
            if (!library) {
                return metal_v5_query_finish(c, output, &failure);
            }
            [libraries addObject:library];
            [warnings addObject:warning ?: [NSNull null]];
        }
        if (c->opcode == INFERNO_METAL_QUERY_LIBRARY_TYPED) {
            id warning = [warnings objectAtIndex:0];
            return metal_v5_library_inventory(
                c, [libraries objectAtIndex:0],
                warning == [NSNull null] ? nil : warning, output, message,
                message_size);
        }
        const uint8_t *pipeline_record =
            inferno_metal_typed_query_pipeline(&view);
        if (c->opcode == INFERNO_METAL_QUERY_RENDER_PIPELINE) {
            uint32_t vi = ldl_le_p(
                pipeline_record +
                INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_LIBRARY_OFFSET);
            uint32_t fi = ldl_le_p(
                pipeline_record +
                INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_LIBRARY_OFFSET);
            MTLRenderPipelineReflection *reflection = nil;
            NSError *diagnostic = nil;
            id vertex_warning = [warnings objectAtIndex:vi];
            id fragment_warning = [warnings objectAtIndex:fi];
            id pipeline = metal_v5_render_pipeline(
                backend, c, pipeline_record,
                inferno_metal_typed_query_library(&view, vi),
                inferno_metal_typed_query_library(&view, fi),
                [libraries objectAtIndex:vi], [libraries objectAtIndex:fi],
                input + view.payload_offset,
                vertex_warning != [NSNull null] ?
                    vertex_warning :
                    (fragment_warning != [NSNull null] ? fragment_warning :
                                                         nil),
                &failure, 0, &reflection, &diagnostic);
            if (!pipeline) {
                return metal_v5_query_finish(c, output, &failure);
            }
            if (!metal_v5_render_metadata(c, pipeline, output, message,
                                          message_size)) {
                return false;
            }
            return metal_query_finish(c, output,
                                      INFERNO_METAL_COMPILER_OUTCOME_OK,
                                      INFERNO_METAL_COMPILER_PHASE_PIPELINE,
                                      diagnostic, nil, diagnostic != nil);
        }
        MTLComputePipelineReflection *reflection = nil;
        NSError *diagnostic = nil;
        id library_warning = [warnings objectAtIndex:0];
        id pipeline = metal_v5_compute_pipeline(
            backend, c, pipeline_record,
            inferno_metal_typed_query_library(&view, 0),
            [libraries objectAtIndex:0], input + view.payload_offset,
            library_warning == [NSNull null] ? nil : library_warning, &failure,
            0, &reflection, &diagnostic);
        if (!pipeline) {
            return metal_v5_query_finish(c, output, &failure);
        }
        if (c->opcode == INFERNO_METAL_QUERY_ARGUMENT_LAYOUT) {
            NSString *reason = nil;
            bool resource_failed = false;
            NSDictionary *layout = metal_v5_argument_layout(
                backend, pipeline_record,
                inferno_metal_typed_query_library(&view, 0),
                [libraries objectAtIndex:0], input + view.payload_offset,
                reflection, view.argument_buffer_index, &failure,
                INFERNO_METAL_BATCH_RECORD_PIPELINE, 0, &reason,
                &resource_failed);
            if (!layout && failure.outcome) {
                return metal_v5_query_finish(c, output, &failure);
            }
            return metal_v5_argument_query_finish(c, output, layout, diagnostic,
                                                  reason);
        }
        if (!metal_query_pipeline_limits(c, pipeline, output, message,
                                         message_size)) {
            return false;
        }
        if (c->opcode == INFERNO_METAL_QUERY_IMAGEBLOCK_TYPED) {
            stq_le_p(
                output +
                    INFERNO_METAL_COMPILER_PIPELINE_IMAGEBLOCK_MEMORY_LENGTH_OFFSET,
                [pipeline
                    imageblockMemoryLengthForDimensions:MTLSizeMake(c->width,
                                                                    c->height,
                                                                    c->depth)]);
        }
        return metal_query_finish(c, output, INFERNO_METAL_COMPILER_OUTCOME_OK,
                                  INFERNO_METAL_COMPILER_PHASE_PIPELINE,
                                  diagnostic, nil, diagnostic != nil);
    }
}

static void metal_v5_batch_initialize(const InfernoMetalCommand *c,
                                      uint8_t *output)
{
    memset(output, 0, c->output_size);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_VERSION_OFFSET,
             INFERNO_METAL_RESOURCE_VERSION);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_OPCODE_OFFSET,
             INFERNO_METAL_BATCH_RESOURCES);
    stq_le_p(output + INFERNO_METAL_BATCH_RESULT_SEQUENCE_OFFSET, c->sequence);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FAILED_KIND_OFFSET,
             INFERNO_METAL_BATCH_RECORD_UNKNOWN);
    stl_le_p(output + INFERNO_METAL_BATCH_RESULT_FAILED_INDEX_OFFSET,
             INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN);
}

static bool metal_v5_batch_finish(const InfernoMetalCommand *c, uint8_t *output,
                                  const InfernoMetalResourceBatchView *view,
                                  MetalV5Failure *failure, bool scheduled)
{
    if (view) {
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_BUFFER_COUNT_OFFSET,
                 view->buffer_count);
        stl_le_p(output + INFERNO_METAL_RESOURCE_RESULT_TEXTURE_COUNT_OFFSET,
                 view->texture_count);
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_IMAGES_SIZE_OFFSET,
                 view->images_size);
    }
    return metal_batch_finish(c, output, failure->outcome, failure->phase,
                              failure->kind, failure->index, failure->error,
                              failure->explanation, scheduled);
}

static void metal_v5_fail(MetalV5Failure *failure, uint32_t outcome,
                          uint32_t phase, uint32_t kind, uint32_t index,
                          NSString *explanation)
{
    failure->outcome = outcome;
    failure->phase = phase;
    failure->kind = kind;
    failure->index = index;
    failure->error = nil;
    failure->explanation = explanation;
}

static float metal_v5_float(const uint8_t *bytes)
{
    uint32_t bits = ldl_le_p(bytes);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static double metal_v5_double(const uint8_t *bytes)
{
    uint64_t bits = ldq_le_p(bytes);
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static MTLTextureUsage metal_v5_texture_usage(uint32_t usage)
{
    MTLTextureUsage result = 0;
    if (usage & INFERNO_METAL_RESOURCE_TEXTURE_USAGE_SHADER_READ) {
        result |= MTLTextureUsageShaderRead;
    }
    if (usage & INFERNO_METAL_RESOURCE_TEXTURE_USAGE_SHADER_WRITE) {
        result |= MTLTextureUsageShaderWrite;
    }
    if (usage & INFERNO_METAL_RESOURCE_TEXTURE_USAGE_RENDER_TARGET) {
        result |= MTLTextureUsageRenderTarget;
    }
    return result;
}

static bool metal_v5_host_family(id<MTLDevice> device)
{
    if (@available(macOS 15.0, *)) {
        if ([device supportsFamily:MTLGPUFamilyApple10]) {
            return true;
        }
    }
    if (@available(macOS 14.0, *)) {
        if ([device supportsFamily:MTLGPUFamilyApple9]) {
            return true;
        }
    }
    return [device supportsFamily:MTLGPUFamilyApple8] ||
           [device supportsFamily:MTLGPUFamilyApple7] ||
           [device supportsFamily:MTLGPUFamilyApple6] ||
           [device supportsFamily:MTLGPUFamilyApple5] ||
           [device supportsFamily:MTLGPUFamilyApple4] ||
           [device supportsFamily:MTLGPUFamilyApple3] ||
           [device supportsFamily:MTLGPUFamilyApple2];
}

static id<MTLBinding> metal_v5_reflection_binding(NSArray<id<MTLBinding>> *list,
                                                  MTLBindingType type,
                                                  NSUInteger index)
{
    for (id<MTLBinding> binding in list) {
        if ([binding type] == type && [binding index] == index) {
            return binding;
        }
    }
    return nil;
}

static bool metal_v5_validate_texture_bindings(
    const InfernoMetalResourceBatchView *view, uint32_t start, uint32_t count,
    NSArray<id<MTLBinding>> *reflection, NSArray *textures,
    MetalV5Failure *failure, uint32_t owner_kind, uint32_t owner_index)
{
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *record =
            inferno_metal_resource_batch_binding(view, start + i);
        if (ldl_le_p(record + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET) !=
            INFERNO_METAL_RESOURCE_BINDING_TEXTURE) {
            continue;
        }
        uint32_t slot =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET);
        id<MTLBinding> binding = metal_v5_reflection_binding(
            reflection, MTLBindingTypeTexture, slot);
        if (!binding || ![binding isUsed] ||
            ![binding conformsToProtocol:@protocol(MTLTextureBinding)]) {
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE, owner_kind, owner_index,
                @"texture binding is absent from final pipeline reflection");
            return false;
        }
        id<MTLTextureBinding> texture_binding = (id<MTLTextureBinding>)binding;
        uint32_t resource =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET);
        id<MTLTexture> texture = [textures objectAtIndex:resource];
        if ([texture_binding textureType] != MTLTextureType2D ||
            [texture_binding arrayLength] != 1 ||
            [texture_binding isDepthTexture] ||
            ([texture_binding textureDataType] != MTLDataTypeFloat &&
             [texture_binding textureDataType] != MTLDataTypeHalf)) {
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE, owner_kind, owner_index,
                @"pipeline texture binding shape is unsupported");
            return false;
        }
        MTLBindingAccess access = [binding access];
        MTLTextureUsage usage = [texture usage];
        if (access > MTLBindingAccessWriteOnly ||
            (access == MTLBindingAccessReadOnly &&
             !(usage & MTLTextureUsageShaderRead)) ||
            (access == MTLBindingAccessWriteOnly &&
             !(usage & MTLTextureUsageShaderWrite)) ||
            (access == MTLBindingAccessReadWrite &&
             (usage &
              (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)) !=
                 (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite))) {
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE, owner_kind, owner_index,
                @"texture usage does not satisfy final pipeline access");
            return false;
        }
    }
    for (id<MTLBinding> binding in reflection) {
        if (![binding isUsed] || [binding type] != MTLBindingTypeTexture) {
            continue;
        }
        bool found = false;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_binding(view, start + i);
            found =
                ldl_le_p(record + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET) ==
                    INFERNO_METAL_RESOURCE_BINDING_TEXTURE &&
                ldl_le_p(record +
                         INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET) ==
                    [binding index];
            if (found) {
                break;
            }
        }
        if (!found) {
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE, owner_kind, owner_index,
                @"used pipeline texture has no encoded binding");
            return false;
        }
    }
    return true;
}

static bool metal_v5_validate_plain_buffer_bindings(
    const InfernoMetalResourceBatchView *view, uint32_t start, uint32_t count,
    NSArray<id<MTLBinding>> *reflection, MetalV5Failure *failure,
    uint32_t owner_kind, uint32_t owner_index)
{
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *record =
            inferno_metal_resource_batch_binding(view, start + i);
        uint32_t kind =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET);
        if (kind != INFERNO_METAL_RESOURCE_BINDING_BUFFER &&
            kind != INFERNO_METAL_RESOURCE_BINDING_INLINE) {
            continue;
        }
        uint32_t slot =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET);
        id<MTLBinding> binding =
            metal_v5_reflection_binding(reflection, MTLBindingTypeBuffer, slot);
        if (!binding ||
            ![binding conformsToProtocol:@protocol(MTLBufferBinding)]) {
            continue;
        }
        id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)binding;
        MTLPointerType *pointer = [buffer bufferPointerType];
        if ((pointer && [pointer elementIsArgumentBuffer]) ||
            (!pointer && ([buffer bufferDataType] == MTLDataTypeStruct ||
                          [buffer bufferDataType] == MTLDataTypeArray))) {
            uint32_t failure_index =
                owner_kind == INFERNO_METAL_BATCH_RECORD_BINDING ? start + i :
                                                                   owner_index;
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE, owner_kind, failure_index,
                pointer ? @"argument buffer slot has a plain buffer binding" :
                          @"buffer root metadata is unavailable");
            return false;
        }
    }
    return true;
}

static bool
metal_v5_validate_declarations(const InfernoMetalResourceBatchView *view,
                               const uint8_t *command, NSArray *textures,
                               MetalV5Failure *failure)
{
    uint32_t start = ldl_le_p(
        command +
        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_START_OFFSET);
    uint32_t count = ldl_le_p(
        command +
        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_COUNT_OFFSET);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t declaration_index = start + i;
        const uint8_t *declaration =
            inferno_metal_resource_batch_declaration(view, declaration_index);
        if (ldl_le_p(declaration +
                     INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_KIND_OFFSET) !=
            INFERNO_METAL_RESOURCE_DECLARATION_TEXTURE) {
            continue;
        }
        uint32_t resource = ldl_le_p(
            declaration + INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_OFFSET);
        MTLResourceUsage usage = (MTLResourceUsage)ldl_le_p(
            declaration + INFERNO_METAL_RESOURCE_DECLARATION_USAGE_OFFSET);
        id<MTLTexture> texture = [textures objectAtIndex:resource];
        MTLTextureUsage texture_usage = [texture usage];
        if (((usage & MTLResourceUsageRead) &&
             !(texture_usage & MTLTextureUsageShaderRead)) ||
            ((usage & MTLResourceUsageWrite) &&
             !(texture_usage & MTLTextureUsageShaderWrite))) {
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE,
                INFERNO_METAL_RESOURCE_RECORD_DECLARATION, declaration_index,
                @"texture declaration usage is incompatible with the resource");
            return false;
        }
    }
    return true;
}

static NSDictionary *metal_v5_argument_member_for_id(NSArray *members,
                                                     uint32_t member_id)
{
    for (NSDictionary *member in members) {
        uint32_t base = [member[@"id"] unsignedIntValue];
        uint32_t length = [member[@"array_length"] unsignedIntValue];
        uint32_t stride = [member[@"index_stride"] unsignedIntValue];
        uint32_t count = length ? length : 1;
        for (uint32_t i = 0; i < count; i++) {
            if (base + i * stride == member_id) {
                return member;
            }
        }
    }
    return nil;
}

static uint32_t metal_v5_argument_expanded_count(NSArray *members)
{
    uint32_t count = 0;
    for (NSDictionary *member in members) {
        uint32_t length = [member[@"array_length"] unsignedIntValue];
        count += length ? length : 1;
    }
    return count;
}

static bool metal_v5_validate_argument_binding(
    InfernoMetalBackend *backend, const InfernoMetalResourceBatchView *view,
    const uint8_t *binding, uint32_t argument_index,
    const uint8_t *pipeline_record, const uint8_t *library_record,
    id<MTLLibrary> library, MTLComputePipelineReflection *reflection,
    NSArray *textures, NSMutableArray *argument_layouts, uint32_t *buffer_total,
    uint32_t *texture_total, uint32_t *sampler_total, MetalV5Failure *failure)
{
    const uint8_t *argument =
        inferno_metal_resource_batch_argument(view, argument_index);
    uint32_t slot =
        ldl_le_p(binding + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET);
    NSString *reason = nil;
    bool resource_failed = false;
    NSDictionary *layout = metal_v5_argument_layout(
        backend, pipeline_record, library_record, library,
        view->bytes + view->payload_offset, reflection, slot, failure,
        INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, argument_index, &reason,
        &resource_failed);
    if (!layout) {
        if (!failure->outcome) {
            metal_v5_fail(
                failure,
                resource_failed ?
                    INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED :
                    INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                resource_failed ? INFERNO_METAL_RESOURCE_PHASE_RESOURCE :
                                  INFERNO_METAL_BATCH_PHASE_VALIDATE,
                INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, argument_index,
                reason ?: @"argument layout is unavailable");
        }
        return false;
    }
    if ([layout[@"encoded_length"] unsignedIntValue] !=
            ldl_le_p(argument +
                     INFERNO_METAL_RESOURCE_ARGUMENT_ENCODED_LENGTH_OFFSET) ||
        [layout[@"alignment"] unsignedIntValue] !=
            ldl_le_p(argument +
                     INFERNO_METAL_RESOURCE_ARGUMENT_ALIGNMENT_OFFSET)) {
        metal_v5_fail(failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                      INFERNO_METAL_BATCH_PHASE_VALIDATE,
                      INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, argument_index,
                      @"argument encoder length or alignment disagrees");
        return false;
    }
    NSArray *members = layout[@"members"];
    uint32_t member_start = ldl_le_p(
        argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_START_OFFSET);
    uint32_t member_count = ldl_le_p(
        argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_COUNT_OFFSET);
    if (member_count != metal_v5_argument_expanded_count(members)) {
        metal_v5_fail(failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                      INFERNO_METAL_BATCH_PHASE_VALIDATE,
                      INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, argument_index,
                      @"argument assignment set is incomplete");
        return false;
    }
    NSMutableSet *assigned = [NSMutableSet setWithCapacity:member_count];
    for (uint32_t i = 0; i < member_count; i++) {
        uint32_t member_index = member_start + i;
        const uint8_t *record =
            inferno_metal_resource_batch_member(view, member_index);
        uint32_t member_id =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_MEMBER_ID_OFFSET);
        uint32_t kind =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_MEMBER_KIND_OFFSET);
        uint32_t resource =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_MEMBER_RESOURCE_OFFSET);
        uint32_t length =
            ldl_le_p(record + INFERNO_METAL_RESOURCE_MEMBER_LENGTH_OFFSET);
        uint64_t offset =
            ldq_le_p(record + INFERNO_METAL_RESOURCE_MEMBER_OFFSET_OFFSET);
        NSDictionary *descriptor =
            metal_v5_argument_member_for_id(members, member_id);
        NSNumber *key = @(member_id);
        bool valid = descriptor && ![assigned containsObject:key] &&
                     kind == [descriptor[@"kind"] unsignedIntValue];
        if (valid && kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER) {
            uint32_t alignment =
                [descriptor[@"pointer_alignment"] unsignedIntValue];
            valid = alignment && !(offset % alignment);
            (*buffer_total)++;
        } else if (valid &&
                   kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE) {
            id<MTLTexture> texture = [textures objectAtIndex:resource];
            valid = [texture textureType] == MTLTextureType2D &&
                    [texture arrayLength] == 1 &&
                    ([descriptor[@"texture_data_type"] unsignedIntValue] ==
                         MTLDataTypeFloat ||
                     [descriptor[@"texture_data_type"] unsignedIntValue] ==
                         MTLDataTypeHalf) &&
                    ([texture usage] & MTLTextureUsageShaderRead);
            (*texture_total)++;
        } else if (valid &&
                   kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER) {
            (*sampler_total)++;
        } else if (valid &&
                   kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
            valid = length == [descriptor[@"constant_size"] unsignedIntValue] &&
                    [descriptor[@"byte_offset"] unsignedLongLongValue] <=
                        [layout[@"encoded_length"] unsignedLongLongValue] &&
                    length <=
                        [layout[@"encoded_length"] unsignedLongLongValue] -
                            [descriptor[@"byte_offset"] unsignedLongLongValue];
        }
        if (!valid || *buffer_total > 31 || *texture_total > 31 ||
            *sampler_total > 16) {
            metal_v5_fail(
                failure, INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                INFERNO_METAL_BATCH_PHASE_VALIDATE,
                INFERNO_METAL_RESOURCE_RECORD_MEMBER, member_index,
                valid ? @"argument resource count exceeds host limits" :
                        @"argument member disagrees with native layout");
            return false;
        }
        [assigned addObject:key];
    }
    [argument_layouts replaceObjectAtIndex:argument_index withObject:layout];
    return true;
}

static bool metal_v5_prepare_arguments(
    InfernoMetalBackend *backend, const InfernoMetalResourceBatchView *view,
    NSArray *argument_layouts, NSArray *buffers, NSArray *textures,
    NSArray *samplers, NSMutableArray *backings, MetalV5Failure *failure)
{
    for (uint32_t i = 0; i < view->argument_count; i++) {
        NSDictionary *layout = [argument_layouts objectAtIndex:i];
        if (layout == (id)[NSNull null]) {
            metal_v5_fail(failure,
                          INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                          INFERNO_METAL_BATCH_PHASE_VALIDATE,
                          INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, i,
                          @"argument record has no validated binding");
            return false;
        }
        NSUInteger encoded_length =
            [layout[@"encoded_length"] unsignedIntegerValue];
        id<MTLBuffer> backing = [backend->device
            newBufferWithLength:encoded_length
                        options:MTLResourceStorageModeShared |
                                MTLResourceHazardTrackingModeTracked];
        if (!backing) {
            metal_v5_fail(failure,
                          INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                          INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                          INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, i,
                          @"Metal returned no argument backing buffer");
            return false;
        }
        memset([backing contents], 0, encoded_length);
        [backings addObject:backing];
        [backing release];
        id<MTLArgumentEncoder> encoder = layout[@"encoder"];
        [encoder setArgumentBuffer:backing offset:0];
        const uint8_t *argument =
            inferno_metal_resource_batch_argument(view, i);
        uint32_t start = ldl_le_p(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_START_OFFSET);
        uint32_t count = ldl_le_p(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_COUNT_OFFSET);
        for (uint32_t j = 0; j < count; j++) {
            const uint8_t *member =
                inferno_metal_resource_batch_member(view, start + j);
            uint32_t kind =
                ldl_le_p(member + INFERNO_METAL_RESOURCE_MEMBER_KIND_OFFSET);
            uint32_t member_id =
                ldl_le_p(member + INFERNO_METAL_RESOURCE_MEMBER_ID_OFFSET);
            uint32_t resource = ldl_le_p(
                member + INFERNO_METAL_RESOURCE_MEMBER_RESOURCE_OFFSET);
            uint64_t offset =
                ldq_le_p(member + INFERNO_METAL_RESOURCE_MEMBER_OFFSET_OFFSET);
            if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER) {
                [encoder setBuffer:[buffers objectAtIndex:resource]
                            offset:(NSUInteger)offset
                           atIndex:member_id];
            } else if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE) {
                [encoder setTexture:[textures objectAtIndex:resource]
                            atIndex:member_id];
            } else if (kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_SAMPLER) {
                [encoder setSamplerState:[samplers objectAtIndex:resource]
                                 atIndex:member_id];
            } else {
                uint32_t length = ldl_le_p(
                    member + INFERNO_METAL_RESOURCE_MEMBER_LENGTH_OFFSET);
                void *constant = [encoder constantDataAtIndex:member_id];
                if (!constant) {
                    metal_v5_fail(
                        failure, INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                        INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                        INFERNO_METAL_RESOURCE_RECORD_ARGUMENT, i,
                        @"Metal returned no argument constant storage");
                    return false;
                }
                memcpy(constant,
                       view->bytes + view->constants_offset + resource, length);
            }
        }
    }
    return true;
}

static void
metal_v5_set_compute_bindings(id<MTLComputeCommandEncoder> encoder,
                              const InfernoMetalResourceBatchView *view,
                              uint32_t start, uint32_t count, NSArray *buffers,
                              NSArray *textures, NSArray *samplers,
                              NSArray *argument_backings)
{
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *b =
            inferno_metal_resource_batch_binding(view, start + i);
        uint32_t kind =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET);
        uint32_t index =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET);
        uint32_t resource =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET);
        uint32_t length =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_LENGTH_OFFSET);
        NSUInteger offset = (NSUInteger)ldq_le_p(
            b + INFERNO_METAL_RESOURCE_BINDING_OFFSET_OFFSET);
        switch (kind) {
        case INFERNO_METAL_RESOURCE_BINDING_BUFFER:
            [encoder setBuffer:[buffers objectAtIndex:resource]
                        offset:offset
                       atIndex:index];
            break;
        case INFERNO_METAL_RESOURCE_BINDING_INLINE:
            [encoder setBytes:view->bytes + view->inline_offset + resource
                       length:length
                      atIndex:index];
            break;
        case INFERNO_METAL_RESOURCE_BINDING_THREADGROUP:
            [encoder setThreadgroupMemoryLength:length atIndex:index];
            break;
        case INFERNO_METAL_RESOURCE_BINDING_TEXTURE:
            [encoder setTexture:[textures objectAtIndex:resource]
                        atIndex:index];
            break;
        case INFERNO_METAL_RESOURCE_BINDING_SAMPLER:
            [encoder setSamplerState:[samplers objectAtIndex:resource]
                             atIndex:index];
            break;
        case INFERNO_METAL_RESOURCE_BINDING_ARGUMENT:
            [encoder setBuffer:[argument_backings objectAtIndex:resource]
                        offset:0
                       atIndex:index];
            break;
        default:
            g_assert_not_reached();
        }
    }
}

static void metal_v5_use_compute_resources(
    id<MTLComputeCommandEncoder> encoder,
    const InfernoMetalResourceBatchView *view, const uint8_t *command,
    uint32_t binding_start, uint32_t binding_count, NSArray *argument_layouts,
    NSArray *buffers, NSArray *textures)
{
    for (uint32_t i = 0; i < binding_count; i++) {
        const uint8_t *binding =
            inferno_metal_resource_batch_binding(view, binding_start + i);
        if (ldl_le_p(binding + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET) !=
            INFERNO_METAL_RESOURCE_BINDING_ARGUMENT) {
            continue;
        }
        uint32_t argument_index =
            ldl_le_p(binding + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET);
        const uint8_t *argument =
            inferno_metal_resource_batch_argument(view, argument_index);
        NSDictionary *layout = [argument_layouts objectAtIndex:argument_index];
        NSArray *descriptors = layout[@"members"];
        uint32_t start = ldl_le_p(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_START_OFFSET);
        uint32_t count = ldl_le_p(
            argument + INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_COUNT_OFFSET);
        for (uint32_t j = 0; j < count; j++) {
            const uint8_t *member =
                inferno_metal_resource_batch_member(view, start + j);
            uint32_t kind =
                ldl_le_p(member + INFERNO_METAL_RESOURCE_MEMBER_KIND_OFFSET);
            if (kind != INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER &&
                kind != INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_TEXTURE) {
                continue;
            }
            uint32_t member_id =
                ldl_le_p(member + INFERNO_METAL_RESOURCE_MEMBER_ID_OFFSET);
            uint32_t resource = ldl_le_p(
                member + INFERNO_METAL_RESOURCE_MEMBER_RESOURCE_OFFSET);
            NSDictionary *descriptor =
                metal_v5_argument_member_for_id(descriptors, member_id);
            MTLResourceUsage usage = MTLResourceUsageRead;
            if ([descriptor[@"access"] unsignedIntValue] !=
                MTLBindingAccessReadOnly) {
                usage |= MTLResourceUsageWrite;
            }
            [encoder useResource:
                         kind == INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_BUFFER ?
                             [buffers objectAtIndex:resource] :
                             [textures objectAtIndex:resource]
                           usage:usage];
        }
    }
    uint32_t declaration_start = ldl_le_p(
        command +
        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_START_OFFSET);
    uint32_t declaration_count = ldl_le_p(
        command +
        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_DECLARATION_COUNT_OFFSET);
    for (uint32_t i = 0; i < declaration_count; i++) {
        const uint8_t *declaration = inferno_metal_resource_batch_declaration(
            view, declaration_start + i);
        uint32_t kind =
            ldl_le_p(declaration +
                     INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_KIND_OFFSET);
        uint32_t resource = ldl_le_p(
            declaration + INFERNO_METAL_RESOURCE_DECLARATION_RESOURCE_OFFSET);
        MTLResourceUsage usage = (MTLResourceUsage)ldl_le_p(
            declaration + INFERNO_METAL_RESOURCE_DECLARATION_USAGE_OFFSET);
        [encoder useResource:kind == INFERNO_METAL_RESOURCE_DECLARATION_BUFFER ?
                                 [buffers objectAtIndex:resource] :
                                 [textures objectAtIndex:resource]
                       usage:usage];
    }
}

static void
metal_v5_set_render_bindings(id<MTLRenderCommandEncoder> encoder, bool vertex,
                             const InfernoMetalResourceBatchView *view,
                             uint32_t start, uint32_t count, NSArray *buffers,
                             NSArray *textures, NSArray *samplers)
{
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *b =
            inferno_metal_resource_batch_binding(view, start + i);
        uint32_t kind =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET);
        uint32_t index =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_INDEX_OFFSET);
        uint32_t resource =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET);
        uint32_t length =
            ldl_le_p(b + INFERNO_METAL_RESOURCE_BINDING_LENGTH_OFFSET);
        NSUInteger offset = (NSUInteger)ldq_le_p(
            b + INFERNO_METAL_RESOURCE_BINDING_OFFSET_OFFSET);
        if (kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
            if (vertex) {
                [encoder setVertexBuffer:[buffers objectAtIndex:resource]
                                  offset:offset
                                 atIndex:index];
            } else {
                [encoder setFragmentBuffer:[buffers objectAtIndex:resource]
                                    offset:offset
                                   atIndex:index];
            }
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_INLINE) {
            const void *bytes = view->bytes + view->inline_offset + resource;
            if (vertex) {
                [encoder setVertexBytes:bytes length:length atIndex:index];
            } else {
                [encoder setFragmentBytes:bytes length:length atIndex:index];
            }
        } else if (kind == INFERNO_METAL_RESOURCE_BINDING_TEXTURE) {
            if (vertex) {
                [encoder setVertexTexture:[textures objectAtIndex:resource]
                                  atIndex:index];
            } else {
                [encoder setFragmentTexture:[textures objectAtIndex:resource]
                                    atIndex:index];
            }
        } else {
            if (vertex) {
                [encoder setVertexSamplerState:[samplers objectAtIndex:resource]
                                       atIndex:index];
            } else {
                [encoder
                    setFragmentSamplerState:[samplers objectAtIndex:resource]
                                    atIndex:index];
            }
        }
    }
}

bool inferno_metal_backend_resource_batch(InfernoMetalBackend *backend,
                                          const InfernoMetalCommand *c,
                                          const uint8_t *input, uint8_t *output,
                                          InfernoMetalProgressFn progress,
                                          void *opaque, char *message,
                                          size_t message_size)
{
    @autoreleasepool {
        InfernoMetalResourceBatchView view;
        InfernoMetalBatchParseError parse_error;
        MetalV5Failure failure = { 0 };
        if (!backend || !c || !input || !output ||
            c->opcode != INFERNO_METAL_BATCH_RESOURCES ||
            c->input_size < INFERNO_METAL_RESOURCE_HEADER_SIZE ||
            c->input_size > INFERNO_METAL_MAX_BUFFER ||
            c->output_size < INFERNO_METAL_RESOURCE_RESULT_SIZE ||
            c->output_size > INFERNO_METAL_MAX_BUFFER) {
            snprintf(message, message_size,
                     "resource batch buffers are outside the v5 contract");
            return false;
        }
        metal_v5_batch_initialize(c, output);
        if (!inferno_metal_resource_batch_parse(
                input, c->input_size, c->output_size, &view, &parse_error)) {
            metal_v5_fail(&failure, INFERNO_METAL_BATCH_OUTCOME_MALFORMED,
                          INFERNO_METAL_BATCH_PHASE_PARSE,
                          parse_error.record_kind, parse_error.record_index,
                          @"malformed resource batch manifest");
            return metal_v5_batch_finish(c, output, NULL, &failure, false);
        }
        trace_inferno_metal_resource_counts(
            c->sequence, view.library_count, view.compute_pipeline_count,
            view.render_pipeline_count, view.buffer_count, view.texture_count,
            view.sampler_count, view.command_count, view.draw_count,
            view.binding_count);
        if (!metal_v5_host_family(backend->device)) {
            metal_v5_fail(&failure,
                          INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST,
                          INFERNO_METAL_BATCH_PHASE_VALIDATE,
                          INFERNO_METAL_BATCH_RECORD_HEADER, 0,
                          @"host GPU has no supported Apple family row");
            return metal_v5_batch_finish(c, output, &view, &failure, false);
        }

        NSMutableArray *libraries =
            [NSMutableArray arrayWithCapacity:view.library_count];
        NSMutableArray *library_warnings =
            [NSMutableArray arrayWithCapacity:view.library_count];
        for (uint32_t i = 0; i < view.library_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_library(&view, i);
            NSError *warning = nil;
            id library = metal_v5_library(backend, c, record,
                                          input + view.payload_offset, &warning,
                                          &failure, i);
            if (!library) {
                return metal_v5_batch_finish(c, output, &view, &failure, false);
            }
            [libraries addObject:library];
            [library_warnings addObject:warning ?: [NSNull null]];
        }

        NSMutableArray *compute_pipelines =
            [NSMutableArray arrayWithCapacity:view.compute_pipeline_count];
        NSMutableArray *compute_reflections =
            [NSMutableArray arrayWithCapacity:view.compute_pipeline_count];
        for (uint32_t i = 0; i < view.compute_pipeline_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_compute_pipeline(&view, i);
            uint32_t library_index = ldl_le_p(
                record +
                INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET);
            id warning = [library_warnings objectAtIndex:library_index];
            MTLComputePipelineReflection *reflection = nil;
            NSError *diagnostic = nil;
            id pipeline = metal_v5_compute_pipeline(
                backend, c, record,
                inferno_metal_resource_batch_library(&view, library_index),
                [libraries objectAtIndex:library_index],
                input + view.payload_offset,
                warning == [NSNull null] ? nil : warning, &failure, i,
                &reflection, &diagnostic);
            if (!pipeline) {
                return metal_v5_batch_finish(c, output, &view, &failure, false);
            }
            [compute_pipelines addObject:pipeline];
            [compute_reflections addObject:reflection];
        }

        NSMutableArray *render_pipelines =
            [NSMutableArray arrayWithCapacity:view.render_pipeline_count];
        NSMutableArray *render_reflections =
            [NSMutableArray arrayWithCapacity:view.render_pipeline_count];
        for (uint32_t i = 0; i < view.render_pipeline_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_render_pipeline(&view, i);
            uint32_t vertex_index = ldl_le_p(
                record +
                INFERNO_METAL_RESOURCE_RENDER_PIPELINE_VERTEX_LIBRARY_OFFSET);
            uint32_t fragment_index = ldl_le_p(
                record +
                INFERNO_METAL_RESOURCE_RENDER_PIPELINE_FRAGMENT_LIBRARY_OFFSET);
            MTLRenderPipelineReflection *reflection = nil;
            NSError *diagnostic = nil;
            id vertex_warning = [library_warnings objectAtIndex:vertex_index];
            id fragment_warning =
                [library_warnings objectAtIndex:fragment_index];
            id pipeline = metal_v5_render_pipeline(
                backend, c, record,
                inferno_metal_resource_batch_library(&view, vertex_index),
                inferno_metal_resource_batch_library(&view, fragment_index),
                [libraries objectAtIndex:vertex_index],
                [libraries objectAtIndex:fragment_index],
                input + view.payload_offset,
                vertex_warning != [NSNull null] ?
                    vertex_warning :
                    (fragment_warning != [NSNull null] ? fragment_warning :
                                                         nil),
                &failure, i, &reflection, &diagnostic);
            if (!pipeline) {
                return metal_v5_batch_finish(c, output, &view, &failure, false);
            }
            [render_pipelines addObject:pipeline];
            [render_reflections addObject:reflection];
        }

        NSMutableArray *buffers =
            [NSMutableArray arrayWithCapacity:view.buffer_count];
        NSMutableArray *textures =
            [NSMutableArray arrayWithCapacity:view.texture_count];
        NSMutableArray *samplers =
            [NSMutableArray arrayWithCapacity:view.sampler_count];
        NSMutableArray *argument_layouts =
            [NSMutableArray arrayWithCapacity:view.argument_count];
        NSMutableArray *argument_backings =
            [NSMutableArray arrayWithCapacity:view.argument_count];
        for (uint32_t i = 0; i < view.argument_count; i++) {
            [argument_layouts addObject:[NSNull null]];
        }
        size_t image_cursor = 0;
        for (uint32_t i = 0; i < view.buffer_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_buffer(&view, i);
            uint32_t length =
                ldl_le_p(record + INFERNO_METAL_RESOURCE_BUFFER_LENGTH_OFFSET);
            id buffer = [[backend->device
                newBufferWithBytes:input + view.images_offset + image_cursor
                            length:length
                           options:MTLResourceStorageModeShared |
                                   MTLResourceHazardTrackingModeTracked]
                autorelease];
            if (!buffer) {
                return metal_error(message, message_size,
                                   "resource batch buffer", nil);
            }
            [buffers addObject:buffer];
            image_cursor += length;
        }
        for (uint32_t i = 0; i < view.texture_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_texture(&view, i);
            uint32_t width =
                ldl_le_p(record + INFERNO_METAL_RESOURCE_TEXTURE_WIDTH_OFFSET);
            uint32_t height =
                ldl_le_p(record + INFERNO_METAL_RESOURCE_TEXTURE_HEIGHT_OFFSET);
            MTLTextureDescriptor *descriptor =
                [[[MTLTextureDescriptor alloc] init] autorelease];
            descriptor.textureType = MTLTextureType2D;
            descriptor.pixelFormat = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_TEXTURE_PIXEL_FORMAT_OFFSET);
            descriptor.width = width;
            descriptor.height = height;
            descriptor.depth = 1;
            descriptor.mipmapLevelCount = 1;
            descriptor.sampleCount = 1;
            descriptor.arrayLength = 1;
            descriptor.cpuCacheMode = MTLCPUCacheModeDefaultCache;
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
            descriptor.usage = metal_v5_texture_usage(
                ldl_le_p(record + INFERNO_METAL_RESOURCE_TEXTURE_USAGE_OFFSET));
            descriptor.allowGPUOptimizedContents =
                (ldl_le_p(record +
                          INFERNO_METAL_RESOURCE_TEXTURE_FLAGS_OFFSET) &
                 INFERNO_METAL_RESOURCE_TEXTURE_ALLOW_GPU_OPTIMIZED_CONTENTS) !=
                0;
            descriptor.compressionType = MTLTextureCompressionTypeLossless;
            descriptor.swizzle = MTLTextureSwizzleChannelsDefault;
            descriptor.placementSparsePageSize = (MTLSparsePageSize)0;
            id<MTLTexture> texture = [[backend->device
                newTextureWithDescriptor:descriptor] autorelease];
            if (!texture) {
                metal_v5_fail(&failure,
                              INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                              INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                              INFERNO_METAL_RESOURCE_RECORD_TEXTURE, i,
                              @"Metal returned no texture");
                return metal_v5_batch_finish(c, output, &view, &failure, false);
            }
            NSUInteger row = (NSUInteger)width *
                             INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL;
            [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:0
                         withBytes:input + view.images_offset + image_cursor
                       bytesPerRow:row];
            [textures addObject:texture];
            image_cursor += row * height;
        }
        for (uint32_t i = 0; i < view.sampler_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_sampler(&view, i);
            MTLSamplerDescriptor *descriptor =
                [[[MTLSamplerDescriptor alloc] init] autorelease];
            descriptor.minFilter = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_MIN_FILTER_OFFSET);
            descriptor.magFilter = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_MAG_FILTER_OFFSET);
            descriptor.mipFilter = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_MIP_FILTER_OFFSET);
            descriptor.maxAnisotropy = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_MAX_ANISOTROPY_OFFSET);
            descriptor.sAddressMode = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_S_ADDRESS_MODE_OFFSET);
            descriptor.tAddressMode = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_T_ADDRESS_MODE_OFFSET);
            descriptor.rAddressMode = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_R_ADDRESS_MODE_OFFSET);
            descriptor.borderColor = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_BORDER_COLOR_OFFSET);
            descriptor.reductionMode = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_REDUCTION_MODE_OFFSET);
            descriptor.normalizedCoordinates = ldl_le_p(
                record +
                INFERNO_METAL_RESOURCE_SAMPLER_NORMALIZED_COORDINATES_OFFSET);
            descriptor.lodMinClamp = metal_v5_float(
                record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_MIN_CLAMP_OFFSET);
            descriptor.lodMaxClamp = metal_v5_float(
                record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_MAX_CLAMP_OFFSET);
            descriptor.lodAverage = ldl_le_p(
                record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_AVERAGE_OFFSET);
            descriptor.lodBias = metal_v5_float(
                record + INFERNO_METAL_RESOURCE_SAMPLER_LOD_BIAS_OFFSET);
            descriptor.compareFunction = ldl_le_p(
                record +
                INFERNO_METAL_RESOURCE_SAMPLER_COMPARE_FUNCTION_OFFSET);
            descriptor.supportArgumentBuffers = ldl_le_p(
                record +
                INFERNO_METAL_RESOURCE_SAMPLER_SUPPORT_ARGUMENT_BUFFERS_OFFSET);
            id sampler = [[backend->device
                newSamplerStateWithDescriptor:descriptor] autorelease];
            if (!sampler) {
                metal_v5_fail(&failure,
                              INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                              INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                              INFERNO_METAL_RESOURCE_RECORD_SAMPLER, i,
                              @"Metal returned no sampler");
                return metal_v5_batch_finish(c, output, &view, &failure, false);
            }
            [samplers addObject:sampler];
        }

        MTLSize device_max = [backend->device maxThreadsPerThreadgroup];
        NSUInteger device_memory = [backend->device maxThreadgroupMemoryLength];
        for (uint32_t i = 0; i < view.command_count; i++) {
            const uint8_t *command =
                inferno_metal_resource_batch_command(&view, i);
            uint32_t kind =
                ldl_le_p(command + INFERNO_METAL_RESOURCE_COMMAND_KIND_OFFSET);
            if (kind == INFERNO_METAL_RESOURCE_COMMAND_COMPUTE) {
                uint32_t pipeline_index = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_PIPELINE_OFFSET);
                id<MTLComputePipelineState> pipeline =
                    [compute_pipelines objectAtIndex:pipeline_index];
                uint32_t gw = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_WIDTH_OFFSET);
                uint32_t gh = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_HEIGHT_OFFSET);
                uint32_t gd = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_DEPTH_OFFSET);
                MTLSize required = [pipeline requiredThreadsPerThreadgroup];
                if (!metal_product_within(
                        gw, gh, gd, [pipeline maxTotalThreadsPerThreadgroup]) ||
                    gw > device_max.width || gh > device_max.height ||
                    gd > device_max.depth ||
                    (required.width &&
                     (gw != required.width || gh != required.height ||
                      gd != required.depth))) {
                    metal_v5_fail(&failure,
                                  INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH,
                                  INFERNO_METAL_BATCH_PHASE_VALIDATE,
                                  INFERNO_METAL_RESOURCE_RECORD_COMMAND, i,
                                  @"threadgroup dimensions exceed pipeline or "
                                  @"device limits");
                    return metal_v5_batch_finish(c, output, &view, &failure,
                                                 false);
                }
                uint32_t start = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_BINDING_START_OFFSET);
                uint32_t count = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_BINDING_COUNT_OFFSET);
                uint64_t memory = [pipeline staticThreadgroupMemoryLength];
                for (uint32_t j = 0; j < count; j++) {
                    const uint8_t *binding =
                        inferno_metal_resource_batch_binding(&view, start + j);
                    if (ldl_le_p(binding +
                                 INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET) ==
                        INFERNO_METAL_RESOURCE_BINDING_THREADGROUP) {
                        memory += ldl_le_p(
                            binding +
                            INFERNO_METAL_RESOURCE_BINDING_LENGTH_OFFSET);
                    }
                }
                if (memory > device_memory ||
                    !metal_v5_validate_texture_bindings(
                        &view, start, count,
                        [[compute_reflections objectAtIndex:pipeline_index]
                            bindings],
                        textures, &failure,
                        INFERNO_METAL_RESOURCE_RECORD_COMMAND, i)) {
                    if (memory > device_memory) {
                        metal_v5_fail(
                            &failure,
                            INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH,
                            INFERNO_METAL_BATCH_PHASE_VALIDATE,
                            INFERNO_METAL_RESOURCE_RECORD_COMMAND, i,
                            @"threadgroup memory exceeds the device limit");
                    }
                    return metal_v5_batch_finish(c, output, &view, &failure,
                                                 false);
                }
                if (!metal_v5_validate_plain_buffer_bindings(
                        &view, start, count,
                        [[compute_reflections objectAtIndex:pipeline_index]
                            bindings],
                        &failure, INFERNO_METAL_BATCH_RECORD_BINDING, 0) ||
                    !metal_v5_validate_declarations(&view, command, textures,
                                                    &failure)) {
                    return metal_v5_batch_finish(c, output, &view, &failure,
                                                 false);
                }
                uint32_t buffer_total = 0;
                uint32_t texture_total = 0;
                uint32_t sampler_total = 0;
                for (uint32_t j = 0; j < count; j++) {
                    const uint8_t *binding =
                        inferno_metal_resource_batch_binding(&view, start + j);
                    uint32_t binding_kind = ldl_le_p(
                        binding + INFERNO_METAL_RESOURCE_BINDING_KIND_OFFSET);
                    if (binding_kind == INFERNO_METAL_RESOURCE_BINDING_BUFFER) {
                        buffer_total++;
                    } else if (binding_kind ==
                               INFERNO_METAL_RESOURCE_BINDING_TEXTURE) {
                        texture_total++;
                    } else if (binding_kind ==
                               INFERNO_METAL_RESOURCE_BINDING_SAMPLER) {
                        sampler_total++;
                    } else if (binding_kind ==
                               INFERNO_METAL_RESOURCE_BINDING_ARGUMENT) {
                        uint32_t argument_index = ldl_le_p(
                            binding +
                            INFERNO_METAL_RESOURCE_BINDING_RESOURCE_OFFSET);
                        const uint8_t *pipeline_record =
                            inferno_metal_resource_batch_compute_pipeline(
                                &view, pipeline_index);
                        uint32_t library_index = ldl_le_p(
                            pipeline_record +
                            INFERNO_METAL_RESOURCE_COMPUTE_PIPELINE_LIBRARY_OFFSET);
                        if (!metal_v5_validate_argument_binding(
                                backend, &view, binding, argument_index,
                                pipeline_record,
                                inferno_metal_resource_batch_library(
                                    &view, library_index),
                                [libraries objectAtIndex:library_index],
                                [compute_reflections
                                    objectAtIndex:pipeline_index],
                                textures, argument_layouts, &buffer_total,
                                &texture_total, &sampler_total, &failure)) {
                            return metal_v5_batch_finish(c, output, &view,
                                                         &failure, false);
                        }
                    }
                    if (buffer_total > 31 || texture_total > 31 ||
                        sampler_total > 16) {
                        metal_v5_fail(
                            &failure,
                            INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                            INFERNO_METAL_BATCH_PHASE_VALIDATE,
                            INFERNO_METAL_RESOURCE_RECORD_COMMAND, i,
                            @"combined direct and argument resources exceed "
                            @"host limits");
                        return metal_v5_batch_finish(c, output, &view, &failure,
                                                     false);
                    }
                }
            } else {
                uint32_t texture_index = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_TEXTURE_OFFSET);
                id<MTLTexture> attachment =
                    [textures objectAtIndex:texture_index];
                uint32_t start = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_DRAW_START_OFFSET);
                uint32_t count = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_DRAW_COUNT_OFFSET);
                for (uint32_t j = 0; j < count; j++) {
                    uint32_t draw_index = start + j;
                    const uint8_t *draw =
                        inferno_metal_resource_batch_draw(&view, draw_index);
                    uint32_t pipeline_index = ldl_le_p(
                        draw + INFERNO_METAL_RESOURCE_DRAW_PIPELINE_OFFSET);
                    const uint8_t *pipeline_record =
                        inferno_metal_resource_batch_render_pipeline(
                            &view, pipeline_index);
                    if (ldl_le_p(
                            pipeline_record +
                            INFERNO_METAL_RESOURCE_RENDER_PIPELINE_PIXEL_FORMAT_OFFSET) !=
                        [attachment pixelFormat]) {
                        metal_v5_fail(
                            &failure,
                            INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE,
                            INFERNO_METAL_BATCH_PHASE_VALIDATE,
                            INFERNO_METAL_RESOURCE_RECORD_DRAW, draw_index,
                            @"draw pipeline color format differs from "
                            @"attachment");
                        return metal_v5_batch_finish(c, output, &view, &failure,
                                                     false);
                    }
                    MTLRenderPipelineReflection *reflection =
                        [render_reflections objectAtIndex:pipeline_index];
                    uint32_t vertex_start = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_START_OFFSET);
                    uint32_t vertex_count = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_COUNT_OFFSET);
                    uint32_t fragment_start = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_START_OFFSET);
                    uint32_t fragment_count = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_COUNT_OFFSET);
                    if (!metal_v5_validate_texture_bindings(
                            &view, vertex_start, vertex_count,
                            [reflection vertexBindings], textures, &failure,
                            INFERNO_METAL_RESOURCE_RECORD_DRAW, draw_index) ||
                        !metal_v5_validate_texture_bindings(
                            &view, fragment_start, fragment_count,
                            [reflection fragmentBindings], textures, &failure,
                            INFERNO_METAL_RESOURCE_RECORD_DRAW, draw_index)) {
                        return metal_v5_batch_finish(c, output, &view, &failure,
                                                     false);
                    }
                    if (!metal_v5_validate_plain_buffer_bindings(
                            &view, vertex_start, vertex_count,
                            [reflection vertexBindings], &failure,
                            INFERNO_METAL_RESOURCE_RECORD_DRAW, draw_index) ||
                        !metal_v5_validate_plain_buffer_bindings(
                            &view, fragment_start, fragment_count,
                            [reflection fragmentBindings], &failure,
                            INFERNO_METAL_RESOURCE_RECORD_DRAW, draw_index)) {
                        return metal_v5_batch_finish(c, output, &view, &failure,
                                                     false);
                    }
                }
            }
        }

        if (!metal_v5_prepare_arguments(backend, &view, argument_layouts,
                                        buffers, textures, samplers,
                                        argument_backings, &failure)) {
            return metal_v5_batch_finish(c, output, &view, &failure, false);
        }

        MTLCommandBufferDescriptor *descriptor =
            [[[MTLCommandBufferDescriptor alloc] init] autorelease];
        descriptor.errorOptions =
            MTLCommandBufferErrorOptionEncoderExecutionStatus;
        id<MTLCommandBuffer> command_buffer =
            [backend->queue commandBufferWithDescriptor:descriptor];
        if (!command_buffer) {
            if (!view.command_count) {
                return metal_error(message, message_size,
                                   "empty resource batch command buffer", nil);
            }
            metal_v5_fail(&failure,
                          INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                          INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                          INFERNO_METAL_RESOURCE_RECORD_COMMAND, 0,
                          @"Metal returned no command buffer");
            return metal_v5_batch_finish(c, output, &view, &failure, false);
        }
        for (uint32_t i = 0; i < view.command_count; i++) {
            const uint8_t *command =
                inferno_metal_resource_batch_command(&view, i);
            uint32_t kind =
                ldl_le_p(command + INFERNO_METAL_RESOURCE_COMMAND_KIND_OFFSET);
            if (kind == INFERNO_METAL_RESOURCE_COMMAND_COMPUTE) {
                id<MTLComputeCommandEncoder> encoder =
                    [command_buffer computeCommandEncoderWithDispatchType:
                                        MTLDispatchTypeSerial];
                if (!encoder) {
                    metal_v5_fail(
                        &failure,
                        INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                        INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                        INFERNO_METAL_RESOURCE_RECORD_COMMAND, i,
                        @"Metal returned no compute encoder");
                    return metal_v5_batch_finish(c, output, &view, &failure,
                                                 false);
                }
                encoder.label =
                    [NSString stringWithFormat:@"Inferno command %u", i];
                uint32_t pipeline_index = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_PIPELINE_OFFSET);
                [encoder
                    setComputePipelineState:[compute_pipelines
                                                objectAtIndex:pipeline_index]];
                uint32_t start = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_BINDING_START_OFFSET);
                uint32_t count = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_BINDING_COUNT_OFFSET);
                metal_v5_set_compute_bindings(encoder, &view, start, count,
                                              buffers, textures, samplers,
                                              argument_backings);
                metal_v5_use_compute_resources(encoder, &view, command, start,
                                               count, argument_layouts, buffers,
                                               textures);
                MTLSize grid = MTLSizeMake(
                    ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_WIDTH_OFFSET),
                    ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_HEIGHT_OFFSET),
                    ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GRID_DEPTH_OFFSET));
                MTLSize group = MTLSizeMake(
                    ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_WIDTH_OFFSET),
                    ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_HEIGHT_OFFSET),
                    ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_GROUP_DEPTH_OFFSET));
                if (ldl_le_p(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_COMPUTE_MODE_OFFSET) ==
                    INFERNO_METAL_BATCH_DISPATCH_THREADS) {
                    [encoder dispatchThreads:grid threadsPerThreadgroup:group];
                } else {
                    [encoder dispatchThreadgroups:grid
                            threadsPerThreadgroup:group];
                }
                [encoder endEncoding];
            } else {
                uint32_t texture_index = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_TEXTURE_OFFSET);
                MTLRenderPassDescriptor *pass =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture =
                    [textures objectAtIndex:texture_index];
                pass.colorAttachments[0].level = 0;
                pass.colorAttachments[0].slice = 0;
                pass.colorAttachments[0].depthPlane = 0;
                pass.colorAttachments[0].resolveTexture = nil;
                pass.colorAttachments[0].storeActionOptions =
                    MTLStoreActionOptionNone;
                pass.colorAttachments[0].loadAction = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_LOAD_ACTION_OFFSET);
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(
                    metal_v5_double(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_RED_OFFSET),
                    metal_v5_double(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_GREEN_OFFSET),
                    metal_v5_double(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_BLUE_OFFSET),
                    metal_v5_double(
                        command +
                        INFERNO_METAL_RESOURCE_COMMAND_RENDER_CLEAR_ALPHA_OFFSET));
                pass.depthAttachment.texture = nil;
                pass.stencilAttachment.texture = nil;
                pass.visibilityResultBuffer = nil;
                pass.renderTargetArrayLength = 0;
                pass.imageblockSampleLength = 0;
                pass.threadgroupMemoryLength = 0;
                pass.tileWidth = 0;
                pass.tileHeight = 0;
                pass.defaultRasterSampleCount = 0;
                pass.renderTargetWidth = 0;
                pass.renderTargetHeight = 0;
                pass.rasterizationRateMap = nil;
                pass.visibilityResultType = MTLVisibilityResultTypeReset;
                pass.supportColorAttachmentMapping = NO;
                id<MTLRenderCommandEncoder> encoder =
                    [command_buffer renderCommandEncoderWithDescriptor:pass];
                if (!encoder) {
                    metal_v5_fail(
                        &failure,
                        INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED,
                        INFERNO_METAL_RESOURCE_PHASE_RESOURCE,
                        INFERNO_METAL_RESOURCE_RECORD_COMMAND, i,
                        @"Metal returned no render encoder");
                    return metal_v5_batch_finish(c, output, &view, &failure,
                                                 false);
                }
                encoder.label =
                    [NSString stringWithFormat:@"Inferno command %u", i];
                uint32_t start = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_DRAW_START_OFFSET);
                uint32_t count = ldl_le_p(
                    command +
                    INFERNO_METAL_RESOURCE_COMMAND_RENDER_DRAW_COUNT_OFFSET);
                for (uint32_t j = 0; j < count; j++) {
                    const uint8_t *draw =
                        inferno_metal_resource_batch_draw(&view, start + j);
                    uint32_t pipeline_index = ldl_le_p(
                        draw + INFERNO_METAL_RESOURCE_DRAW_PIPELINE_OFFSET);
                    [encoder
                        setRenderPipelineState:
                            [render_pipelines objectAtIndex:pipeline_index]];
                    uint32_t vertex_start = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_START_OFFSET);
                    uint32_t vertex_count = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_VERTEX_BINDING_COUNT_OFFSET);
                    uint32_t fragment_start = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_START_OFFSET);
                    uint32_t fragment_count = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_FRAGMENT_BINDING_COUNT_OFFSET);
                    for (NSUInteger slot = 0; slot <= 30; slot++) {
                        [encoder setVertexBuffer:nil offset:0 atIndex:slot];
                        [encoder setFragmentBuffer:nil offset:0 atIndex:slot];
                        [encoder setVertexTexture:nil atIndex:slot];
                        [encoder setFragmentTexture:nil atIndex:slot];
                    }
                    for (NSUInteger slot = 0; slot <= 15; slot++) {
                        [encoder setVertexSamplerState:nil atIndex:slot];
                        [encoder setFragmentSamplerState:nil atIndex:slot];
                    }
                    metal_v5_set_render_bindings(encoder, true, &view,
                                                 vertex_start, vertex_count,
                                                 buffers, textures, samplers);
                    metal_v5_set_render_bindings(encoder, false, &view,
                                                 fragment_start, fragment_count,
                                                 buffers, textures, samplers);
                    [encoder
                        setCullMode:
                            ldl_le_p(
                                draw +
                                INFERNO_METAL_RESOURCE_DRAW_CULL_MODE_OFFSET)];
                    [encoder
                        setFrontFacingWinding:
                            ldl_le_p(
                                draw +
                                INFERNO_METAL_RESOURCE_DRAW_WINDING_OFFSET)];
                    [encoder
                        setTriangleFillMode:
                            ldl_le_p(
                                draw +
                                INFERNO_METAL_RESOURCE_DRAW_FILL_MODE_OFFSET)];
                    uint32_t flags = ldl_le_p(
                        draw + INFERNO_METAL_RESOURCE_DRAW_FLAGS_OFFSET);
                    id<MTLTexture> attachment =
                        pass.colorAttachments[0].texture;
                    MTLScissorRect scissor = { 0, 0, [attachment width],
                                               [attachment height] };
                    if (flags & INFERNO_METAL_RESOURCE_DRAW_SCISSOR) {
                        scissor = (MTLScissorRect){ ldl_le_p(draw + 56),
                                                    ldl_le_p(draw + 60),
                                                    ldl_le_p(draw + 64),
                                                    ldl_le_p(draw + 68) };
                    }
                    [encoder setScissorRect:scissor];
                    MTLViewport viewport = {
                        0, 0, [attachment width], [attachment height], 0, 1
                    };
                    if (flags & INFERNO_METAL_RESOURCE_DRAW_VIEWPORT) {
                        viewport = (MTLViewport){ metal_v5_double(draw + 72),
                                                  metal_v5_double(draw + 80),
                                                  metal_v5_double(draw + 88),
                                                  metal_v5_double(draw + 96),
                                                  metal_v5_double(draw + 104),
                                                  metal_v5_double(draw + 112) };
                    }
                    [encoder setViewport:viewport];
                    float blend_red = 0;
                    float blend_green = 0;
                    float blend_blue = 0;
                    float blend_alpha = 0;
                    if (flags & INFERNO_METAL_RESOURCE_DRAW_BLEND_COLOR) {
                        blend_red = metal_v5_float(draw + 120);
                        blend_green = metal_v5_float(draw + 124);
                        blend_blue = metal_v5_float(draw + 128);
                        blend_alpha = metal_v5_float(draw + 132);
                    }
                    [encoder setBlendColorRed:blend_red
                                        green:blend_green
                                         blue:blend_blue
                                        alpha:blend_alpha];
                    MTLPrimitiveType primitive = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_PRIMITIVE_TYPE_OFFSET);
                    NSUInteger vertex_start_value = ldl_le_p(
                        draw + INFERNO_METAL_RESOURCE_DRAW_VERTEX_START_OFFSET);
                    NSUInteger vertex_count_value = ldl_le_p(
                        draw + INFERNO_METAL_RESOURCE_DRAW_VERTEX_COUNT_OFFSET);
                    NSUInteger instance_count = ldl_le_p(
                        draw +
                        INFERNO_METAL_RESOURCE_DRAW_INSTANCE_COUNT_OFFSET);
                    if (instance_count == 1) {
                        [encoder drawPrimitives:primitive
                                    vertexStart:vertex_start_value
                                    vertexCount:vertex_count_value];
                    } else {
                        [encoder drawPrimitives:primitive
                                    vertexStart:vertex_start_value
                                    vertexCount:vertex_count_value
                                  instanceCount:instance_count];
                    }
                }
                [encoder endEncoding];
            }
        }

        bool scheduled = false;
        if (!metal_commit_wait(command_buffer, progress, opaque, &scheduled,
                               message, message_size)) {
            return false;
        }
        MTLCommandBufferStatus status = [command_buffer status];
        stl_le_p(output + INFERNO_METAL_BATCH_RESULT_HOST_STATUS_OFFSET,
                 status);
        if (status != MTLCommandBufferStatusCompleted) {
            uint32_t failed_index = INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN;
            NSArray *infos = [[command_buffer error].userInfo
                objectForKey:MTLCommandBufferEncoderInfoErrorKey];
            for (unsigned pass_number = 0;
                 pass_number < 2 &&
                 failed_index == INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN;
                 pass_number++) {
                MTLCommandEncoderErrorState wanted =
                    pass_number ? MTLCommandEncoderErrorStateAffected :
                                  MTLCommandEncoderErrorStateFaulted;
                for (id info in infos) {
                    if ([info errorState] != wanted) {
                        continue;
                    }
                    NSString *label = [info label];
                    if ([label hasPrefix:@"Inferno command "]) {
                        unsigned long value =
                            strtoul([[label substringFromIndex:16] UTF8String],
                                    NULL, 10);
                        if (value < view.command_count) {
                            failed_index = (uint32_t)value;
                        }
                    }
                    break;
                }
            }
            failure.outcome = INFERNO_METAL_BATCH_OUTCOME_EXECUTION_FAILED;
            failure.phase = INFERNO_METAL_BATCH_PHASE_EXECUTE;
            failure.kind =
                failed_index == INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN ?
                    INFERNO_METAL_BATCH_RECORD_UNKNOWN :
                    INFERNO_METAL_RESOURCE_RECORD_COMMAND;
            failure.index = failed_index;
            failure.error = [command_buffer error];
            failure.explanation = @"Metal execution failed without an NSError";
            return metal_v5_batch_finish(c, output, &view, &failure, scheduled);
        }
        if (!scheduled) {
            return metal_error(message, message_size,
                               "resource batch completed without scheduling",
                               nil);
        }
        image_cursor = 0;
        for (uint32_t i = 0; i < view.buffer_count; i++) {
            uint32_t length =
                ldl_le_p(inferno_metal_resource_batch_buffer(&view, i) +
                         INFERNO_METAL_RESOURCE_BUFFER_LENGTH_OFFSET);
            memcpy(output + INFERNO_METAL_BATCH_RESULT_IMAGES_OFFSET +
                       image_cursor,
                   [[buffers objectAtIndex:i] contents], length);
            image_cursor += length;
        }
        for (uint32_t i = 0; i < view.texture_count; i++) {
            const uint8_t *record =
                inferno_metal_resource_batch_texture(&view, i);
            uint32_t width =
                ldl_le_p(record + INFERNO_METAL_RESOURCE_TEXTURE_WIDTH_OFFSET);
            uint32_t height =
                ldl_le_p(record + INFERNO_METAL_RESOURCE_TEXTURE_HEIGHT_OFFSET);
            NSUInteger row = (NSUInteger)width *
                             INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL;
            [[textures objectAtIndex:i]
                   getBytes:output + INFERNO_METAL_BATCH_RESULT_IMAGES_OFFSET +
                            image_cursor
                bytesPerRow:row
                 fromRegion:MTLRegionMake2D(0, 0, width, height)
                mipmapLevel:0];
            image_cursor += row * height;
        }
        failure.outcome = INFERNO_METAL_BATCH_OUTCOME_OK;
        failure.phase = INFERNO_METAL_BATCH_PHASE_EXECUTE;
        failure.kind = INFERNO_METAL_BATCH_RECORD_UNKNOWN;
        failure.index = INFERNO_METAL_BATCH_FAILED_INDEX_UNKNOWN;
        return metal_v5_batch_finish(c, output, &view, &failure, true);
    }
}
