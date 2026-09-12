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

#import "InfernoMetalCommandBuffer.h"
#import "InfernoMetalCommandQueue.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalComputeCommandEncoder.h"
#import "InfernoMetalContextPrivate.h"
#import "InfernoMetalErrors.h"

#include <pthread.h>

@interface InfernoMetalEmptyLogContainer : NSObject <MTLLogContainer>
@end
@implementation InfernoMetalEmptyLogContainer
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                  objects:(id __unsafe_unretained[])objects
                                    count:(NSUInteger)count
{
    (void)state;
    (void)objects;
    (void)count;
    return 0;
}
@end

@interface InfernoMetalCommandBuffer () {
    pthread_mutex_t _mutex;
    pthread_cond_t _changed;
    pthread_t _deliveryThread;
    BOOL _mutexReady;
    BOOL _conditionReady;
}
@property(nonatomic, strong) InfernoMetalCommandQueue *storedQueue;
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@property(nonatomic) MTLCommandBufferErrorOption storedErrorOptions;
@property(nonatomic) MTLCommandBufferStatus storedStatus;
@property(nonatomic, strong, nullable) NSError *storedError;
@property(nonatomic, strong) NSMutableArray *dispatches;
@property(nonatomic, strong) NSMutableArray *scheduled;
@property(nonatomic, strong) NSMutableArray *completed;
@property(nonatomic, weak) InfernoMetalComputeCommandEncoder *activeEncoder;
@property(nonatomic, strong) dispatch_queue_t deliveryQueue;
@property(nonatomic) uint64_t commitSerial;
@property(nonatomic) BOOL admitted;
@property(nonatomic) BOOL scheduledDelivered;
@property(nonatomic) BOOL scheduledClosed;
@property(nonatomic) BOOL completedDelivered;
@property(nonatomic) NSUInteger scheduledDeliveriesPending;
@property(nonatomic) NSUInteger completedDeliveriesPending;
@property(nonatomic) BOOL delivering;
@property(nonatomic) BOOL tokenReturned;
@property(nonatomic) BOOL commitStarted;
@property(nonatomic) uint64_t reservationToken;
@end

@implementation InfernoMetalCommandBuffer

- (instancetype)initWithQueue:(InfernoMetalCommandQueue *)queue
                       device:(id<MTLDevice>)device
                      context:(InfernoMetalCompilerContext *)context
                 errorOptions:(MTLCommandBufferErrorOption)options
             reservationToken:(uint64_t)reservationToken
{
    if (!(self = [super init]) || !queue || !device || !context ||
        !reservationToken)
        return nil;
    if (pthread_mutex_init(&_mutex, NULL))
        return nil;
    _mutexReady = YES;
    if (pthread_cond_init(&_changed, NULL))
        return nil;
    _conditionReady = YES;
    _storedQueue = queue;
    _storedDevice = device;
    _storedContext = context;
    _storedErrorOptions = options;
    _reservationToken = reservationToken;
    _storedStatus = MTLCommandBufferStatusNotEnqueued;
    _dispatches = [NSMutableArray array];
    _scheduled = [NSMutableArray array];
    _completed = [NSMutableArray array];
    _deliveryQueue = dispatch_queue_create("org.inferno.metal.delivery",
                                           DISPATCH_QUEUE_SERIAL);
    dispatch_set_target_queue(_deliveryQueue,
                              dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
    return self;
}

- (void)dealloc
{
    BOOL returnToken = NO;
    if (_mutexReady) {
        pthread_mutex_lock(&_mutex);
        returnToken = !_tokenReturned;
        pthread_mutex_unlock(&_mutex);
    }
    if (returnToken)
        [_storedQueue infernoRemove:self];
    if (_conditionReady)
        pthread_cond_destroy(&_changed);
    if (_mutexReady)
        pthread_mutex_destroy(&_mutex);
}

static void invalidUse(NSString *message)
{
    [NSException raise:InfernoMetalInvalidUseException format:@"%@", message];
}

- (id<MTLDevice>)device
{
    return _storedDevice;
}

- (id<MTLCommandQueue>)commandQueue
{
    return _storedQueue;
}

- (BOOL)retainedReferences
{
    return YES;
}

- (MTLCommandBufferErrorOption)errorOptions
{
    return _storedErrorOptions;
}

- (NSString *)label
{
    pthread_mutex_lock(&_mutex);
    NSString *value = _storedLabel;
    pthread_mutex_unlock(&_mutex);
    return value;
}

- (void)setLabel:(NSString *)label
{
    pthread_mutex_lock(&_mutex);
    if (_commitStarted) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"A committed command buffer cannot be relabeled");
        return;
    }
    _storedLabel = [label copy];
    pthread_mutex_unlock(&_mutex);
}

