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
#import "InfernoMetalContextPrivate.h"
#import "InfernoMetalErrors.h"

#include <pthread.h>

@interface InfernoMetalReservation : NSObject
@property(nonatomic, weak) InfernoMetalCommandBuffer *buffer;
@property(nonatomic, strong, nullable) InfernoMetalCommandBuffer *committed;
@property(nonatomic) uint64_t token;
@end
@implementation InfernoMetalReservation
@end

@interface InfernoMetalCommandQueue () {
    pthread_mutex_t _mutex;
    pthread_cond_t _capacityChanged;
    BOOL _mutexReady;
    BOOL _conditionReady;
}
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@property(nonatomic, strong) NSMutableArray<InfernoMetalReservation *> *items;
@property(nonatomic, strong) NSMutableSet<NSNumber *> *allocatedTokens;
@property(nonatomic) NSUInteger maxCount;
@property(nonatomic) NSUInteger allocatedCount;
@property(nonatomic) uint64_t nextToken;
@property(nonatomic) BOOL closed;
@end

@implementation InfernoMetalCommandQueue

- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                       maxCount:(NSUInteger)maxCount
{
    if (!(self = [super init]) || !context || !device || !maxCount ||
        maxCount > UINT32_MAX) {
        return nil;
    }
    if (pthread_mutex_init(&_mutex, NULL))
        return nil;
    _mutexReady = YES;
    if (pthread_cond_init(&_capacityChanged, NULL))
        return nil;
    _conditionReady = YES;
    _storedContext = context;
    _storedDevice = device;
    _items = [NSMutableArray array];
    _allocatedTokens = [NSMutableSet set];
    _maxCount = maxCount;
    _nextToken = 1;
    return self;
}

- (void)dealloc
{
    if (_conditionReady)
        pthread_cond_destroy(&_capacityChanged);
    if (_mutexReady)
        pthread_mutex_destroy(&_mutex);
}

- (void)returnTokenLocked:(uint64_t)token
{
    NSNumber *key = @(token);
    if (![_allocatedTokens containsObject:key])
        return;
    [_allocatedTokens removeObject:key];
    if (_allocatedCount)
        _allocatedCount--;
    pthread_cond_broadcast(&_capacityChanged);
}

- (void)returnToken:(uint64_t)token
{
    BOOL rescan = NO;
    InfernoMetalCompilerContext *context = nil;
    pthread_mutex_lock(&_mutex);
    for (NSInteger i = (NSInteger)_items.count - 1; i >= 0; i--) {
        InfernoMetalReservation *item = _items[(NSUInteger)i];
        if (item.token == token) {
            rescan = i == 0 && !item.committed && !_closed;
            if (rescan)
                context = _storedContext;
            [_items removeObjectAtIndex:(NSUInteger)i];
            break;
        }
    }
    [self returnTokenLocked:token];
    pthread_mutex_unlock(&_mutex);
    if (rescan)
        [context scheduleExecutionPump];
}

- (InfernoMetalCompilerContext *)infernoContext
{
    return _storedContext;
}

- (id<MTLDevice>)device
{
    return _storedDevice;
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
    _storedLabel = [label copy];
    pthread_mutex_unlock(&_mutex);
}

- (id<MTLCommandBuffer>)commandBuffer
{
    MTLCommandBufferDescriptor *descriptor =
        [[MTLCommandBufferDescriptor alloc] init];
    return [self commandBufferWithDescriptor:descriptor];
}

- (id<MTLCommandBuffer>)commandBufferWithDescriptor:
    (MTLCommandBufferDescriptor *)descriptor
{
    if (!descriptor || !descriptor.retainedReferences || descriptor.logState ||
        (descriptor.errorOptions &
         ~MTLCommandBufferErrorOptionEncoderExecutionStatus)) {
        return nil;
    }
    pthread_mutex_lock(&_mutex);
    while (!_closed && _allocatedCount == _maxCount)
        pthread_cond_wait(&_capacityChanged, &_mutex);
    if (_closed) {
        pthread_mutex_unlock(&_mutex);
        return nil;
    }
    if (_nextToken == UINT64_MAX) {
        pthread_mutex_unlock(&_mutex);
        return nil;
    }
    uint64_t token = _nextToken++;
    _allocatedCount++;
    [_allocatedTokens addObject:@(token)];
    pthread_mutex_unlock(&_mutex);
    InfernoMetalCommandBuffer *buffer =
        [[InfernoMetalCommandBuffer alloc] initWithQueue:self
                                                  device:_storedDevice
                                                 context:_storedContext
                                            errorOptions:descriptor.errorOptions
                                        reservationToken:token];
    if (!buffer)
        [self returnToken:token];
    return buffer;
}

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences
{
    return nil;
}

