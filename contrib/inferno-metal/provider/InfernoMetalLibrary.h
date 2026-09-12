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

@class InfernoMetalCompilerContext;

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalLibrary : NSObject <MTLLibrary>

    -(nullable instancetype)initWithContext
        : (InfernoMetalCompilerContext *)context device
        : (id<MTLDevice>)device source : (NSData *)source result
        : (const ImtlCompilerResult *)result compileWarning
        : (nullable NSError *)compileWarning error : (NSError **)error;

    @property(nonatomic, readonly) InfernoMetalCompilerContext *infernoContext;
    @property(nonatomic, readonly) NSData *infernoSource;
    @property(nonatomic, readonly, nullable) NSError *compileWarning;

    @end

NS_ASSUME_NONNULL_END
