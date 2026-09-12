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
#import "InfernoMetalReflectionObjects.h"

@interface InfernoMetalFunctionConstant ()
@property(nonatomic, copy) NSString *storedName;
@property(nonatomic) MTLDataType storedType;
@property(nonatomic) NSUInteger storedIndex;
@property(nonatomic) BOOL storedRequired;
@end
@implementation InfernoMetalFunctionConstant
- (instancetype)initWithName:(NSString *)name
                        type:(MTLDataType)type
                       index:(NSUInteger)index
                    required:(BOOL)required
{
    if ((self = [super init])) {
        _storedName = [name copy];
        _storedType = type;
        _storedIndex = index;
        _storedRequired = required;
    }
    return self;
}
- (NSString *)name
{
    return _storedName;
}
- (MTLDataType)type
{
    return _storedType;
}
- (NSUInteger)index
{
    return _storedIndex;
}
- (BOOL)required
{
    return _storedRequired;
}
@end

#define IMTL_ATTRIBUTE_IMPL(CLASS)                                       \
    @interface CLASS ()                                                  \
    @property(nonatomic, copy) NSString *storedName;                     \
    @property(nonatomic) MTLDataType storedType;                         \
    @property(nonatomic) NSUInteger storedIndex;                         \
    @property(nonatomic) BOOL storedActive, storedPatch, storedControl;  \
    @end                                                                 \
    @implementation CLASS                                                \
    -(instancetype)initWithName : (NSString *)name type                  \
        : (MTLDataType)type index : (NSUInteger)index active             \
        : (BOOL)active patchData : (BOOL)patchData patchControlPointData \
        : (BOOL)control                                                  \
    {                                                                    \
        if ((self = [super init])) {                                     \
            _storedName = [name copy];                                   \
            _storedType = type;                                          \
            _storedIndex = index;                                        \
            _storedActive = active;                                      \
            _storedPatch = patchData;                                    \
            _storedControl = control;                                    \
        }                                                                \
        return self;                                                     \
    }                                                                    \
    -(NSString *)name                                                    \
    {                                                                    \
        return _storedName;                                              \
    }                                                                    \
    -(MTLDataType)attributeType                                          \
    {                                                                    \
        return _storedType;                                              \
    }                                                                    \
    -(NSUInteger)attributeIndex                                          \
    {                                                                    \
        return _storedIndex;                                             \
    }                                                                    \
    -(BOOL)isActive                                                      \
    {                                                                    \
        return _storedActive;                                            \
    }                                                                    \
    -(BOOL)isPatchData                                                   \
    {                                                                    \
        return _storedPatch;                                             \
    }                                                                    \
    -(BOOL)isPatchControlPointData                                       \
    {                                                                    \
        return _storedControl;                                           \
    }                                                                    \
    @end

IMTL_ATTRIBUTE_IMPL(InfernoMetalVertexAttribute)
IMTL_ATTRIBUTE_IMPL(InfernoMetalAttribute)
