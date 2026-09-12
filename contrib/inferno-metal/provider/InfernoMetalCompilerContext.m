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
#import "InfernoMetalBuffer.h"
#import "InfernoMetalCommandQueue.h"
#import "InfernoMetalComputePipelineState.h"
#import "InfernoMetalContextPrivate.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#import "InfernoMetalLibrary.h"
#import "InfernoMetalRenderPipelineState.h"
#import "InfernoMetalSamplerState.h"
#import "InfernoMetalTexture.h"

@implementation InfernoMetalCompilerContext
- (instancetype)initWithService:(io_service_t)service
                          owner:(id<MTLDevice>)owner
                      timeoutNS:(uint64_t)timeout
                          error:(NSError **)error
{
    if (error)
        *error = nil;
    if (!owner) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                      @"A live Metal device owner is required");
        return nil;
    }
    ImtlUserClient *client = NULL;
    IOReturn io = imtl_user_client_open(service, &client);
    if (io != kIOReturnSuccess) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorTransport,
                                      @"Cannot open the Inferno Metal service");
        return nil;
    }
    ImtlCoordinatorConfig config;
    imtl_coordinator_config_default(&config);
    config.timeout_ns = timeout;
    ImtlCoordinator *coordinator = NULL;
    io = imtl_coordinator_create(client, &config, &coordinator);
    if (io != kIOReturnSuccess) {
        imtl_user_client_close(&client);
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorInvalidArgument,
                @"Cannot create the Inferno Metal coordinator");
        return nil;
    }
    return [self
        initWithCoordinator:coordinator
                      owner:owner
            completionQueue:dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0)];
}
- (instancetype)initWithCoordinator:(ImtlCoordinator *)coordinator
                              owner:(id<MTLDevice>)owner
                    completionQueue:(dispatch_queue_t)queue
{
    self = [super init];
    if (!self || !coordinator || !owner || !queue) {
        if (coordinator)
            imtl_coordinator_close(&coordinator);
        return nil;
    }
    _coordinator = coordinator;
    _owner = owner;
    _completionQueue = queue;
    _executionQueue = dispatch_queue_create("org.inferno.metal.execution",
                                            DISPATCH_QUEUE_SERIAL);
    _schedulerLock = [[NSLock alloc] init];
    _queues = [NSHashTable weakObjectsHashTable];
    _nextCommitSerial = 1;
    if (!_executionQueue || !_schedulerLock || !_queues)
        return nil;
    return self;
}

- (id<MTLDevice>)executionOwner
{
    [_schedulerLock lock];
    id<MTLDevice> owner = _executionClosed ? nil : _owner;
    [_schedulerLock unlock];
    return owner;
}
- (void)dealloc
{
    ImtlCoordinator *value = NULL;
    @synchronized(self) {
        value = _coordinator;
        _coordinator = NULL;
    }
    if (value)
        imtl_coordinator_close(&value);
}
- (void)invalidate
{
    [_schedulerLock lock];
    if (_executionClosed) {
        [_schedulerLock unlock];
        return;
    }
    _executionClosed = YES;
    NSArray *queues = _queues.allObjects;
    [_schedulerLock unlock];
    for (InfernoMetalCommandQueue *queue in queues)
        [queue infernoClose];
    @synchronized(self) {
        if (_coordinator)
            imtl_coordinator_invalidate(_coordinator);
    }
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length
                             options:(MTLResourceOptions)options
{
    id<MTLDevice> owner = [self executionOwner];
    if (!owner)
        return nil;
    return [[InfernoMetalBuffer alloc] initWithContext:self
                                                device:owner
                                                length:length
                                               options:options
                                                 bytes:NULL];
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)pointer
                             length:(NSUInteger)length
                            options:(MTLResourceOptions)options
{
    id<MTLDevice> owner = [self executionOwner];
    if (!owner || !pointer)
        return nil;
    return [[InfernoMetalBuffer alloc] initWithContext:self
                                                device:owner
                                                length:length
                                               options:options
                                                 bytes:pointer];
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))block
{
    (void)pointer;
    (void)length;
    (void)options;
    (void)block;
    return nil;
}