- (MTLCommandBufferStatus)status
{
    pthread_mutex_lock(&_mutex);
    MTLCommandBufferStatus value = _storedStatus;
    pthread_mutex_unlock(&_mutex);
    return value;
}

- (NSError *)error
{
    pthread_mutex_lock(&_mutex);
    NSError *value = _storedError;
    pthread_mutex_unlock(&_mutex);
    return value;
}

- (CFTimeInterval)kernelStartTime
{
    return 0;
}
- (CFTimeInterval)kernelEndTime
{
    return 0;
}
- (CFTimeInterval)GPUStartTime
{
    return 0;
}
- (CFTimeInterval)GPUEndTime
{
    return 0;
}
- (id<MTLLogContainer>)logs
{
    return [[InfernoMetalEmptyLogContainer alloc] init];
}

- (void)enqueue
{
    pthread_mutex_lock(&_mutex);
    if (_commitStarted) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"A committed command buffer cannot be enqueued");
        return;
    }
    if (_storedStatus == MTLCommandBufferStatusEnqueued) {
        pthread_mutex_unlock(&_mutex);
        return;
    }
    if (_storedStatus != MTLCommandBufferStatusNotEnqueued) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"A committed command buffer cannot be enqueued");
        return;
    }
    _storedStatus = MTLCommandBufferStatusEnqueued;
    pthread_mutex_unlock(&_mutex);
    if (![_storedQueue infernoReserve:self]) {
        [self infernoFail:InfernoMetalMakeError(
                              InfernoMetalErrorClosed,
                              @"The command queue has been invalidated")
                scheduled:NO];
        [_storedQueue infernoRemove:self];
    }
}

- (void)commit
{
    pthread_mutex_lock(&_mutex);
    if (_commitStarted) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"A command buffer can be committed only once");
        return;
    }
    if (_activeEncoder) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"End the active encoder before committing");
        return;
    }
    _commitStarted = YES;
    if (_storedStatus == MTLCommandBufferStatusError ||
        _storedStatus == MTLCommandBufferStatusCompleted) {
        NSArray *completed = [_completed copy];
        __attribute__((objc_precise_lifetime)) NSArray *scheduledToDiscard =
            [_scheduled copy];
        [_completed removeAllObjects];
        [_scheduled removeAllObjects];
        _scheduledClosed = YES;
        if (completed.count) {
            _completedDeliveriesPending++;
            _completedDelivered = NO;
        } else if (!_completedDeliveriesPending) {
            _completedDelivered = YES;
        }
        pthread_cond_broadcast(&_changed);
        pthread_mutex_unlock(&_mutex);
        (void)scheduledToDiscard;
        if (completed.count)
            [self deliverHandlers:completed scheduled:NO];
        return;
    }
    BOOL needsReservation = _storedStatus == MTLCommandBufferStatusNotEnqueued;
    if (needsReservation)
        _storedStatus = MTLCommandBufferStatusEnqueued;
    pthread_mutex_unlock(&_mutex);
    if (needsReservation && ![_storedQueue infernoReserve:self]) {
        [self infernoFail:InfernoMetalMakeError(
                              InfernoMetalErrorClosed,
                              @"The command queue has been invalidated")
                scheduled:NO];
        [_storedQueue infernoRemove:self];
        return;
    }
    pthread_mutex_lock(&_mutex);
    if (_storedStatus == MTLCommandBufferStatusError) {
        pthread_mutex_unlock(&_mutex);
        return;
    }
    pthread_mutex_unlock(&_mutex);
    uint64_t serial =
        [_storedContext commitCommandBuffer:self queue:_storedQueue];
    if (!serial) {
        [self infernoFail:InfernoMetalMakeError(
                              InfernoMetalErrorClosed,
                              @"The Metal context cannot accept more work")
                scheduled:NO];
        [_storedQueue infernoRemove:self];
        return;
    }
    [_storedContext scheduleExecutionPump];
}

