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

#import "InfernoMetalTexture.h"
#import "InfernoMetalCompilerContext.h"
#import "InfernoMetalErrors.h"

#import <TargetConditionals.h>

#include "standard-headers/inferno/metal.h"
#include <stdlib.h>
#include <string.h>

@interface InfernoMetalTexture () {
    void *_storage;
}
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic) NSUInteger storedWidth;
@property(nonatomic) NSUInteger storedHeight;
@property(nonatomic) MTLPixelFormat storedPixelFormat;
@property(nonatomic) MTLTextureUsage storedUsage;
@property(nonatomic) BOOL storedOptimized;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@end

@implementation InfernoMetalTexture

static void textureInvalid(NSString *message)
{
    [NSException raise:InfernoMetalInvalidUseException format:@"%@", message];
}

- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                     descriptor:(MTLTextureDescriptor *)descriptor
{
    NSUInteger width = descriptor.width;
    NSUInteger height = descriptor.height;
    if (!(self = [super init]) || !context || !device || !descriptor ||
        descriptor.textureType != MTLTextureType2D || !width || !height ||
        width > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
        height > INFERNO_METAL_RESOURCE_MAX_TEXTURE_DIMENSION ||
        width > SIZE_MAX / INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL ||
        width * INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL >
            SIZE_MAX / height)
        return nil;
    NSUInteger size =
        width * INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL * height;
    if (size > INFERNO_METAL_RESOURCE_MAX_IMAGES)
        return nil;
    _storage = calloc(1, size);
    if (!_storage)
        return nil;
    _storedContext = context;
    _storedDevice = device;
    _storedWidth = width;
    _storedHeight = height;
    _storedPixelFormat = descriptor.pixelFormat;
    _storedUsage = descriptor.usage;
    _storedOptimized = descriptor.allowGPUOptimizedContents;
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
- (NSUInteger)infernoImageSize
{
    return _storedWidth * INFERNO_METAL_RESOURCE_TEXTURE_BYTES_PER_PIXEL *
           _storedHeight;
}
- (NSData *)infernoSnapshot
{
    @synchronized(self) {
        return [NSData dataWithBytes:_storage length:self.infernoImageSize];
    }
}
- (BOOL)infernoReplaceSnapshot:(NSData *)snapshot
{
    if (snapshot.length != self.infernoImageSize)
        return NO;
    @synchronized(self) {
        memcpy(_storage, snapshot.bytes, snapshot.length);
    }
    return YES;
}

static BOOL validRegion(MTLRegion region, NSUInteger width, NSUInteger height)
{
    return region.origin.z == 0 && region.size.depth == 1 &&
           region.size.width && region.size.height &&
           region.origin.x <= width &&
           region.size.width <= width - region.origin.x &&
           region.origin.y <= height &&
           region.size.height <= height - region.origin.y;
}

- (void)copyRegion:(MTLRegion)region
             level:(NSUInteger)level
             slice:(NSUInteger)slice
           toBytes:(void *)bytes
       bytesPerRow:(NSUInteger)bytesPerRow
     bytesPerImage:(NSUInteger)bytesPerImage
{
    NSUInteger active = region.size.width * 4;
    if (!bytes || level || slice ||
        !validRegion(region, _storedWidth, _storedHeight) ||
        bytesPerRow < active ||
        (region.size.height &&
         bytesPerRow > NSUIntegerMax / region.size.height) ||
        (bytesPerImage && bytesPerImage < bytesPerRow * region.size.height))
        textureInvalid(@"The texture read region or row layout is invalid");
    @synchronized(self) {
        for (NSUInteger y = 0; y < region.size.height; y++) {
            const uint8_t *source = _storage;
            source +=
                ((region.origin.y + y) * _storedWidth + region.origin.x) * 4;
            memcpy((uint8_t *)bytes + y * bytesPerRow, source, active);
        }
    }
}

- (void)copyRegion:(MTLRegion)region
             level:(NSUInteger)level
             slice:(NSUInteger)slice
         fromBytes:(const void *)bytes
       bytesPerRow:(NSUInteger)bytesPerRow
     bytesPerImage:(NSUInteger)bytesPerImage
{
    NSUInteger active = region.size.width * 4;
    if (!bytes || level || slice ||
        !validRegion(region, _storedWidth, _storedHeight) ||
        bytesPerRow < active ||
        (region.size.height &&
         bytesPerRow > NSUIntegerMax / region.size.height) ||
        (bytesPerImage && bytesPerImage < bytesPerRow * region.size.height))
        textureInvalid(@"The texture upload region or row layout is invalid");
    @synchronized(self) {
        for (NSUInteger y = 0; y < region.size.height; y++) {
            uint8_t *destination = _storage;
            destination +=
                ((region.origin.y + y) * _storedWidth + region.origin.x) * 4;
            memcpy(destination, (const uint8_t *)bytes + y * bytesPerRow,
                   active);
        }
    }
}

- (void)getBytes:(void *)bytes
     bytesPerRow:(NSUInteger)bytesPerRow
      fromRegion:(MTLRegion)region
     mipmapLevel:(NSUInteger)level
{
    [self copyRegion:region
                level:level
                slice:0
              toBytes:bytes
          bytesPerRow:bytesPerRow
        bytesPerImage:0];
}
- (void)getBytes:(void *)bytes
      bytesPerRow:(NSUInteger)bytesPerRow
    bytesPerImage:(NSUInteger)bytesPerImage
       fromRegion:(MTLRegion)region
      mipmapLevel:(NSUInteger)level
            slice:(NSUInteger)slice
{
    [self copyRegion:region
                level:level
                slice:slice
              toBytes:bytes
          bytesPerRow:bytesPerRow
        bytesPerImage:bytesPerImage];
}
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)bytesPerRow
{
    [self copyRegion:region
                level:level
                slice:0
            fromBytes:bytes
          bytesPerRow:bytesPerRow
        bytesPerImage:0];
}
- (void)replaceRegion:(MTLRegion)region
          mipmapLevel:(NSUInteger)level
                slice:(NSUInteger)slice
            withBytes:(const void *)bytes
          bytesPerRow:(NSUInteger)bytesPerRow
        bytesPerImage:(NSUInteger)bytesPerImage
{
    [self copyRegion:region
                level:level
                slice:slice
            fromBytes:bytes
          bytesPerRow:bytesPerRow
        bytesPerImage:bytesPerImage];
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
- (MTLTextureType)textureType
{
    return MTLTextureType2D;
}
- (MTLPixelFormat)pixelFormat
{
    return _storedPixelFormat;
}
- (NSUInteger)width
{
    return _storedWidth;
}
- (NSUInteger)height
{
    return _storedHeight;
}
- (NSUInteger)depth
{
    return 1;
}
- (NSUInteger)mipmapLevelCount
{
    return 1;
}
- (NSUInteger)sampleCount
{
    return 1;
}
- (NSUInteger)arrayLength
{
    return 1;
}
- (MTLTextureUsage)usage
{
    return _storedUsage;
}
- (BOOL)isShareable
{
    return NO;
}
- (BOOL)isFramebufferOnly
{
    return NO;
}
- (BOOL)isSparse
{
    return NO;
}
- (MTLTextureSparseTier)sparseTextureTier
{
    return MTLTextureSparseTierNone;
}
- (NSUInteger)firstMipmapInTail
{
    return 0;
}
- (NSUInteger)tailSizeInBytes
{
    return 0;
}
- (BOOL)allowGPUOptimizedContents
{
    return _storedOptimized;
}
- (MTLTextureCompressionType)compressionType
{
    return MTLTextureCompressionTypeLossless;
}
- (MTLTextureSwizzleChannels)swizzle
{
    return MTLTextureSwizzleChannelsDefault;
}
- (id<MTLTexture>)parentTexture
{
    return nil;
}
- (NSUInteger)parentRelativeLevel
{
    return 0;
}
- (NSUInteger)parentRelativeSlice
{
    return 0;
}
- (id<MTLBuffer>)buffer
{
    return nil;
}
- (NSUInteger)bufferOffset
{
    return 0;
}
- (NSUInteger)bufferBytesPerRow
{
    return 0;
}
- (IOSurfaceRef)iosurface
{
    return NULL;
}
- (NSUInteger)iosurfacePlane
{
    return 0;
}
- (id<MTLResource>)rootResource
{
    return nil;
}
- (NSUInteger)allocatedSize
{
    return self.infernoImageSize;
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
    return MTLHazardTrackingModeTracked;
}
- (MTLResourceOptions)resourceOptions
{
    return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache |
           (MTLHazardTrackingModeTracked << MTLResourceHazardTrackingModeShift);
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
                format:@"Texture aliasing is unsupported"];
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state
{
    if (state != MTLPurgeableStateKeepCurrent &&
        state != MTLPurgeableStateNonVolatile)
        [NSException raise:InfernoMetalUnsupportedException
                    format:@"Discardable textures are unsupported"];
    return MTLPurgeableStateNonVolatile;
}
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)identity
{
    (void)identity;
    return KERN_NOT_SUPPORTED;
}
- (MTLResourceID)gpuResourceID
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"No stable GPU resource ID exists"];
    return (MTLResourceID){ 0 };
}
- (id<MTLTexture>)newTextureViewWithDescriptor:
    (MTLTextureViewDescriptor *)descriptor
{
    (void)descriptor;
    return nil;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
{
    (void)format;
    return nil;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
{
    (void)format;
    (void)type;
    (void)levels;
    (void)slices;
    return nil;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format
                                    textureType:(MTLTextureType)type
                                         levels:(NSRange)levels
                                         slices:(NSRange)slices
                                        swizzle:
                                            (MTLTextureSwizzleChannels)swizzle
{
    (void)format;
    (void)type;
    (void)levels;
    (void)slices;
    (void)swizzle;
    return nil;
}
- (MTLSharedTextureHandle *)newSharedTextureHandle
{
    return nil;
}
#if TARGET_OS_OSX
- (id<MTLTexture>)remoteStorageTexture
{
    return nil;
}
- (id<MTLTexture>)newRemoteTextureViewForDevice:(id<MTLDevice>)device
{
    (void)device;
    [NSException raise:InfernoMetalUnsupportedException
                format:@"Remote texture views are unsupported"];
    return nil;
}
#endif
@end
