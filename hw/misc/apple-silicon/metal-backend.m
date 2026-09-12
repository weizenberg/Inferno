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
             * Protocol v1 fixes all other pipeline state (including BGRA8).
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
                library = [[device newLibraryWithSource:text
                                                options:nil
                                                  error:&error] autorelease];
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
                pipeline = [[device newComputePipelineStateWithFunction:function
                                                                  error:&error]
                    autorelease];
                if (!pipeline) {
                    return metal_error(message, message_size,
                                       "compute pipeline", error);
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