static BOOL supportedPixelFormat(MTLPixelFormat format)
{
    return format == MTLPixelFormatRGBA8Unorm ||
           format == MTLPixelFormatRGBA8Unorm_sRGB ||
           format == MTLPixelFormatRGBA8Snorm ||
           format == MTLPixelFormatBGRA8Unorm ||
           format == MTLPixelFormatBGRA8Unorm_sRGB;
}

static BOOL defaultSwizzle(MTLTextureSwizzleChannels value)
{
    MTLTextureSwizzleChannels expected = MTLTextureSwizzleChannelsDefault;
    return value.red == expected.red && value.green == expected.green &&
           value.blue == expected.blue && value.alpha == expected.alpha;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
{
    id<MTLDevice> owner = [self executionOwner];
    NSUInteger allowed = MTLResourceCPUCacheModeMask |
                         MTLResourceStorageModeMask |
                         MTLResourceHazardTrackingModeMask;
    MTLTextureUsage usage = descriptor.usage;
    if (!owner || !descriptor || descriptor.textureType != MTLTextureType2D ||
        !supportedPixelFormat(descriptor.pixelFormat) || !descriptor.width ||
        !descriptor.height ||
        descriptor.width > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
        descriptor.height > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
        descriptor.depth != 1 || descriptor.mipmapLevelCount != 1 ||
        descriptor.sampleCount != 1 || descriptor.arrayLength != 1 ||
        descriptor.cpuCacheMode != MTLCPUCacheModeDefaultCache ||
        descriptor.storageMode != MTLStorageModeShared ||
        (descriptor.hazardTrackingMode != MTLHazardTrackingModeDefault &&
         descriptor.hazardTrackingMode != MTLHazardTrackingModeTracked) ||
        (descriptor.resourceOptions & ~allowed) || !usage ||
        (usage & ~(MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                   MTLTextureUsageRenderTarget)) ||
        descriptor.compressionType != MTLTextureCompressionTypeLossless ||
        !defaultSwizzle(descriptor.swizzle) ||
        descriptor.placementSparsePageSize)
        return nil;
    return [[InfernoMetalTexture alloc] initWithContext:self
                                                 device:owner
                                             descriptor:descriptor];
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:
    (MTLSamplerDescriptor *)descriptor
{
    id<MTLDevice> owner = [self executionOwner];
    if (!owner || !descriptor)
        return nil;
    return [[InfernoMetalSamplerState alloc] initWithContext:self
                                                      device:owner
                                                  descriptor:descriptor];
}

- (id<MTLCommandQueue>)newCommandQueue
{
    return [self newCommandQueueWithMaxCommandBufferCount:64];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:
    (NSUInteger)count
{
    id<MTLDevice> owner = [self executionOwner];
    if (!owner || !count)
        return nil;
    InfernoMetalCommandQueue *queue =
        [[InfernoMetalCommandQueue alloc] initWithContext:self
                                                   device:owner
                                                 maxCount:count];
    if (queue && ![self registerCommandQueue:queue]) {
        [queue infernoClose];
        queue = nil;
    }
    return queue;
}

- (id<MTLCommandQueue>)newCommandQueueWithDescriptor:
    (MTLCommandQueueDescriptor *)descriptor
{
    if (!descriptor || descriptor.logState || !descriptor.maxCommandBufferCount)
        return nil;
    return [self
        newCommandQueueWithMaxCommandBufferCount:descriptor
                                                     .maxCommandBufferCount];
}

static NSError *resultError(const ImtlCompilerResult *result, IOReturn timer,
                            IOReturn cleanup)
{
    NSError *primary = InfernoMetalErrorFromCompilerResult(result, cleanup);
    if (!primary && timer == kIOReturnSuccess && cleanup == kIOReturnSuccess)
        return nil;
    NSMutableDictionary *info =
        [primary.userInfo mutableCopy] ?: [NSMutableDictionary dictionary];
    if (!primary)
        info[NSLocalizedDescriptionKey] =
            @"Metal query completed with a bridge cleanup diagnostic";
    if (timer != kIOReturnSuccess)
        info[InfernoMetalTimerErrorKey] = @((uint32_t)timer);
    if (cleanup != kIOReturnSuccess)
        info[InfernoMetalCleanupErrorKey] = @((uint32_t)cleanup);
    return [NSError
        errorWithDomain:primary.domain ?: InfernoMetalErrorDomain
                   code:primary ? primary.code : InfernoMetalErrorTransport
               userInfo:info];
}

static NSError *localLibraryError(InfernoMetalErrorCode code,
                                  NSString *description, NSURL *url,
                                  NSError *underlying)
{
    NSMutableDictionary *info =
        [@{ NSLocalizedDescriptionKey : description } mutableCopy];
    if (url)
        info[NSURLErrorKey] = url;
    if (underlying)
        info[NSUnderlyingErrorKey] = underlying;
    return [NSError errorWithDomain:InfernoMetalErrorDomain
                               code:code
                           userInfo:info];
}

- (id<MTLLibrary>)newLibraryWithPayload:(NSData *)payload
                                   kind:(uint32_t)kind
                                  error:(NSError **)error
{
    if (error)
        *error = nil;
    if (kind != INFERNO_METAL_RESOURCE_LIBRARY_SOURCE &&
        kind != INFERNO_METAL_RESOURCE_LIBRARY_METALLIB) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"Library kind is invalid");
        return nil;
    }
    size_t maximum = kind == INFERNO_METAL_RESOURCE_LIBRARY_SOURCE ?
                         INFERNO_METAL_MAX_SOURCE :
                         INFERNO_METAL_RESOURCE_MAX_METALLIB;
    if (!payload.length) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"Library payload is empty");
        return nil;
    }
    if (payload.length > maximum) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorPayloadTooLarge,
                @"Library payload exceeds the transport limit");
        return nil;
    }
    id<MTLDevice> owner = [self executionOwner];
    if (!owner) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorClosed,
                @"The device owner is no longer available");
        return nil;
    }
    NSData *snapshot = [NSData dataWithBytes:payload.bytes
                                      length:payload.length];
    ImtlBatch5Library record = {
        .kind = kind,
        .bytes = snapshot.bytes,
        .size = snapshot.length,
    };
    ImtlTypedQueryManifest manifest = {
        .libraries = &record,
        .library_count = 1,
    };
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator &&
             imtl_coordinator_query_library_typed(_coordinator, &manifest,
                                                  &reply, &coordinatorError);
    }
    if (!ok) {
        if (error)
            *error = InfernoMetalErrorFromCoordinator(&coordinatorError);
        return nil;
    }
    NSError *remote =
        resultError(&reply.result, reply.timer_error, reply.cleanup_io);
    id library = nil;
    if (reply.result.outcome == INFERNO_METAL_COMPILER_OUTCOME_OK)
        library = [[InfernoMetalLibrary alloc] initWithContext:self
                                                        device:owner
                                                       payload:snapshot
                                                          kind:kind
                                                        result:&reply.result
                                                compileWarning:remote
                                                         error:error];
    else if (error)
        *error = remote;
    if (library && error)
        *error = remote;
    imtl_query_reply_free(&reply);
    return library;
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source
                               options:(MTLCompileOptions *)options
                                 error:(NSError **)error
{
    if (error)
        *error = nil;
    if (options) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                           @"Compile options are unsupported");
        return nil;
    }
    NSData *snapshot = [source dataUsingEncoding:NSUTF8StringEncoding];
    if (!snapshot || !snapshot.length) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                      @"A nonempty UTF-8 source is required");
        return nil;
    }
    return [self newLibraryWithPayload:snapshot
                                  kind:INFERNO_METAL_RESOURCE_LIBRARY_SOURCE
                                 error:error];
}
- (void)newLibraryWithSource:(NSString *)source
                     options:(MTLCompileOptions *)options
           completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    NSString *sourceCopy = [source copy];
    MTLCompileOptions *optionsCopy = [options copy];
    id<MTLDevice> owner = [self executionOwner];
    dispatch_async(_completionQueue, ^{
      NSError *error = nil;
      id value = owner ? [self newLibraryWithSource:sourceCopy
                                            options:optionsCopy
                                              error:&error] :
                         nil;
      if (!owner)
          error =
              InfernoMetalMakeError(InfernoMetalErrorClosed,
                                    @"The device owner is no longer available");
      handler(value, error);
    });
}

