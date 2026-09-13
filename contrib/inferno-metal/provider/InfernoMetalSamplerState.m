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
#import "InfernoMetalErrors.h"
#import "InfernoMetalSamplerState.h"

#include <math.h>

@interface InfernoMetalSamplerState ()
@property(nonatomic, strong) InfernoMetalCompilerContext *storedContext;
@property(nonatomic, strong) id<MTLDevice> storedDevice;
@property(nonatomic, copy, nullable) NSString *storedLabel;
@property(nonatomic) ImtlBatch5Sampler storedRecord;
@end
@implementation InfernoMetalSamplerState
static uint32_t samplerAddressMode(MTLSamplerAddressMode mode)
{
    switch (mode) {
    case MTLSamplerAddressModeClampToEdge:
        return INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_EDGE;
    case MTLSamplerAddressModeMirrorRepeat:
        return INFERNO_METAL_RESOURCE_ADDRESS_MIRROR_REPEAT;
    case MTLSamplerAddressModeClampToZero:
        return INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_ZERO;
    case MTLSamplerAddressModeRepeat:
        return INFERNO_METAL_RESOURCE_ADDRESS_REPEAT;
    case MTLSamplerAddressModeMirrorClampToEdge:
        return INFERNO_METAL_RESOURCE_ADDRESS_MIRROR_CLAMP_TO_EDGE;
    case MTLSamplerAddressModeClampToBorderColor:
        return INFERNO_METAL_RESOURCE_ADDRESS_CLAMP_TO_BORDER_COLOR;
    default:
        return UINT32_MAX;
    }
}

- (instancetype)initWithContext:(InfernoMetalCompilerContext *)context
                         device:(id<MTLDevice>)device
                     descriptor:(MTLSamplerDescriptor *)d
{
    if (!(self = [super init]) || !context || !device || !d ||
        d.minFilter > MTLSamplerMinMagFilterLinear ||
        d.magFilter > MTLSamplerMinMagFilterLinear ||
        d.mipFilter > MTLSamplerMipFilterLinear || !d.maxAnisotropy ||
        d.maxAnisotropy > 16 ||
        samplerAddressMode(d.sAddressMode) == UINT32_MAX ||
        samplerAddressMode(d.tAddressMode) == UINT32_MAX ||
        samplerAddressMode(d.rAddressMode) == UINT32_MAX || d.borderColor > 2 ||
        d.reductionMode != MTLSamplerReductionModeWeightedAverage ||
        !isfinite(d.lodMinClamp) || !isfinite(d.lodMaxClamp) ||
        d.lodMinClamp < 0 || d.lodMaxClamp < d.lodMinClamp || d.lodAverage ||
        d.lodBias != 0 || signbit(d.lodBias) ||
        d.compareFunction > MTLCompareFunctionAlways)
        return nil;
    if (!d.normalizedCoordinates &&
        (d.sAddressMode != MTLSamplerAddressModeClampToEdge ||
         d.tAddressMode != MTLSamplerAddressModeClampToEdge ||
         d.rAddressMode != MTLSamplerAddressModeClampToEdge ||
         d.mipFilter != MTLSamplerMipFilterNotMipmapped ||
         d.minFilter != d.magFilter || d.maxAnisotropy != 1))
        return nil;
    _storedContext = context;
    _storedDevice = device;
    _storedLabel = [d.label copy];
    _storedRecord = (ImtlBatch5Sampler){
        .min_filter = (uint32_t)d.minFilter,
        .mag_filter = (uint32_t)d.magFilter,
        .mip_filter = (uint32_t)d.mipFilter,
        .max_anisotropy = (uint32_t)d.maxAnisotropy,
        .s_address_mode = samplerAddressMode(d.sAddressMode),
        .t_address_mode = samplerAddressMode(d.tAddressMode),
        .r_address_mode = samplerAddressMode(d.rAddressMode),
        .border_color = (uint32_t)d.borderColor,
        .reduction_mode = (uint32_t)d.reductionMode,
        .normalized_coordinates = d.normalizedCoordinates,
        .lod_min_clamp = d.lodMinClamp,
        .lod_max_clamp = d.lodMaxClamp,
        .lod_average = d.lodAverage,
        .lod_bias = d.lodBias,
        .compare_function = (uint32_t)d.compareFunction,
        .support_argument_buffers = d.supportArgumentBuffers,
    };
    return self;
}
- (InfernoMetalCompilerContext *)infernoContext
{
    return _storedContext;
}
- (ImtlBatch5Sampler)infernoRecord
{
    return _storedRecord;
}
- (BOOL)infernoSupportsArgumentBuffers
{
    return _storedRecord.support_argument_buffers != 0;
}
- (NSString *)label
{
    return _storedLabel;
}
- (id<MTLDevice>)device
{
    return _storedDevice;
}
- (MTLResourceID)gpuResourceID
{
    [NSException raise:InfernoMetalUnsupportedException
                format:@"No stable GPU resource ID exists"];
    return (MTLResourceID){ 0 };
}
@end
