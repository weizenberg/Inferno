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

#include "../coordinator.h"

NS_ASSUME_NONNULL_BEGIN

    FOUNDATION_EXPORT NSErrorDomain const InfernoMetalErrorDomain;
    FOUNDATION_EXPORT NSErrorDomain const InfernoMetalRemoteErrorDomain;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalPhaseErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalOutcomeErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalDomainPrefixErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const
        InfernoMetalDescriptionTruncatedKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalDomainTruncatedKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalCleanupErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalIOErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalSequenceErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalStateErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalCompletionErrorKey;
    FOUNDATION_EXPORT NSErrorUserInfoKey const InfernoMetalTimerErrorKey;
    FOUNDATION_EXPORT NSExceptionName const InfernoMetalUnsupportedException;
    FOUNDATION_EXPORT NSExceptionName const InfernoMetalTransportException;

    typedef NS_ERROR_ENUM(InfernoMetalErrorDomain, InfernoMetalErrorCode){
        InfernoMetalErrorUnsupported = 1,
        InfernoMetalErrorInvalidArgument = 2,
        InfernoMetalErrorBusy = 3,
        InfernoMetalErrorTimeout = 4,
        InfernoMetalErrorTransport = 5,
        InfernoMetalErrorUncertainSubmission = 6,
        InfernoMetalErrorDeviceFault = 7,
        InfernoMetalErrorProtocol = 8,
        InfernoMetalErrorClosed = 9,
        InfernoMetalErrorSpecializationRequired = 10,
        InfernoMetalErrorFunctionTypeMismatch = 11,
        InfernoMetalErrorRemoteWithoutNSError = 12,
    };

    FOUNDATION_EXPORT NSError *InfernoMetalMakeError(InfernoMetalErrorCode code,
                                                     NSString * description);
    FOUNDATION_EXPORT NSError *_Nullable InfernoMetalErrorFromCompilerResult(
        const ImtlCompilerResult *result, IOReturn cleanupIO);
    FOUNDATION_EXPORT NSError *InfernoMetalErrorFromCoordinator(
        const ImtlCoordinatorError *error);

NS_ASSUME_NONNULL_END