- (uint64_t)infernoReservationToken
{
    return _reservationToken;
}

- (uint64_t)infernoCommitSerial
{
    pthread_mutex_lock(&_mutex);
    uint64_t value = _commitSerial;
    pthread_mutex_unlock(&_mutex);
    return value;
}

- (BOOL)infernoAdmitted
{
    pthread_mutex_lock(&_mutex);
    BOOL value = _admitted;
    pthread_mutex_unlock(&_mutex);
    return value;
}

- (NSArray *)infernoDispatches
{
    pthread_mutex_lock(&_mutex);
    NSArray *value = [_dispatches copy];
    pthread_mutex_unlock(&_mutex);
    return value;
}

- (InfernoMetalCompilerContext *)infernoContext
{
    return _storedContext;
}

- (void)infernoSetAdmitted
{
    pthread_mutex_lock(&_mutex);
    _admitted = YES;
    pthread_mutex_unlock(&_mutex);
}

- (BOOL)infernoPublishCommitSerial:(uint64_t)serial
{
    pthread_mutex_lock(&_mutex);
    BOOL result =
        _commitStarted && _storedStatus == MTLCommandBufferStatusEnqueued;
    if (result) {
        _commitSerial = serial;
        _storedStatus = MTLCommandBufferStatusCommitted;
    }
    pthread_mutex_unlock(&_mutex);
    return result;
}

- (BOOL)infernoMarkTokenReturned
{
    pthread_mutex_lock(&_mutex);
    BOOL result = !_tokenReturned;
    _tokenReturned = YES;
    pthread_mutex_unlock(&_mutex);
    return result;
}

- (void)deliverHandlers:(NSArray<MTLCommandBufferHandler> *)handlers
              scheduled:(BOOL)isScheduled
{
    dispatch_async(_deliveryQueue, ^{
      pthread_mutex_lock(&self->_mutex);
      self->_delivering = YES;
      self->_deliveryThread = pthread_self();
      pthread_mutex_unlock(&self->_mutex);
      NSException *first = nil;
      @try {
          for (MTLCommandBufferHandler handler in handlers) {
              @try {
                  handler(self);
              } @catch (NSException *exception) {
                  if (!first)
                      first = exception;
              }
          }
      } @finally {
          pthread_mutex_lock(&self->_mutex);
          self->_delivering = NO;
          if (isScheduled) {
              NSCAssert(self->_scheduledDeliveriesPending,
                        @"Scheduled delivery accounting underflow");
              self->_scheduledDeliveriesPending--;
              if (!self->_scheduledDeliveriesPending)
                  self->_scheduledDelivered = YES;
          } else {
              NSCAssert(self->_completedDeliveriesPending,
                        @"Completed delivery accounting underflow");
              self->_completedDeliveriesPending--;
              if (!self->_completedDeliveriesPending && !self->_completed.count)
                  self->_completedDelivered = YES;
          }
          pthread_cond_broadcast(&self->_changed);
          pthread_mutex_unlock(&self->_mutex);
      }
      if (first)
          @throw first;
    });
}

- (void)infernoObserveScheduled
{
    pthread_mutex_lock(&_mutex);
    if (_storedStatus != MTLCommandBufferStatusCommitted) {
        pthread_mutex_unlock(&_mutex);
        return;
    }
    _storedStatus = MTLCommandBufferStatusScheduled;
    NSArray *handlers = [_scheduled copy];
    [_scheduled removeAllObjects];
    if (handlers.count) {
        _scheduledDeliveriesPending++;
        _scheduledDelivered = NO;
    } else {
        _scheduledDelivered = YES;
    }
    pthread_cond_broadcast(&_changed);
    pthread_mutex_unlock(&_mutex);
    if (handlers.count)
        [self deliverHandlers:handlers scheduled:YES];
}

