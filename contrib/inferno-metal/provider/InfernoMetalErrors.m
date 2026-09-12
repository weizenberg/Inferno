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
#import "InfernoMetalErrors.h"

NSErrorDomain const InfernoMetalErrorDomain = @"org.inferno.metal";
NSErrorDomain const InfernoMetalRemoteErrorDomain = @"org.inferno.metal.remote";
NSErrorUserInfoKey const InfernoMetalPhaseErrorKey = @"InfernoMetalPhase";
NSErrorUserInfoKey const InfernoMetalOutcomeErrorKey = @"InfernoMetalOutcome";
NSErrorUserInfoKey const InfernoMetalDomainPrefixErrorKey =
    @"InfernoMetalRemoteDomain";
NSErrorUserInfoKey const InfernoMetalDescriptionTruncatedKey =
    @"InfernoMetalDescriptionTruncated";
NSErrorUserInfoKey const InfernoMetalDomainTruncatedKey =
    @"InfernoMetalDomainTruncated";
NSErrorUserInfoKey const InfernoMetalCleanupErrorKey = @"InfernoMetalCleanup";
NSErrorUserInfoKey const InfernoMetalIOErrorKey = @"InfernoMetalIOReturn";
NSErrorUserInfoKey const InfernoMetalSequenceErrorKey = @"InfernoMetalSequence";
NSErrorUserInfoKey const InfernoMetalStateErrorKey = @"InfernoMetalState";
NSErrorUserInfoKey const InfernoMetalCompletionErrorKey =
    @"InfernoMetalCompletionError";
NSErrorUserInfoKey const InfernoMetalTimerErrorKey = @"InfernoMetalTimerError";
NSExceptionName const InfernoMetalUnsupportedException =
    @"InfernoMetalUnsupportedException";
NSExceptionName const InfernoMetalTransportException =
    @"InfernoMetalTransportException";

NSError *InfernoMetalMakeError(InfernoMetalErrorCode code,
                               NSString *description)
{
    return
        [NSError errorWithDomain:InfernoMetalErrorDomain
                            code:code
                        userInfo:@{ NSLocalizedDescriptionKey : description }];
}

NSError *InfernoMetalErrorFromCompilerResult(const ImtlCompilerResult *r,
                                             IOReturn cleanup)
{
    if (!r)
        return InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                     @"Missing compiler result");
    if (r->outcome == INFERNO_METAL_COMPILER_OUTCOME_OK &&
        !(r->flags & INFERNO_METAL_COMPILER_FLAG_WARNING))
        return nil;
    NSString *description = [[NSString alloc] initWithBytes:r->error_description length:r->error_description_length encoding:NSUTF8StringEncoding] ?: @"Remote Metal operation failed";
    NSMutableDictionary *info = [@{
        NSLocalizedDescriptionKey : description,
        InfernoMetalPhaseErrorKey : @(r->phase),
        InfernoMetalOutcomeErrorKey : @(r->outcome)
    } mutableCopy];
    if (cleanup != kIOReturnSuccess)
        info[InfernoMetalCleanupErrorKey] = @(cleanup);
    if (r->flags & INFERNO_METAL_COMPILER_FLAG_DESCRIPTION_TRUNCATED)
        info[InfernoMetalDescriptionTruncatedKey] = @YES;
    if (r->flags & INFERNO_METAL_COMPILER_FLAG_DOMAIN_TRUNCATED)
        info[InfernoMetalDomainTruncatedKey] = @YES;
    if (!(r->flags & INFERNO_METAL_COMPILER_FLAG_NO_NSERROR)) {
        NSString *domain =
            [[NSString alloc] initWithBytes:r->error_domain
                                     length:r->error_domain_length
                                   encoding:NSUTF8StringEncoding];
        bool truncated =
            r->flags & INFERNO_METAL_COMPILER_FLAG_DOMAIN_TRUNCATED;
        if (truncated && domain)
            info[InfernoMetalDomainPrefixErrorKey] = domain;
        return [NSError
            errorWithDomain:truncated ? InfernoMetalRemoteErrorDomain : domain
                       code:(NSInteger)r->error_code
                   userInfo:info];
    }
    InfernoMetalErrorCode code =
        r->outcome == INFERNO_METAL_COMPILER_OUTCOME_SPECIALIZATION_REQUIRED ?
            InfernoMetalErrorSpecializationRequired :
        r->outcome == INFERNO_METAL_COMPILER_OUTCOME_FUNCTION_TYPE_MISMATCH ?
            InfernoMetalErrorFunctionTypeMismatch :
            InfernoMetalErrorRemoteWithoutNSError;
    return [NSError errorWithDomain:InfernoMetalErrorDomain
                               code:code
                           userInfo:info];
}

NSError *InfernoMetalErrorFromCoordinator(const ImtlCoordinatorError *e)
{
    if (!e)
        return InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                     @"Missing coordinator error");
    InfernoMetalErrorCode code = InfernoMetalErrorTransport;
    switch (e->kind) {
    case IMTL_COORDINATOR_ERROR_BUSY:
        code = InfernoMetalErrorBusy;
        break;
    case IMTL_COORDINATOR_ERROR_TIMEOUT:
        code = InfernoMetalErrorTimeout;
        break;
    case IMTL_COORDINATOR_ERROR_UNCERTAIN:
        code = InfernoMetalErrorUncertainSubmission;
        break;
    case IMTL_COORDINATOR_ERROR_DEVICE_FAULT:
        code = InfernoMetalErrorDeviceFault;
        break;
    case IMTL_COORDINATOR_ERROR_PROTOCOL:
        code = InfernoMetalErrorProtocol;
        break;
    case IMTL_COORDINATOR_ERROR_CLOSED:
        code = InfernoMetalErrorClosed;
        break;
    case IMTL_COORDINATOR_ERROR_INVALID_ARGUMENT:
        code = InfernoMetalErrorInvalidArgument;
        break;
    default:
        break;
    }
    return [NSError
        errorWithDomain:InfernoMetalErrorDomain
                   code:code
               userInfo:@{
                   NSLocalizedDescriptionKey :
                       @"Inferno Metal coordinator operation failed",
                   InfernoMetalIOErrorKey : @((uint32_t)e->io),
                   InfernoMetalSequenceErrorKey : @(e->sequence),
                   InfernoMetalStateErrorKey : @(e->state),
                   InfernoMetalCompletionErrorKey : @(e->completion_error),
                   InfernoMetalTimerErrorKey : @((uint32_t)e->timer_error),
                   InfernoMetalCleanupErrorKey : @((uint32_t)e->cleanup_io)
               }];
}