- (id<MTLComputePipelineState>)
    newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                  error:(NSError **)error
{
    return [self newComputePipelineStateWithFunction:function
                                             options:MTLPipelineOptionNone
                                          reflection:NULL
                                               error:error];
}
- (id<MTLComputePipelineState>)
    newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                options:(MTLPipelineOption)options
                             reflection:
                                 (MTLComputePipelineReflection **)reflection
                                  error:(NSError **)error
{
    if (error)
        *error = nil;
    if (reflection)
        *reflection = nil;
    if (options) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                           @"Pipeline options are unsupported");
        return nil;
    }
    if (![function isKindOfClass:[InfernoMetalFunction class]]) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                      @"Function belongs to another provider");
        return nil;
    }
    InfernoMetalFunction *value = (id)function;
    if (value.infernoLibrary.infernoContext != self) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                      @"Function belongs to another context");
        return nil;
    }
    if (value.functionType != MTLFunctionTypeKernel) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorFunctionTypeMismatch,
                                      @"Function is not a compute kernel");
        return nil;
    }
    if (value.functionConstantsDictionary.count) {
        if (error)
            *error =
                InfernoMetalMakeError(InfernoMetalErrorSpecializationRequired,
                                      @"Function requires specialization");
        return nil;
    }
    if (!value.name.length ||
        [value.name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 63) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorInvalidArgument,
                @"Function name is outside the transport limit");
        return nil;
    }
    id<MTLDevice> owner = [self executionOwner];
    if (!owner) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorClosed,
                @"The device owner is no longer available");
        return nil;
    }
    InfernoMetalLibrary *library = value.infernoLibrary;
    NSData *payload = library.infernoPayload;
    ImtlBatch5Library libraryRecord = {
        .kind = library.infernoLibraryKind,
        .bytes = payload.bytes,
        .size = payload.length,
    };
    ImtlBatch5ComputePipeline pipelineRecord = {
        .library_id = 0,
        .function_name = value.name.UTF8String,
    };
    ImtlTypedQueryManifest manifest = {
        .libraries = &libraryRecord,
        .library_count = 1,
        .compute_pipeline = &pipelineRecord,
    };
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator &&
             imtl_coordinator_query_pipeline_typed(_coordinator, &manifest,
                                                   &reply, &coordinatorError);
    }
    if (!ok) {
        if (error)
            *error = InfernoMetalErrorFromCoordinator(&coordinatorError);
        return nil;
    }
    NSError *remote =
        resultError(&reply.result, reply.timer_error, reply.cleanup_io);
    id pipeline = nil;
    if (reply.result.outcome == INFERNO_METAL_COMPILER_OUTCOME_OK)
        pipeline = [[InfernoMetalComputePipelineState alloc]
            initWithContext:self
                     device:owner
                   function:value
                     result:&reply.result];
    if (error)
        *error = remote;
    imtl_query_reply_free(&reply);
    return pipeline;
}
- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                          completionHandler:
                              (MTLNewComputePipelineStateCompletionHandler)
                                  handler
{
    id<MTLDevice> owner = [self executionOwner];
    dispatch_async(_completionQueue, ^{
      NSError *error = nil;
      id value = owner ? [self newComputePipelineStateWithFunction:function
                                                             error:&error] :
                         nil;
      if (!owner)
          error =
              InfernoMetalMakeError(InfernoMetalErrorClosed,
                                    @"The device owner is no longer available");
      handler(value, error);
    });
}
- (void)
    newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                options:(MTLPipelineOption)options
                      completionHandler:
                          (MTLNewComputePipelineStateWithReflectionCompletionHandler)
                              handler
{
    id<MTLDevice> owner = [self executionOwner];
    dispatch_async(_completionQueue, ^{
      NSError *error = nil;
      MTLComputePipelineReflection *reflection = nil;
      id value = owner ? [self newComputePipelineStateWithFunction:function
                                                           options:options
                                                        reflection:&reflection
                                                             error:&error] :
                         nil;
      if (!owner)
          error =
              InfernoMetalMakeError(InfernoMetalErrorClosed,
                                    @"The device owner is no longer available");
      handler(value, reflection, error);
    });
}
- (BOOL)queryImageblockForFunction:(InfernoMetalFunction *)function
                        dimensions:(MTLSize)d
                            length:(NSUInteger *)length
                             error:(NSError **)error
{
    if (error)
        *error = nil;
    if (!length || !d.width || d.width > 65536 || !d.height ||
        d.height > 65536 || !d.depth || d.depth > 65536) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"Invalid imageblock dimensions");
        return NO;
    }
    InfernoMetalLibrary *library = function.infernoLibrary;
    NSData *payload = library.infernoPayload;
    ImtlBatch5Library libraryRecord = {
        .kind = library.infernoLibraryKind,
        .bytes = payload.bytes,
        .size = payload.length,
    };
    ImtlBatch5ComputePipeline pipelineRecord = {
        .library_id = 0,
        .function_name = function.name.UTF8String,
    };
    ImtlTypedQueryManifest manifest = {
        .libraries = &libraryRecord,
        .library_count = 1,
        .compute_pipeline = &pipelineRecord,
    };
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator &&
             imtl_coordinator_query_imageblock_typed(
                 _coordinator, &manifest, (uint32_t)d.width, (uint32_t)d.height,
                 (uint32_t)d.depth, &reply, &coordinatorError);
    }
    if (!ok) {
        if (error)
            *error = InfernoMetalErrorFromCoordinator(&coordinatorError);
        return NO;
    }
    NSError *remote =
        resultError(&reply.result, reply.timer_error, reply.cleanup_io);
    BOOL success = reply.result.outcome == INFERNO_METAL_COMPILER_OUTCOME_OK;
    if (success)
        *length = (NSUInteger)reply.result.imageblock_memory_length;
    if (error)
        *error = remote;
    imtl_query_reply_free(&reply);
    return success;
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data
                               error:(NSError **)error
{
    if (error)
        *error = nil;
    if (!data) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"Library data is required");
        return nil;
    }
    size_t declaredSize = dispatch_data_get_size(data);
    if (!declaredSize) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"Library data is empty");
        return nil;
    }
    if (declaredSize > INFERNO_METAL_RESOURCE_MAX_METALLIB) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorPayloadTooLarge,
                @"Compiled library exceeds the transport limit");
        return nil;
    }
    const void *bytes = NULL;
    size_t size = 0;
    __attribute__((objc_precise_lifetime)) dispatch_data_t mapping =
        dispatch_data_create_map(data, &bytes, &size);
    if (!mapping || !bytes || size != declaredSize) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorTransport,
                                           @"Cannot map library data");
        return nil;
    }
    NSData *payload = [NSData dataWithBytes:bytes length:size];
    return [self newLibraryWithPayload:payload
                                  kind:INFERNO_METAL_RESOURCE_LIBRARY_METALLIB
                                 error:error];
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if (error)
        *error = nil;
    if (!url.isFileURL) {
        if (error)
            *error = localLibraryError(
                InfernoMetalErrorInvalidArgument,
                @"A compiled library URL must be a file URL", url, nil);
        return nil;
    }
    NSError *readError = nil;
    NSData *read = [NSData dataWithContentsOfURL:url
                                         options:NSDataReadingMappedIfSafe
                                           error:&readError];
    if (!read) {
        if (error)
            *error = localLibraryError(InfernoMetalErrorLocalRead,
                                       @"Cannot read the compiled library", url,
                                       readError);
        return nil;
    }
    if (!read.length) {
        if (error)
            *error = localLibraryError(InfernoMetalErrorInvalidArgument,
                                       @"Compiled library is empty", url, nil);
        return nil;
    }
    if (read.length > INFERNO_METAL_RESOURCE_MAX_METALLIB) {
        if (error)
            *error = localLibraryError(
                InfernoMetalErrorPayloadTooLarge,
                @"Compiled library exceeds the transport limit", url, nil);
        return nil;
    }
    NSData *payload = [NSData dataWithBytes:read.bytes length:read.length];
    return [self newLibraryWithPayload:payload
                                  kind:INFERNO_METAL_RESOURCE_LIBRARY_METALLIB
                                 error:error];
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    if (!path.length) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"A library path is required");
        return nil;
    }
    return [self newLibraryWithURL:[NSURL fileURLWithPath:path] error:error];
}
- (id<MTLLibrary>)newDefaultLibrary
{
    return [self newDefaultLibraryWithBundle:NSBundle.mainBundle error:nil];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle
                                        error:(NSError **)error
{
    if (error)
        *error = nil;
    if (!bundle) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorInvalidArgument,
                                           @"A bundle is required");
        return nil;
    }
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    if (!url) {
        if (error) {
            NSString *description = [NSString
                stringWithFormat:@"Bundle has no default.metallib: %@",
                                 bundle.bundlePath];
            *error = [NSError
                errorWithDomain:InfernoMetalErrorDomain
                           code:InfernoMetalErrorLibraryNotFound
                       userInfo:@{
                           NSLocalizedDescriptionKey : description,
                           NSFilePathErrorKey : bundle.bundlePath ?: @""
                       }];
        }
        return nil;
    }
    return [self newLibraryWithURL:url error:error];
}
- (id<MTLComputePipelineState>)
    newComputePipelineStateWithDescriptor:
        (MTLComputePipelineDescriptor *)descriptor
                                  options:(MTLPipelineOption)options
                               reflection:
                                   (MTLComputePipelineReflection **)reflection
                                    error:(NSError **)error
{
    (void)descriptor;
    (void)options;
    if (reflection)
        *reflection = nil;
    if (error)
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Pipeline descriptors are unsupported");
    return nil;
}