- (void)infernoCompleteWithError:(NSError *)error scheduled:(BOOL)scheduled
{
    if (scheduled)
        [self infernoObserveScheduled];
    pthread_mutex_lock(&_mutex);
    if (_storedStatus == MTLCommandBufferStatusCompleted ||
        _storedStatus == MTLCommandBufferStatusError) {
        pthread_mutex_unlock(&_mutex);
        return;
    }
    BOOL observed = scheduled ||
                    _storedStatus == MTLCommandBufferStatusScheduled ||
                    _scheduledDelivered;
    _storedError = error;
    _storedStatus =
        error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    _scheduledClosed = !observed;
    __attribute__((objc_precise_lifetime)) NSArray *scheduledToDiscard = nil;
    if (_scheduledClosed) {
        scheduledToDiscard = [_scheduled copy];
        [_scheduled removeAllObjects];
    }
    NSArray *handlers = [_completed copy];
    [_completed removeAllObjects];
    if (handlers.count) {
        _completedDeliveriesPending++;
        _completedDelivered = NO;
    } else if (!_completedDeliveriesPending) {
        _completedDelivered = YES;
    }
    _dispatches = nil;
    pthread_cond_broadcast(&_changed);
    pthread_mutex_unlock(&_mutex);
    (void)scheduledToDiscard;
    if (handlers.count)
        [self deliverHandlers:handlers scheduled:NO];
}

- (void)infernoFail:(NSError *)error scheduled:(BOOL)scheduled
{
    [self infernoCompleteWithError:error scheduled:scheduled];
}

- (void)addScheduledHandler:(MTLCommandBufferHandler)handler
{
    if (!handler)
        invalidUse(@"A scheduled handler cannot be nil");
    pthread_mutex_lock(&_mutex);
    if (_commitStarted) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"Handlers must be registered before commit");
        return;
    }
    if (_scheduledClosed) {
        pthread_mutex_unlock(&_mutex);
        return;
    }
    [_scheduled addObject:[handler copy]];
    pthread_mutex_unlock(&_mutex);
}

- (void)addCompletedHandler:(MTLCommandBufferHandler)handler
{
    if (!handler)
        invalidUse(@"A completed handler cannot be nil");
    pthread_mutex_lock(&_mutex);
    if (_commitStarted) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"Handlers must be registered before commit");
        return;
    }
    [_completed addObject:[handler copy]];
    if (_storedStatus == MTLCommandBufferStatusCompleted ||
        _storedStatus == MTLCommandBufferStatusError)
        _completedDelivered = NO;
    pthread_mutex_unlock(&_mutex);
}

- (void)waitForPredicate:(BOOL (^)(void))predicate phase:(NSString *)phase
{
    pthread_mutex_lock(&_mutex);
    if (!_commitStarted) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"Waits require a committed command buffer");
        return;
    }
    if (_delivering && pthread_equal(_deliveryThread, pthread_self())) {
        pthread_mutex_unlock(&_mutex);
        invalidUse([NSString
            stringWithFormat:@"A %@ handler cannot wait on its own buffer",
                             phase]);
        return;
    }
    while (!predicate())
        pthread_cond_wait(&_changed, &_mutex);
    pthread_mutex_unlock(&_mutex);
}

- (void)waitUntilScheduled
{
    [self waitForPredicate:^BOOL {
      return self->_scheduledDelivered ||
             (self->_scheduledClosed && self->_completedDelivered);
    }
                     phase:@"scheduled"];
}

- (void)waitUntilCompleted
{
    [self waitForPredicate:^BOOL {
      return self->_completedDelivered;
    }
                     phase:@"completed"];
}

