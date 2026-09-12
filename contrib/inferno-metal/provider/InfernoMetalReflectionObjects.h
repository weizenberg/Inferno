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

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalFunctionConstant : MTLFunctionConstant
    -(nullable instancetype)initWithName : (NSString *)name type
        : (MTLDataType)type index : (NSUInteger)index required : (BOOL)required;
    @end

    @interface InfernoMetalVertexAttribute : MTLVertexAttribute
    -(nullable instancetype)initWithName : (NSString *)name type
        : (MTLDataType)type index : (NSUInteger)index active
        : (BOOL)active patchData : (BOOL)patchData patchControlPointData
        : (BOOL)patchControlPointData;
    @end

    @interface InfernoMetalAttribute : MTLAttribute
    -(nullable instancetype)initWithName : (NSString *)name type
        : (MTLDataType)type index : (NSUInteger)index active
        : (BOOL)active patchData : (BOOL)patchData patchControlPointData
        : (BOOL)patchControlPointData;
    @end

NS_ASSUME_NONNULL_END
