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
#import "InfernoMetalBuffer.h"
#import "InfernoMetalCommandQueue.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalComputePipelineState.h"
#import "InfernoMetalContextPrivate.h"
#import "InfernoMetalErrors.h"
#import "InfernoMetalFunction.h"
#import "InfernoMetalLibrary.h"

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
    id<MTLDevice> owner = self.owner;
    if (!snapshot || !snapshot.length || !owner) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorInvalidArgument,
                @"A valid source and live owner are required");
        return nil;
    }
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator && imtl_coordinator_query_library(
                                 _coordinator, snapshot.bytes, snapshot.length,
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
                                                        source:snapshot
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
- (void)newLibraryWithSource:(NSString *)source
                     options:(MTLCompileOptions *)options
           completionHandler:(MTLNewLibraryCompletionHandler)handler
{
    NSString *sourceCopy = [source copy];
    MTLCompileOptions *optionsCopy = [options copy];
    id<MTLDevice> owner = self.owner;
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
    id<MTLDevice> owner = self.owner;
    if (!owner) {
        if (error)
            *error = InfernoMetalMakeError(
                InfernoMetalErrorClosed,
                @"The device owner is no longer available");
        return nil;
    }
    NSData *source = value.infernoLibrary.infernoSource;
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator &&
             imtl_coordinator_query_pipeline(
                 _coordinator, source.bytes, source.length,
                 value.name.UTF8String, &reply, &coordinatorError);
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
    id<MTLDevice> owner = self.owner;
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
    id<MTLDevice> owner = self.owner;
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
    NSData *source = function.infernoLibrary.infernoSource;
    ImtlQueryReply reply = { 0 };
    ImtlCoordinatorError coordinatorError = { 0 };
    BOOL ok;
    @synchronized(self) {
        ok = _coordinator && imtl_coordinator_query_imageblock(
                                 _coordinator, source.bytes, source.length,
                                 function.name.UTF8String, (uint32_t)d.width,
                                 (uint32_t)d.height, (uint32_t)d.depth, &reply,
                                 &coordinatorError);
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
    (void)data;
    if (error)
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Binary libraries are unsupported");
    return nil;
}
- (id<MTLLibrary>)newDefaultLibrary
{
    return nil;
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle
                                        error:(NSError **)error
{
    (void)bundle;
    if (error)
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Default libraries are unsupported");
    return nil;
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
@end
