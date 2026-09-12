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

@class InfernoMetalCommandQueue;
@class InfernoMetalCompilerContext;
@class InfernoMetalComputeCommandEncoder;

NS_ASSUME_NONNULL_BEGIN

    @interface InfernoMetalCommandBuffer : NSObject <MTLCommandBuffer>

    -(nullable instancetype)initWithQueue
        : (InfernoMetalCommandQueue *)queue device
        : (id<MTLDevice>)device context
        : (InfernoMetalCompilerContext *)context errorOptions
        : (MTLCommandBufferErrorOption)options reservationToken
        : (uint64_t)reservationToken;

    @property(nonatomic, readonly) uint64_t infernoCommitSerial;
    @property(nonatomic, readonly) BOOL infernoAdmitted;
    @property(nonatomic, readonly) NSArray *infernoDispatches;
    @property(nonatomic, readonly) InfernoMetalCompilerContext *infernoContext;
    @property(nonatomic, readonly) uint64_t infernoReservationToken;

    -(void)infernoSetAdmitted;
    -(BOOL)infernoPublishCommitSerial : (uint64_t)serial;
    -(BOOL)infernoMarkTokenReturned;
    -(void)infernoObserveScheduled;
    -(void)infernoCompleteWithError : (nullable NSError *)error scheduled
        : (BOOL)scheduled;
    -(void)infernoFail : (NSError *)error scheduled : (BOOL)scheduled;
    -(BOOL)infernoAppendDispatch : (id)dispatch;
    -(void)infernoEncoderEnded : (InfernoMetalComputeCommandEncoder *)encoder;

    @end

NS_ASSUME_NONNULL_END
