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

#import "InfernoMetalArgumentObjects.h"
#import "InfernoMetalBuffer.h"
#import "InfernoMetalErrors.h"

#include "standard-headers/inferno/metal.h"

@interface InfernoMetalWeakArgumentResource : NSObject
@property(nonatomic, weak, nullable) id resource;
@end
@implementation InfernoMetalWeakArgumentResource
@end

@implementation InfernoMetalArgumentLayoutKey
- (instancetype)initWithPayload:(NSData *)payload
                           kind:(uint32_t)kind
                           name:(NSString *)name
                    bufferIndex:(NSUInteger)index
{
    if (!(self = [super init]) || !payload || !name)
        return nil;
    _payload = [payload copy];
    _libraryKind = kind;
    _functionName = [name copy];
    _bufferIndex = index;
    return self;
}
- (id)copyWithZone:(NSZone *)zone
{
    (void)zone;
    return self;
}
- (NSUInteger)hash
{
    return _payload.hash ^ _functionName.hash ^ _libraryKind ^ _bufferIndex;
}
- (BOOL)isEqual:(id)object
{
    if (object == self)
        return YES;
    if (![object isKindOfClass:[InfernoMetalArgumentLayoutKey class]])
        return NO;
    InfernoMetalArgumentLayoutKey *other = object;
    return _libraryKind == other.libraryKind &&
           _bufferIndex == other.bufferIndex &&
           [_functionName isEqualToString:other.functionName] &&
           [_payload isEqual:other.payload];
}
@end

@implementation InfernoMetalArgumentMemberLayout
- (instancetype)initWithRecord:(ImtlArgumentLayoutMember)record
{
    if (!(self = [super init]))
        return nil;
    _record = record;
    return self;
}
@end

@implementation InfernoMetalArgumentLayout
- (instancetype)initWithKey:(InfernoMetalArgumentLayoutKey *)key
                     result:(const ImtlCompilerResult *)result
{
    ImtlArgumentLayout layout;
    if (!(self = [super init]) || !key ||
        !imtl_compiler_argument_layout(result, &layout) ||
        layout.encoded_length > NSUIntegerMax ||
        layout.alignment > NSUIntegerMax)
        return nil;
    NSMutableArray *members =
        [NSMutableArray arrayWithCapacity:layout.member_count];
    for (uint32_t i = 0; i < layout.member_count; i++) {
        ImtlArgumentLayoutMember member;
        if (!imtl_argument_layout_member_at(&layout, i, &member))
            return nil;
        [members addObject:[[InfernoMetalArgumentMemberLayout alloc]
                               initWithRecord:member]];
    }
    _key = key;
    _encodedLength = (NSUInteger)layout.encoded_length;
    _alignment = (NSUInteger)layout.alignment;
    _members = [members copy];
    return self;
}
- (InfernoMetalArgumentMemberLayout *)memberForIndex:(NSUInteger)index
{
    for (InfernoMetalArgumentMemberLayout *member in _members) {
        ImtlArgumentLayoutMember record = member.record;
        if (record.array_length >= 2) {
            for (uint32_t i = 0; i < record.array_length; i++) {
                uint64_t expanded = (uint64_t)record.member_id +
                                    (uint64_t)i * record.argument_index_stride;
                if (expanded == index)
                    return member;
            }
        } else if (record.member_id == index) {
            return member;
        }
    }
    return nil;
}
@end

@implementation InfernoMetalEncodedArgumentMember
- (instancetype)initWithLayout:(InfernoMetalArgumentMemberLayout *)layout
                   memberIndex:(NSUInteger)memberIndex
                      resource:(id)resource
                        offset:(NSUInteger)offset
                  constantData:(NSData *)data
{
    if (!(self = [super init]) || !layout)
        return nil;
    _layout = layout;
    _memberIndex = memberIndex;
    _resource = resource;
    _offset = offset;
    _constantData = [data copy];
    return self;
}
@end

@implementation InfernoMetalEncodedArgument
- (instancetype)initWithLayout:(InfernoMetalArgumentLayout *)layout
                       members:(NSArray<InfernoMetalEncodedArgumentMember *> *)
                                   members
{
    if (!(self = [super init]) || !layout || !members)
        return nil;
    _layout = layout;
    _members = [members copy];
    return self;
}
@end