static BOOL linkedFunctionsEmpty(MTLLinkedFunctions *functions)
{
    return !functions.functions.count && !functions.binaryFunctions.count &&
           !functions.privateFunctions.count && !functions.groups.count;
}

static BOOL
defaultColorAttachment(MTLRenderPipelineColorAttachmentDescriptor *color)
{
    return color.pixelFormat == MTLPixelFormatInvalid &&
           !color.blendingEnabled &&
           color.sourceRGBBlendFactor == MTLBlendFactorOne &&
           color.destinationRGBBlendFactor == MTLBlendFactorZero &&
           color.rgbBlendOperation == MTLBlendOperationAdd &&
           color.sourceAlphaBlendFactor == MTLBlendFactorOne &&
           color.destinationAlphaBlendFactor == MTLBlendFactorZero &&
           color.alphaBlendOperation == MTLBlendOperationAdd &&
           color.writeMask == MTLColorWriteMaskAll;
}

static BOOL defaultVertexDescriptor(MTLVertexDescriptor *descriptor)
{
    if (!descriptor)
        return YES;
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexAttributeDescriptor *attribute = descriptor.attributes[i];
        MTLVertexBufferLayoutDescriptor *layout = descriptor.layouts[i];
        if (attribute.format != MTLVertexFormatInvalid || attribute.offset ||
            attribute.bufferIndex || layout.stride ||
            layout.stepFunction != MTLVertexStepFunctionPerVertex ||
            layout.stepRate != 1)
            return NO;
    }
    return YES;
}

