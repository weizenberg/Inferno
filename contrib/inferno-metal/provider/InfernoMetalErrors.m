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
NSErrorUserInfoKey const InfernoMetalFailedRecordKindKey =
    @"InfernoMetalFailedRecordKind";
NSErrorUserInfoKey const InfernoMetalFailedRecordIndexKey =
    @"InfernoMetalFailedRecordIndex";
NSErrorUserInfoKey const InfernoMetalHostStatusKey = @"InfernoMetalHostStatus";
NSErrorUserInfoKey const InfernoMetalScheduledKey = @"InfernoMetalScheduled";
NSExceptionName const InfernoMetalUnsupportedException =
    @"InfernoMetalUnsupportedException";
NSExceptionName const InfernoMetalTransportException =
    @"InfernoMetalTransportException";
NSExceptionName const InfernoMetalInvalidUseException =
    @"InfernoMetalInvalidUseException";

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

NSError *InfernoMetalErrorFromBatchResult(const ImtlBatchResult *r,
                                          IOReturn timer, IOReturn cleanup)
{
    if (!r)
        return InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                     @"Missing batch result");
    if (r->outcome == INFERNO_METAL_BATCH_OUTCOME_OK &&
        timer == kIOReturnSuccess && cleanup == kIOReturnSuccess)
        return nil;
    NSString *description =
        [[NSString alloc] initWithBytes:r->error_description
                                 length:r->error_description_length
                               encoding:NSUTF8StringEncoding];
    NSMutableDictionary *info = [@{
        NSLocalizedDescriptionKey : description ?:
            @"Inferno Metal batch operation failed",
        InfernoMetalPhaseErrorKey : @(r->phase),
        InfernoMetalOutcomeErrorKey : @(r->outcome),
        InfernoMetalFailedRecordKindKey : @(r->failed_record_kind),
        InfernoMetalFailedRecordIndexKey : @(r->failed_record_index),
        InfernoMetalHostStatusKey : @(r->host_command_buffer_status),
        InfernoMetalSequenceErrorKey : @(r->sequence),
        InfernoMetalScheduledKey :
            @((r->flags & INFERNO_METAL_BATCH_FLAG_SCHEDULED) != 0)
    } mutableCopy];
    if (timer != kIOReturnSuccess)
        info[InfernoMetalTimerErrorKey] = @((uint32_t)timer);
    if (cleanup != kIOReturnSuccess)
        info[InfernoMetalCleanupErrorKey] = @((uint32_t)cleanup);
    if (r->flags & INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED)
        info[InfernoMetalDescriptionTruncatedKey] = @YES;
    if (r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED)
        info[InfernoMetalDomainTruncatedKey] = @YES;
    if (!(r->flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR)) {
        NSString *domain =
            [[NSString alloc] initWithBytes:r->error_domain
                                     length:r->error_domain_length
                                   encoding:NSUTF8StringEncoding];
        BOOL truncated = r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED;
        if (truncated && domain)
            info[InfernoMetalDomainPrefixErrorKey] = domain;
        return [NSError
            errorWithDomain:truncated ? InfernoMetalRemoteErrorDomain : domain
                       code:(NSInteger)r->error_code
                   userInfo:info];
    }
    InfernoMetalErrorCode code;
    switch (r->outcome) {
    case INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH:
        code = InfernoMetalErrorInvalidDispatch;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_MALFORMED:
        code = InfernoMetalErrorMalformedBatch;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST:
        code = InfernoMetalErrorUnsupportedHost;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED:
        code = InfernoMetalErrorSpecializationRequired;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH:
        code = InfernoMetalErrorFunctionTypeMismatch;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_EXECUTION_FAILED:
        code = InfernoMetalErrorExecutionFailed;
        break;
    default:
        code = r->outcome == INFERNO_METAL_BATCH_OUTCOME_OK ?
                   InfernoMetalErrorTransport :
                   InfernoMetalErrorRemoteWithoutNSError;
        break;
    }
    return [NSError errorWithDomain:InfernoMetalErrorDomain
                               code:code
                           userInfo:info];
}

