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

@class InfernoMetalCommandBuffer;
@class InfernoMetalCompilerContext;

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalCommandQueue : NSObject <MTLCommandQueue>

    -(nullable instancetype)initWithContext
        : (InfernoMetalCompilerContext *)context device
        : (id<MTLDevice>)device maxCount : (NSUInteger)maxCount;

    -(void)infernoClose;
    -(BOOL)infernoReserve : (InfernoMetalCommandBuffer *)buffer;
    -(BOOL)infernoCommitReservation : (InfernoMetalCommandBuffer *)buffer;
    -(nullable InfernoMetalCommandBuffer *)infernoReadyHead;
    -(BOOL)infernoAdmitHead : (InfernoMetalCommandBuffer *)buffer;
    -(void)infernoRemove : (InfernoMetalCommandBuffer *)buffer;

    @property(nonatomic, readonly) InfernoMetalCompilerContext *infernoContext;

    @end

NS_ASSUME_NONNULL_END