- (BOOL)validateRenderPipelineDescriptor:(MTLRenderPipelineDescriptor *)d
                                   error:(NSError **)error
{
    InfernoMetalFunction *vertex = nil;
    InfernoMetalFunction *fragment = nil;
    MTLRenderPipelineColorAttachmentDescriptor *color = nil;
    if (![d.vertexFunction isKindOfClass:[InfernoMetalFunction class]] ||
        ![d.fragmentFunction isKindOfClass:[InfernoMetalFunction class]])
        goto invalid;
    vertex = (id)d.vertexFunction;
    fragment = (id)d.fragmentFunction;
    if (vertex.infernoLibrary.infernoContext != self ||
        fragment.infernoLibrary.infernoContext != self ||
        vertex.functionType != MTLFunctionTypeVertex ||
        fragment.functionType != MTLFunctionTypeFragment ||
        vertex.functionConstantsDictionary.count ||
        fragment.functionConstantsDictionary.count || !vertex.name.length ||
        !fragment.name.length ||
        [vertex.name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 63 ||
        [fragment.name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 63 ||
        !defaultVertexDescriptor(d.vertexDescriptor) ||
        d.rasterSampleCount != 1 || d.alphaToCoverageEnabled ||
        d.alphaToOneEnabled || !d.rasterizationEnabled ||
        d.maxVertexAmplificationCount != 1 ||
        d.depthAttachmentPixelFormat != MTLPixelFormatInvalid ||
        d.stencilAttachmentPixelFormat != MTLPixelFormatInvalid ||
        d.inputPrimitiveTopology != MTLPrimitiveTopologyClassUnspecified ||
        d.tessellationPartitionMode != MTLTessellationPartitionModePow2 ||
        d.maxTessellationFactor != 16 || d.tessellationFactorScaleEnabled ||
        d.tessellationFactorFormat != MTLTessellationFactorFormatHalf ||
        d.tessellationControlPointIndexType !=
            MTLTessellationControlPointIndexTypeNone ||
        d.tessellationFactorStepFunction !=
            MTLTessellationFactorStepFunctionConstant ||
        d.tessellationOutputWindingOrder != MTLWindingClockwise ||
        d.supportIndirectCommandBuffers || d.binaryArchives.count ||
        d.vertexPreloadedLibraries.count ||
        d.fragmentPreloadedLibraries.count ||
        !linkedFunctionsEmpty(d.vertexLinkedFunctions) ||
        !linkedFunctionsEmpty(d.fragmentLinkedFunctions) ||
        d.supportAddingVertexBinaryFunctions ||
        d.supportAddingFragmentBinaryFunctions ||
        d.maxVertexCallStackDepth != 1 || d.maxFragmentCallStackDepth != 1 ||
        d.shaderValidation != MTLShaderValidationDefault)
        goto unsupported;
    for (NSUInteger i = 0; i < 31; i++) {
        if (d.vertexBuffers[i].mutability != MTLMutabilityDefault ||
            d.fragmentBuffers[i].mutability != MTLMutabilityDefault)
            goto unsupported;
    }
    for (NSUInteger i = 1; i < 8; i++) {
        if (!defaultColorAttachment(d.colorAttachments[i]))
            goto unsupported;
    }
    color = d.colorAttachments[0];
    if (!supportedPixelFormat(color.pixelFormat) ||
        color.sourceRGBBlendFactor > MTLBlendFactorOneMinusSource1Alpha ||
        color.destinationRGBBlendFactor > MTLBlendFactorOneMinusSource1Alpha ||
        color.rgbBlendOperation > MTLBlendOperationMax ||
        color.sourceAlphaBlendFactor > MTLBlendFactorOneMinusSource1Alpha ||
        color.destinationAlphaBlendFactor >
            MTLBlendFactorOneMinusSource1Alpha ||
        color.alphaBlendOperation > MTLBlendOperationMax ||
        color.writeMask > MTLColorWriteMaskAll)
        goto invalid;
    return YES;
unsupported:
    if (error)
        *error = InfernoMetalMakeError(
            InfernoMetalErrorUnsupportedState,
            @"The render pipeline descriptor uses unsupported state");
    return NO;
invalid:
    if (error)
        *error = InfernoMetalMakeError(
            InfernoMetalErrorInvalidArgument,
            @"The render pipeline descriptor is invalid or foreign");
    return NO;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:
                                  (MTLRenderPipelineDescriptor *)descriptor
                                                             error:(NSError **)
                                                                       error
{
    return [self newRenderPipelineStateWithDescriptor:descriptor
                                              options:MTLPipelineOptionNone
                                           reflection:NULL
                                                error:error];
}

- (id<MTLRenderPipelineState>)
    newRenderPipelineStateWithDescriptor:
        (MTLRenderPipelineDescriptor *)descriptor
                                 options:(MTLPipelineOption)options
                              reflection:
                                  (MTLRenderPipelineReflection **)reflection
                                   error:(NSError **)error
{
    if (error)
        *error = nil;
    if (reflection)
        *reflection = nil;
    if (options) {
        if (error)
            *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                           @"Pipeline options are unsupported");
        return nil;
    }
    if (!descriptor || ![self validateRenderPipelineDescriptor:descriptor
                                                         error:error])
        return nil;
    id<MTLDevice> owner = [self executionOwner];
    if (!owner) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorClosed,
                @"The device owner is no longer available");
        return nil;
    }
    InfernoMetalFunction *vertex = (id)descriptor.vertexFunction;
    InfernoMetalFunction *fragment = (id)descriptor.fragmentFunction;
    InfernoMetalLibrary *vertexLibrary = vertex.infernoLibrary;
    InfernoMetalLibrary *fragmentLibrary = fragment.infernoLibrary;
    BOOL sameLibrary =
        vertexLibrary.infernoLibraryKind ==
            fragmentLibrary.infernoLibraryKind &&
        [vertexLibrary.infernoPayload isEqual:fragmentLibrary.infernoPayload];
    ImtlBatch5Library libraries[2] = {
        { .kind = vertexLibrary.infernoLibraryKind,
          .bytes = vertexLibrary.infernoPayload.bytes,
          .size = vertexLibrary.infernoPayload.length },
        { .kind = fragmentLibrary.infernoLibraryKind,
          .bytes = fragmentLibrary.infernoPayload.bytes,
          .size = fragmentLibrary.infernoPayload.length },
    };
    MTLRenderPipelineColorAttachmentDescriptor *color =
        descriptor.colorAttachments[0];
    ImtlBatch5RenderPipeline pipeline = {
        .vertex_library_id = 0,
        .fragment_library_id = sameLibrary ? 0 : 1,
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
    ImtlTypedQueryManifest manifest = {
        .libraries = libraries,
        .library_count = sameLibrary ? 1 : 2,
        .render_pipeline = &pipeline,
    };
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator &&
             imtl_coordinator_query_render_pipeline(_coordinator, &manifest,
                                                    &reply, &coordinatorError);
    }
    if (!ok) {
        if (error)
            *error = InfernoMetalErrorFromCoordinator(&coordinatorError);
        return nil;
    }
    NSError *remote =
        resultError(&reply.result, reply.timer_error, reply.cleanup_io);
    id pipelineState = nil;
    if (reply.result.outcome == INFERNO_METAL_COMPILER_OUTCOME_OK)
        pipelineState = [[InfernoMetalRenderPipelineState alloc]
            initWithContext:self
                     device:owner
                 descriptor:descriptor
                     result:&reply.result];
    if (error)
        *error = remote;
    imtl_query_reply_free(&reply);
    return pipelineState;
}