- (BOOL)infernoAppendDispatch:(id)dispatch
{
    pthread_mutex_lock(&_mutex);
    BOOL valid = !_commitStarted;
    if (valid)
        [_dispatches addObject:dispatch];
    pthread_mutex_unlock(&_mutex);
    return valid;
}

- (void)infernoEncoderEnded:(InfernoMetalComputeCommandEncoder *)encoder
{
    pthread_mutex_lock(&_mutex);
    if (_activeEncoder == encoder)
        _activeEncoder = nil;
    pthread_mutex_unlock(&_mutex);
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
    return [self computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:
    (MTLDispatchType)dispatchType
{
    if (dispatchType != MTLDispatchTypeSerial &&
        dispatchType != MTLDispatchTypeConcurrent) {
        invalidUse(@"The compute dispatch type is invalid");
        return nil;
    }
    pthread_mutex_lock(&_mutex);
    if (_commitStarted || _activeEncoder) {
        pthread_mutex_unlock(&_mutex);
        invalidUse(@"Only one encoder may be active before commit");
        return nil;
    }
    InfernoMetalComputeCommandEncoder *encoder =
        [[InfernoMetalComputeCommandEncoder alloc]
            initWithCommandBuffer:self
                           device:_storedDevice];
    _activeEncoder = encoder;
    pthread_mutex_unlock(&_mutex);
    return encoder;
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:
    (MTLComputePassDescriptor *)descriptor
{
    if (!descriptor)
        return nil;
    for (NSUInteger i = 0; i < 4; i++) {
        if (descriptor.sampleBufferAttachments[i].sampleBuffer)
            return nil;
    }
    return [self computeCommandEncoderWithDispatchType:descriptor.dispatchType];
}

#define IMTL_UNSUPPORTED_VOID(selector, arguments, body)     \
    -(void)selector arguments                                \
    {                                                        \
        body;                                                \
        invalidUse(@"The requested command is unsupported"); \
    }

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Blit commands are unsupported"];
    return nil;
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:
    (MTLBlitPassDescriptor *)descriptor
{
    (void)descriptor;
    return [self blitCommandEncoder];
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
    (MTLRenderPassDescriptor *)descriptor
{
    (void)descriptor;
    [self blitCommandEncoder];
    return nil;
}
- (id<MTLParallelRenderCommandEncoder>)
    parallelRenderCommandEncoderWithDescriptor:
        (MTLRenderPassDescriptor *)descriptor
{
    (void)descriptor;
    [self blitCommandEncoder];
    return nil;
}
- (id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoder
{
    [self blitCommandEncoder];
    return nil;
}
- (id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoderWithDescriptor:
    (MTLResourceStatePassDescriptor *)descriptor
{
    (void)descriptor;
    return [self resourceStateCommandEncoder];
}
- (id<MTLAccelerationStructureCommandEncoder>)
    accelerationStructureCommandEncoder
{
    [self blitCommandEncoder];
    return nil;
}
- (id<MTLAccelerationStructureCommandEncoder>)
    accelerationStructureCommandEncoderWithDescriptor:
        (MTLAccelerationStructurePassDescriptor *)descriptor
{
    (void)descriptor;
    return [self accelerationStructureCommandEncoder];
}

- (void)presentDrawable:(id<MTLDrawable>)drawable
{
    (void)drawable;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Presentation is unsupported"];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time
{
    (void)time;
    [self presentDrawable:drawable];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable
    afterMinimumDuration:(CFTimeInterval)duration
{
    (void)duration;
    [self presentDrawable:drawable];
}
- (void)encodeWaitForEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    (void)event;
    (void)value;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Events are unsupported"];
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
    [self encodeWaitForEvent:event value:value];
}
- (void)pushDebugGroup:(NSString *)string
{
    (void)string;
}
- (void)popDebugGroup
{
}
- (void)useResidencySet:(id<MTLResidencySet>)set
{
    (void)set;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Residency sets are unsupported"];
}
- (void)useResidencySets:(const id<MTLResidencySet>[])sets
                   count:(NSUInteger)count
{
    (void)sets;
    (void)count;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Residency sets are unsupported"];
}

@end
