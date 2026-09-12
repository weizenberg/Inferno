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

@class InfernoMetalCommandBuffer;
@class InfernoMetalCommandQueue;

@interface InfernoMetalCompilerContext ()
@property(nonatomic) ImtlCoordinator *coordinator;
@property(nonatomic, weak) id<MTLDevice> owner;
@property(nonatomic, strong) dispatch_queue_t completionQueue;
@property(nonatomic, strong) dispatch_queue_t executionQueue;
@property(nonatomic, strong) NSLock *schedulerLock;
@property(nonatomic, strong) NSHashTable<InfernoMetalCommandQueue *> *queues;
@property(nonatomic) BOOL executionClosed;
@property(nonatomic) uint64_t nextCommitSerial;
@end

@interface InfernoMetalCompilerContext (Execution)
- (BOOL)registerCommandQueue:(InfernoMetalCommandQueue *)queue;
- (uint64_t)commitCommandBuffer:(InfernoMetalCommandBuffer *)commandBuffer
                          queue:(InfernoMetalCommandQueue *)queue;
- (void)scheduleExecutionPump;
- (void)runExecutionPump;
- (void)executeCommandBuffer:(InfernoMetalCommandBuffer *)commandBuffer;
@end