NSError *InfernoMetalErrorFromBatch5Result(const ImtlBatch5Result *r,
                                           IOReturn timer, IOReturn cleanup)
{
    if (!r)
        return InfernoMetalMakeError(InfernoMetalErrorProtocol,
                                     @"Missing resource batch result");
    if (r->outcome == INFERNO_METAL_BATCH_OUTCOME_OK &&
        timer == kIOReturnSuccess && cleanup == kIOReturnSuccess)
        return nil;
    NSString *description =
        [[NSString alloc] initWithBytes:r->error_description
                                 length:r->error_description_length
                               encoding:NSUTF8StringEncoding];
    if (!description.length)
        description = nil;
    NSMutableDictionary *info = [@{
        NSLocalizedDescriptionKey : description ?:
            @"Inferno Metal resource batch operation failed",
        InfernoMetalPhaseErrorKey : @(r->phase),
        InfernoMetalOutcomeErrorKey : @(r->outcome),
        InfernoMetalFailedRecordKindKey : @(r->failed_record_kind),
        InfernoMetalFailedRecordIndexKey : @(r->failed_record_index),
        InfernoMetalHostStatusKey : @(r->host_command_buffer_status),
        InfernoMetalSequenceErrorKey : @(r->sequence),
        InfernoMetalScheduledKey :
            @((r->flags & INFERNO_METAL_BATCH_FLAG_SCHEDULED) != 0)
    } mutableCopy];
    if (timer != kIOReturnSuccess)
        info[InfernoMetalTimerErrorKey] = @((uint32_t)timer);
    if (cleanup != kIOReturnSuccess)
        info[InfernoMetalCleanupErrorKey] = @((uint32_t)cleanup);
    if (r->flags & INFERNO_METAL_BATCH_FLAG_DESCRIPTION_TRUNCATED)
        info[InfernoMetalDescriptionTruncatedKey] = @YES;
    if (r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED)
        info[InfernoMetalDomainTruncatedKey] = @YES;
    if (!(r->flags & INFERNO_METAL_BATCH_FLAG_NO_NSERROR)) {
        NSString *domain =
            [[NSString alloc] initWithBytes:r->error_domain
                                     length:r->error_domain_length
                                   encoding:NSUTF8StringEncoding];
        BOOL truncated = r->flags & INFERNO_METAL_BATCH_FLAG_DOMAIN_TRUNCATED;
        if (truncated && domain)
            info[InfernoMetalDomainPrefixErrorKey] = domain;
        return [NSError
            errorWithDomain:truncated ? InfernoMetalRemoteErrorDomain : domain
                       code:(NSInteger)r->error_code
                   userInfo:info];
    }
    InfernoMetalErrorCode code;
    switch (r->outcome) {
    case INFERNO_METAL_BATCH_OUTCOME_INVALID_DISPATCH:
        code = InfernoMetalErrorInvalidDispatch;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_MALFORMED:
        code = InfernoMetalErrorMalformedBatch;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_UNSUPPORTED_HOST:
        code = InfernoMetalErrorUnsupportedHost;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_SPECIALIZATION_REQUIRED:
        code = InfernoMetalErrorSpecializationRequired;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_FUNCTION_TYPE_MISMATCH:
        code = InfernoMetalErrorFunctionTypeMismatch;
        break;
    case INFERNO_METAL_BATCH_OUTCOME_EXECUTION_FAILED:
        code = InfernoMetalErrorExecutionFailed;
        break;
    case INFERNO_METAL_RESOURCE_OUTCOME_UNSUPPORTED_STATE:
        code = InfernoMetalErrorUnsupportedState;
        break;
    case INFERNO_METAL_RESOURCE_OUTCOME_RESOURCE_FAILED:
        code = InfernoMetalErrorResourceCreation;
        break;
    default:
        code = r->outcome == INFERNO_METAL_BATCH_OUTCOME_OK ?
                   InfernoMetalErrorTransport :
                   InfernoMetalErrorRemoteWithoutNSError;
        break;
    }
    return [NSError errorWithDomain:InfernoMetalErrorDomain
                               code:code
                           userInfo:info];
}
