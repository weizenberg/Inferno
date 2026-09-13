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

#include "../compiler-client.h"

@class InfernoMetalBuffer;
@class InfernoMetalCompilerContext;

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalArgumentLayoutKey : NSObject <NSCopying>
    -(instancetype)initWithPayload : (NSData *)payload kind
        : (uint32_t)kind name : (NSString *)name functionType
        : (MTLFunctionType)type bufferIndex : (NSUInteger)index;
    @property(nonatomic, readonly) NSData *payload;
    @property(nonatomic, readonly) uint32_t libraryKind;
    @property(nonatomic, readonly) NSString *functionName;
    @property(nonatomic, readonly) MTLFunctionType functionType;
    @property(nonatomic, readonly) NSUInteger bufferIndex;
    @end

    @interface InfernoMetalArgumentMemberLayout : NSObject
    -(instancetype)initWithRecord : (ImtlArgumentLayoutMember)record;
    @property(nonatomic, readonly) ImtlArgumentLayoutMember record;
    @end

    @interface InfernoMetalArgumentLayout : NSObject
    -(nullable instancetype)initWithKey
        : (InfernoMetalArgumentLayoutKey *)key result
        : (const ImtlCompilerResult *)result;
    @property(nonatomic, readonly) InfernoMetalArgumentLayoutKey *key;
    @property(nonatomic, readonly) NSUInteger encodedLength;
    @property(nonatomic, readonly) NSUInteger alignment;
    @property(nonatomic, readonly)
        NSArray<InfernoMetalArgumentMemberLayout *> *members;
    -(nullable InfernoMetalArgumentMemberLayout *)memberForIndex
        : (NSUInteger)index;
    @end

    @interface InfernoMetalEncodedArgumentMember : NSObject
    -(instancetype)initWithLayout
        : (InfernoMetalArgumentMemberLayout *)layout memberIndex
        : (NSUInteger)memberIndex resource : (nullable id)resource offset
        : (NSUInteger)offset constantData
        : (nullable NSData *)data explicitlyNull : (BOOL)explicitlyNull;
    @property(nonatomic, readonly) InfernoMetalArgumentMemberLayout *layout;
    @property(nonatomic, readonly) NSUInteger memberIndex;
    @property(nonatomic, readonly, nullable) id resource;
    @property(nonatomic, readonly) NSUInteger offset;
    @property(nonatomic, readonly, nullable) NSData *constantData;
    @property(nonatomic, readonly) BOOL explicitlyNull;
    @end

    @interface InfernoMetalEncodedArgument : NSObject
    -(instancetype)initWithLayout : (InfernoMetalArgumentLayout *)layout members
        : (NSArray<InfernoMetalEncodedArgumentMember *> *)members;
    @property(nonatomic, readonly) InfernoMetalArgumentLayout *layout;
    @property(nonatomic, readonly)
        NSArray<InfernoMetalEncodedArgumentMember *> *members;
    @end

    @interface InfernoMetalArgumentRegion : NSObject
    -(instancetype)initWithLayout : (InfernoMetalArgumentLayout *)layout base
        : (NSUInteger)base;
    @property(nonatomic, readonly) InfernoMetalArgumentLayout *layout;
    @property(nonatomic, readonly) NSUInteger base;
    -(void)setResources : (NSArray *)resources offsets
        : (NSArray<NSNumber *> *)offsets indexes
        : (NSArray<NSNumber *> *)indexes;
    -(nullable InfernoMetalEncodedArgument *)snapshotFromData : (NSData *)data;
    @end

    @interface InfernoMetalResourceDeclaration : NSObject
    -(instancetype)initWithResource : (id)resource usage : (uint32_t)usage;
    -(instancetype)initWithResource : (id)resource vertexUsage
        : (uint32_t)vertexUsage fragmentUsage : (uint32_t)fragmentUsage;
    @property(nonatomic, readonly) id resource;
    @property(nonatomic) uint32_t usage;
    @property(nonatomic) uint32_t vertexUsage;
    @property(nonatomic) uint32_t fragmentUsage;
    @end

NS_ASSUME_NONNULL_END