- (void)insertDebugCaptureBoundary
{
}

- (void)addResidencySet:(id<MTLResidencySet>)set
{
    (void)set;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Residency sets are unsupported"];
}

- (void)addResidencySets:(const id<MTLResidencySet>[])sets
                   count:(NSUInteger)count
{
    (void)sets;
    (void)count;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Residency sets are unsupported"];
}

- (void)removeResidencySet:(id<MTLResidencySet>)set
{
    [self addResidencySet:set];
}

- (void)removeResidencySets:(const id<MTLResidencySet>[])sets
                      count:(NSUInteger)count
{
    (void)sets;
    (void)count;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Residency sets are unsupported"];
}

- (BOOL)infernoReserve:(InfernoMetalCommandBuffer *)buffer
{
    pthread_mutex_lock(&_mutex);
    uint64_t token = buffer.infernoReservationToken;
    if (_closed || ![_allocatedTokens containsObject:@(token)]) {
        pthread_mutex_unlock(&_mutex);
        return NO;
    }
    for (InfernoMetalReservation *item in _items) {
        if (item.token == token) {
            pthread_mutex_unlock(&_mutex);
            return YES;
        }
    }
    InfernoMetalReservation *item = [[InfernoMetalReservation alloc] init];
    item.buffer = buffer;
    item.token = token;
    [_items addObject:item];
    pthread_mutex_unlock(&_mutex);
    return YES;
}

- (BOOL)infernoCommitReservation:(InfernoMetalCommandBuffer *)buffer
{
    pthread_mutex_lock(&_mutex);
    uint64_t token = buffer.infernoReservationToken;
    if (_closed || ![_allocatedTokens containsObject:@(token)]) {
        pthread_mutex_unlock(&_mutex);
        return NO;
    }
    for (InfernoMetalReservation *item in _items) {
        if (item.token == token && item.buffer == buffer) {
            item.committed = buffer;
            pthread_mutex_unlock(&_mutex);
            return YES;
        }
    }
    pthread_mutex_unlock(&_mutex);
    return NO;
}

- (InfernoMetalCommandBuffer *)infernoReadyHead
{
    pthread_mutex_lock(&_mutex);
    while (_items.count && !_items[0].buffer) {
        uint64_t token = _items[0].token;
        [_items removeObjectAtIndex:0];
        [self returnTokenLocked:token];
    }
    InfernoMetalCommandBuffer *result = _items.firstObject.committed;
    pthread_mutex_unlock(&_mutex);
    return result;
}

- (BOOL)infernoAdmitHead:(InfernoMetalCommandBuffer *)buffer
{
    pthread_mutex_lock(&_mutex);
    BOOL result = _items.count && _items[0].committed == buffer;
    pthread_mutex_unlock(&_mutex);
    return result;
}

- (void)infernoRemove:(InfernoMetalCommandBuffer *)buffer
{
    if (![buffer infernoMarkTokenReturned])
        return;
    [self returnToken:buffer.infernoReservationToken];
}

- (void)infernoClose
{
    pthread_mutex_lock(&_mutex);
    if (_closed) {
        pthread_mutex_unlock(&_mutex);
        return;
    }
    _closed = YES;
    NSMutableArray *committed = [NSMutableArray array];
    for (NSInteger i = (NSInteger)_items.count - 1; i >= 0; i--) {
        InfernoMetalReservation *item = _items[(NSUInteger)i];
        InfernoMetalCommandBuffer *buffer = item.committed ?: item.buffer;
        if (!item.committed.infernoAdmitted) {
            if (item.committed)
                [committed addObject:item.committed];
            [_items removeObjectAtIndex:(NSUInteger)i];
            if (buffer)
                [buffer infernoMarkTokenReturned];
            [self returnTokenLocked:item.token];
        }
    }
    pthread_cond_broadcast(&_capacityChanged);
    pthread_mutex_unlock(&_mutex);
    NSError *error = InfernoMetalMakeError(
        InfernoMetalErrorClosed, @"The command queue has been invalidated");
    for (InfernoMetalCommandBuffer *buffer in committed)
        [buffer infernoFail:error scheduled:NO];
}

@end
