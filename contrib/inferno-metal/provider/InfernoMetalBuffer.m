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
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"

#import <TargetConditionals.h>

#include "standard-headers/inferno/metal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface InfernoMetalBuffer () {
    void *_storage;
}
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic) NSUInteger storedLength;
@property(nonatomic) MTLResourceOptions storedOptions;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@end

@implementation InfernoMetalBuffer

- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                         length:(NSUInteger)length
                        options:(MTLResourceOptions)options
                          bytes:(const void *)bytes
{
    NSUInteger allowed = MTLResourceCPUCacheModeMask |
                         MTLResourceStorageModeMask |
                         MTLResourceHazardTrackingModeMask;
    MTLStorageMode storage =
        (options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift;
    MTLCPUCacheMode cache =
        (options & MTLResourceCPUCacheModeMask) >> MTLResourceCPUCacheModeShift;
    MTLHazardTrackingMode hazard =
        (options & MTLResourceHazardTrackingModeMask) >>
        MTLResourceHazardTrackingModeShift;
    if (!(self = [super init]) || !context || !device || !length ||
        length > INFERNO_METAL_BATCH_MAX_BUFFER_LENGTH ||
        (options & ~allowed) || storage != MTLStorageModeShared ||
        cache != MTLCPUCacheModeDefaultCache ||
        (hazard != MTLHazardTrackingModeDefault &&
         hazard != MTLHazardTrackingModeTracked)) {
        return nil;
    }
    size_t alignment = (size_t)getpagesize();
    if (posix_memalign(&_storage, alignment, length)) {
        return nil;
    }
    if (bytes) {
        memcpy(_storage, bytes, length);
    } else {
        memset(_storage, 0, length);
    }
    _storedContext = context;
    _storedDevice = device;
    _storedLength = length;
    _storedOptions = options;
    return self;
}

- (void)dealloc
{
    free(_storage);
}

- (InfernoMetalCompilerContext *)infernoContext
{
    return _storedContext;
}

- (NSData *)infernoSnapshot
{
    @synchronized(self) {
        return [NSData dataWithBytes:_storage length:_storedLength];
    }
}

- (BOOL)infernoReplaceSnapshot:(NSData *)snapshot
{
    if (snapshot.length != _storedLength)
        return NO;
    @synchronized(self) {
        memcpy(_storage, snapshot.bytes, snapshot.length);
    }
    return YES;
}

- (void *)contents
{
    return _storage;
}

- (NSUInteger)length
{
    return _storedLength;
}

- (NSUInteger)allocatedSize
{
    return _storedLength;
}

- (id<MTLDevice>)device
{
    return _storedDevice;
}

- (NSString *)label
{
    @synchronized(self) {
        return _storedLabel;
    }
}

- (void)setLabel:(NSString *)label
{
    @synchronized(self) {
        _storedLabel = [label copy];
    }
}

- (MTLCPUCacheMode)cpuCacheMode
{
    return MTLCPUCacheModeDefaultCache;
}

- (MTLStorageMode)storageMode
{
    return MTLStorageModeShared;
}

- (MTLHazardTrackingMode)hazardTrackingMode
{
    NSUInteger encoded = (_storedOptions & MTLResourceHazardTrackingModeMask) >>
                         MTLResourceHazardTrackingModeShift;
    return encoded == MTLHazardTrackingModeDefault ?
               MTLHazardTrackingModeTracked :
               (MTLHazardTrackingMode)encoded;
}

- (MTLResourceOptions)resourceOptions
{
    return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache |
           (self.hazardTrackingMode << MTLResourceHazardTrackingModeShift);
}

- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (state != MTLPurgeableStateKeepCurrent &&
        state != MTLPurgeableStateNonVolatile) {
        [NSException raise:InfernoMetalUnsupportedException
                    format:@"Discardable buffers are unsupported"];
    }
    return MTLPurgeableStateNonVolatile;
}

- (id<MTLHeap>)heap
{
    return nil;
}

- (NSUInteger)heapOffset
{
    return 0;
}

- (BOOL)isAliasable
{
    return NO;
}

- (void)makeAliasable
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Buffer aliasing is unsupported"];
}

- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)identity
{
    (void)identity;
    return KERN_NOT_SUPPORTED;
}

- (void)didModifyRange:(NSRange)range
{
    (void)range;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Managed buffers are unsupported"];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                    offset:(NSUInteger)offset
                               bytesPerRow:(NSUInteger)bytesPerRow
{
    (void)descriptor;
    (void)offset;
    (void)bytesPerRow;
    return nil;
}

- (id<MTLTensor>)newTensorWithDescriptor:(MTLTensorDescriptor *)descriptor
                                  offset:(NSUInteger)offset
                                   error:(NSError **)error
{
    (void)descriptor;
    (void)offset;
    if (error) {
        *error = InfernoMetalMakeError(InfernoMetalErrorUnsupported,
                                       @"Buffer tensor views are unsupported");
    }
    return nil;
}

- (void)addDebugMarker:(NSString *)marker range:(NSRange)range
{
    (void)marker;
    (void)range;
}

- (void)removeAllDebugMarkers
{
}

#if TARGET_OS_OSX
- (id<MTLBuffer>)remoteStorageBuffer
{
    return nil;
}

- (id<MTLBuffer>)newRemoteBufferViewForDevice:(id<MTLDevice>)device
{
    (void)device;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Remote buffer views are unsupported"];
    return nil;
}
#endif

- (MTLGPUAddress)gpuAddress
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"No stable GPU address exists"];
    return 0;
}

- (MTLBufferSparseTier)sparseBufferTier
{
    return MTLBufferSparseTierNone;
}

@end
