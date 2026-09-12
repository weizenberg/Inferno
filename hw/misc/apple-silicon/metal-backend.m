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
#include "qemu/timer.h"
#include "trace.h"

#import <Metal/Metal.h>

#define METAL_PIPELINE_CACHE_SIZE 8

struct InfernoMetalBackend {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    /* Oldest first. Only the single outstanding worker accesses the cache. */
    NSMutableArray *pipeline_keys;
    NSMutableArray *pipelines;
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
        if (!backend->pipeline_keys || !backend->pipelines) {
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
        [backend->pipelines release];
        [backend->pipeline_keys release];
        [backend->queue release];
        [backend->device release];
        g_free(backend);
    }
}

/* Retain across promotion, even when removal drops the cache's last reference.
 */
static id metal_pipeline_lookup(InfernoMetalBackend *backend, NSArray *key)
{
    NSUInteger index = [backend->pipeline_keys indexOfObject:key];

    if (index == NSNotFound) {
        return nil;
    }
    id pipeline =
        [[[backend->pipelines objectAtIndex:index] retain] autorelease];
    [backend->pipeline_keys removeObjectAtIndex:index];
    [backend->pipelines removeObjectAtIndex:index];
    [backend->pipeline_keys addObject:key];
    [backend->pipelines addObject:pipeline];
    return pipeline;
}

static void metal_pipeline_insert(InfernoMetalBackend *backend, NSArray *key,
                                  id pipeline)
{
    if ([backend->pipeline_keys count] == METAL_PIPELINE_CACHE_SIZE) {
        [backend->pipeline_keys removeObjectAtIndex:0];
        [backend->pipelines removeObjectAtIndex:0];
    }
    [backend->pipeline_keys addObject:key];
    [backend->pipelines addObject:pipeline];
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

    if (!maximum || maximum > UINT32_MAX || !width || width > UINT32_MAX ||
        memory > UINT32_MAX) {
        snprintf(message, message_size,
                 "compute pipeline limits are outside the v2 wire range");
        return false;
    }
    stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_TYPE_OFFSET,
             INFERNO_METAL_FUNCTION_TYPE_KERNEL);
    stl_le_p(output + INFERNO_METAL_COMPILER_MAX_TOTAL_THREADS_OFFSET, maximum);
    stl_le_p(output + INFERNO_METAL_COMPILER_THREAD_EXECUTION_WIDTH_OFFSET,
             width);
    stl_le_p(output + INFERNO_METAL_COMPILER_STATIC_THREADGROUP_MEMORY_OFFSET,
             memory);
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
        if (c->opcode == INFERNO_METAL_QUERY_PIPELINE) {
            function_name = [NSString stringWithUTF8String:c->function];
            if (!function_name) {
                return metal_error(message, message_size, "UTF-8 function name",
                                   nil);
            }
            pipeline_key = @[
                @(INFERNO_METAL_COMPUTE),
                [NSData dataWithBytes:source length:c->source_size],
                function_name, @""
            ];
            pipeline = metal_pipeline_lookup(backend, pipeline_key);
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
                    INFERNO_METAL_COMPILER_PHASE_PIPELINE, nil, nil, false);
            }
        }

        id<MTLLibrary> library =
            [[backend->device newLibraryWithSource:text
                                           options:nil
                                             error:&library_error] autorelease];
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

            if (count > UINT32_MAX) {
                return metal_error(message, message_size,
                                   "function inventory count exceeds uint32",
                                   nil);
            }
            stl_le_p(output + INFERNO_METAL_COMPILER_FUNCTION_COUNT_OFFSET,
                     count);
            uint32_t capacity =
                (c->output_size - INFERNO_METAL_COMPILER_MIN_OUTPUT) /
                INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
            if (count > INFERNO_METAL_COMPILER_MAX_FUNCTIONS) {
                return metal_query_finish(
                    c, output,
                    INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
                    INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                    @"function inventory exceeds the v2 maximum", false);
            }
            if (count > capacity) {
                return metal_query_finish(
                    c, output, INFERNO_METAL_COMPILER_OUTCOME_OUTPUT_TOO_SMALL,
                    INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                    @"output has insufficient function-record capacity", false);
            }

            NSMutableSet<NSString *> *seen =
                [NSMutableSet setWithCapacity:count];
            for (NSUInteger i = 0; i < count; i++) {
                NSString *name = [names objectAtIndex:i];
                NSData *name_data =
                    [name dataUsingEncoding:NSUTF8StringEncoding];
                NSUInteger name_length = [name_data length];
                id<MTLFunction> function =
                    [[library newFunctionWithName:name] autorelease];
                uint32_t type;

                if (!name || !name_data || !name_length ||
                    name_length >= INFERNO_METAL_COMPILER_FUNCTION_NAME_SIZE ||
                    [seen containsObject:name] || !function ||
                    !metal_function_type(&type, [function functionType])) {
                    memset(output + INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET, 0,
                           c->output_size -
                               INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET);
                    return metal_query_finish(
                        c, output,
                        INFERNO_METAL_COMPILER_OUTCOME_INVENTORY_UNSUPPORTED,
                        INFERNO_METAL_COMPILER_PHASE_INVENTORY, nil,
                        @"function inventory contains unsupported metadata",
                        false);
                }
                [seen addObject:name];
                uint8_t *record =
                    output + INFERNO_METAL_COMPILER_FUNCTIONS_OFFSET +
                    i * INFERNO_METAL_COMPILER_FUNCTION_RECORD_SIZE;
                stl_le_p(record + INFERNO_METAL_COMPILER_RECORD_TYPE_OFFSET,
                         type);
                stl_le_p(record +
                             INFERNO_METAL_COMPILER_RECORD_NAME_LENGTH_OFFSET,
                         name_length);
                memcpy(record + INFERNO_METAL_COMPILER_RECORD_NAME_OFFSET,
                       [name_data bytes], name_length);
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
            return metal_error(message, message_size,
                               "compute function requires specialization", nil);
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
        metal_pipeline_insert(backend, pipeline_key, pipeline);
        metal_phase(c, "query-pipeline", &phase_start);
        NSError *diagnostic = pipeline_error ?: library_error;
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
            cached_pipeline = metal_pipeline_lookup(backend, pipeline_key);
            trace_inferno_metal_pipeline_cache(c->sequence, c->opcode,
                                               cached_pipeline != nil,
                                               [backend->pipeline_keys count]);
            if (!cached_pipeline) {
                library =
                    [[device newLibraryWithSource:text options:nil error:&error]
                        autorelease];
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
                metal_pipeline_insert(backend, pipeline_key, pipeline);
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
                    metal_pipeline_insert(backend, pipeline_key, pipeline);
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