@interface InfernoMetalArgumentRegion ()
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, id> *assignments;
@property(nonatomic, strong)
    NSMutableDictionary<NSNumber *, NSNumber *> *offsets;
@end

@implementation InfernoMetalArgumentRegion
- (instancetype)initWithLayout:(InfernoMetalArgumentLayout *)layout
                          base:(NSUInteger)base
{
    if (!(self = [super init]) || !layout)
        return nil;
    _layout = layout;
    _base = base;
    _assignments = [NSMutableDictionary dictionary];
    _offsets = [NSMutableDictionary dictionary];
    return self;
}
- (void)setResources:(NSArray *)resources
             offsets:(NSArray<NSNumber *> *)offsets
             indexes:(NSArray<NSNumber *> *)indexes
{
    @synchronized(self) {
        for (NSUInteger i = 0; i < resources.count; i++) {
            NSNumber *key = indexes[i];
            id resource = resources[i];
            if (resource == [NSNull null]) {
                _assignments[key] = [NSNull null];
                [_offsets removeObjectForKey:key];
            } else {
                InfernoMetalWeakArgumentResource *box =
                    [[InfernoMetalWeakArgumentResource alloc] init];
                box.resource = resource;
                _assignments[key] = box;
                _offsets[key] = offsets[i];
            }
        }
    }
}
- (InfernoMetalEncodedArgument *)snapshotFromData:(NSData *)data
{
    @synchronized(self) {
        NSMutableArray *members = [NSMutableArray array];
        for (InfernoMetalArgumentMemberLayout *layoutMember in _layout
                 .members) {
            ImtlArgumentLayoutMember record = layoutMember.record;
            uint32_t count = record.array_length >= 2 ? record.array_length : 1;
            uint32_t stride =
                record.array_length >= 2 ? record.argument_index_stride : 0;
            for (uint32_t element = 0; element < count; element++) {
                NSUInteger index = record.member_id + element * stride;
                if (record.kind ==
                    INFERNO_METAL_RESOURCE_ARGUMENT_MEMBER_CONSTANT) {
                    NSUInteger offset = _base + record.byte_offset;
                    if (offset > data.length ||
                        record.constant_size > data.length - offset)
                        return nil;
                    NSData *constant = [data
                        subdataWithRange:NSMakeRange(offset,
                                                     record.constant_size)];
                    [members
                        addObject:[[InfernoMetalEncodedArgumentMember alloc]
                                      initWithLayout:layoutMember
                                         memberIndex:index
                                            resource:nil
                                              offset:0
                                        constantData:constant]];
                    continue;
                }
                id entry = _assignments[@(index)];
                if (!entry) {
                    [NSException raise:InfernoMetalInvalidUseException
                                format:@"Argument member %lu is unassigned",
                                       (unsigned long)index];
                }
                if (entry == [NSNull null]) {
                    [NSException
                         raise:InfernoMetalUnsupportedException
                        format:@"Nil argument member %lu is unsupported",
                               (unsigned long)index];
                }
                id resource =
                    ((InfernoMetalWeakArgumentResource *)entry).resource;
                if (!resource) {
                    [NSException raise:InfernoMetalInvalidUseException
                                format:@"Argument member %lu was released",
                                       (unsigned long)index];
                }
                [members addObject:[[InfernoMetalEncodedArgumentMember alloc]
                                       initWithLayout:layoutMember
                                          memberIndex:index
                                             resource:resource
                                               offset:[_offsets[@(index)]
                                                          unsignedIntegerValue]
                                         constantData:nil]];
            }
        }
        [members sortUsingComparator:^NSComparisonResult(
                     InfernoMetalEncodedArgumentMember *a,
                     InfernoMetalEncodedArgumentMember *b) {
          if (a.memberIndex == b.memberIndex)
              return NSOrderedSame;
          return a.memberIndex < b.memberIndex ? NSOrderedAscending :
                                                 NSOrderedDescending;
        }];
        return [[InfernoMetalEncodedArgument alloc] initWithLayout:_layout
                                                           members:members];
    }
}
@end

@implementation InfernoMetalResourceDeclaration
- (instancetype)initWithResource:(id)resource usage:(uint32_t)usage
{
    if (!(self = [super init]) || !resource || !usage)
        return nil;
    _resource = resource;
    _usage = usage;
    return self;
}
@end