- (void)newRenderPipelineStateWithDescriptor:
            (MTLRenderPipelineDescriptor *)descriptor
                           completionHandler:
                               (MTLNewRenderPipelineStateCompletionHandler)
                                   handler
{
    MTLRenderPipelineDescriptor *snapshot = [descriptor copy];
    id<MTLDevice> owner = [self executionOwner];
    dispatch_async(_completionQueue, ^{
      NSError *error = nil;
      id value = owner ? [self newRenderPipelineStateWithDescriptor:snapshot
                                                              error:&error] :
                         nil;
      if (!owner)
          error =
              InfernoMetalMakeError(InfernoMetalErrorClosed,
                                    @"The device owner is no longer available");
      handler(value, error);
    });
}

- (void)
    newRenderPipelineStateWithDescriptor:
        (MTLRenderPipelineDescriptor *)descriptor
                                 options:(MTLPipelineOption)options
                       completionHandler:
                           (MTLNewRenderPipelineStateWithReflectionCompletionHandler)
                               handler
{
    MTLRenderPipelineDescriptor *snapshot = [descriptor copy];
    id<MTLDevice> owner = [self executionOwner];
    dispatch_async(_completionQueue, ^{
      NSError *error = nil;
      MTLRenderPipelineReflection *reflection = nil;
      id value = owner ? [self newRenderPipelineStateWithDescriptor:snapshot
                                                            options:options
                                                         reflection:&reflection
                                                              error:&error] :
                         nil;
      if (!owner)
          error =
              InfernoMetalMakeError(InfernoMetalErrorClosed,
                                    @"The device owner is no longer available");
      handler(value, reflection, error);
    });
}
@end
