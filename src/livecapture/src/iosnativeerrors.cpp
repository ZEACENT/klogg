/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "iosnativeerrors.h"

#include <utility>

namespace klogg::livecapture::ios {
namespace {

// These values mirror the public enums in the pinned libimobiledevice 1.4.0
// headers. Keeping them local avoids a vendor-header dependency in the pure
// source-neutral error layer.
namespace NativeCode {
namespace Idevice {
constexpr std::int32_t NoDevice = -3;
constexpr std::int32_t Ssl = -6;
constexpr std::int32_t Timeout = -7;
} // namespace Idevice
namespace Lockdown {
constexpr std::int32_t Ssl = -5;
constexpr std::int32_t Timeout = -7;
constexpr std::int32_t Mux = -8;
constexpr std::int32_t PasswordProtected = -17;
constexpr std::int32_t UserDeniedPairing = -18;
constexpr std::int32_t PairingDialogPending = -19;
constexpr std::int32_t MissingHostId = -20;
constexpr std::int32_t InvalidHostId = -21;
constexpr std::int32_t MissingService = -26;
constexpr std::int32_t InvalidService = -27;
constexpr std::int32_t ServiceLimit = -28;
constexpr std::int32_t MissingPairRecord = -29;
constexpr std::int32_t InvalidPairRecord = -31;
constexpr std::int32_t ServiceProhibited = -34;
constexpr std::int32_t EscrowLocked = -35;
constexpr std::int32_t PairingProhibited = -36;
constexpr std::int32_t FindMyProtected = -37;
constexpr std::int32_t MobileConfigurationProtected = -38;
constexpr std::int32_t MobileConfigurationChallengeRequired = -39;
} // namespace Lockdown
namespace Service {
constexpr std::int32_t Mux = -3;
constexpr std::int32_t Ssl = -4;
constexpr std::int32_t Timeout = -7;
} // namespace Service
namespace OsTrace {
constexpr std::int32_t Mux = -2;
constexpr std::int32_t Ssl = -3;
constexpr std::int32_t NotEnoughData = -4;
constexpr std::int32_t Timeout = -5;
constexpr std::int32_t RequestFailed = -7;
} // namespace OsTrace
namespace SyslogRelay {
constexpr std::int32_t Mux = -2;
constexpr std::int32_t Ssl = -3;
constexpr std::int32_t NotEnoughData = -4;
constexpr std::int32_t Timeout = -5;
} // namespace SyslogRelay
} // namespace NativeCode

ClassifiedIosNativeError
classified( const IosNativeError& nativeError, ErrorCategory category, const char* stableCode,
            ErrorScope scope, RetryPolicy retryPolicy, const char* message,
            std::optional<AwaitingUserReason> awaitingUserReason = std::nullopt )
{
    LiveSourceError error;
    error.category = category;
    error.code = stableCode;
    error.scope = scope;
    error.retryPolicy = retryPolicy;
    error.message = message;
    error.nativeDetail = nativeError.detail;
    return { std::move( error ), awaitingUserReason };
}

bool isCode( const IosNativeError& error, IosNativeErrorDomain domain, std::int32_t code ) noexcept
{
    return error.domain == domain && error.code == code;
}

} // namespace

ClassifiedIosNativeError classifyIosNativeError( const IosNativeError& nativeError )
{
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::PairingDialogPending ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-trust-pending",
                           ErrorScope::Device, RetryPolicy::AwaitUser,
                           "Confirm that this computer is trusted on the iOS device.",
                           AwaitingUserReason::Trust );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::UserDeniedPairing ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-trust-denied",
                           ErrorScope::Device, RetryPolicy::AwaitUser,
                           "Trust for this computer was denied on the iOS device.",
                           AwaitingUserReason::Trust );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::PasswordProtected ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-device-locked",
                           ErrorScope::Device, RetryPolicy::AwaitUser,
                           "Unlock the iOS device to continue.", AwaitingUserReason::Unlock );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::MissingPairRecord )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown,
                    NativeCode::Lockdown::InvalidPairRecord )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown,
                    NativeCode::Lockdown::MissingHostId )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown,
                    NativeCode::Lockdown::InvalidHostId ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-stale-pair", ErrorScope::Device,
                           RetryPolicy::AwaitUser, "Pair the iOS device with this computer again.",
                           AwaitingUserReason::Pair );
    }

    if ( isCode( nativeError, IosNativeErrorDomain::Idevice, NativeCode::Idevice::NoDevice )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown, NativeCode::Lockdown::Mux )
         || isCode( nativeError, IosNativeErrorDomain::Service, NativeCode::Service::Mux )
         || isCode( nativeError, IosNativeErrorDomain::OsTrace, NativeCode::OsTrace::Mux )
         || isCode( nativeError, IosNativeErrorDomain::SyslogRelay,
                    NativeCode::SyslogRelay::Mux ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-device-disconnected",
                           ErrorScope::Device, RetryPolicy::WaitForDevice,
                           "The iOS device disconnected." );
    }

    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown, NativeCode::Lockdown::MissingService )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown,
                    NativeCode::Lockdown::InvalidService ) ) {
        return classified( nativeError, ErrorCategory::Service, "ios-service-missing",
                           ErrorScope::Service, RetryPolicy::Never,
                           "The requested iOS service is unavailable." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::ServiceLimit ) ) {
        return classified( nativeError, ErrorCategory::Service, "ios-service-limit",
                           ErrorScope::Service, RetryPolicy::Backoff,
                           "The iOS device cannot start another service instance yet." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::ServiceProhibited ) ) {
        return classified( nativeError, ErrorCategory::Service, "ios-service-prohibited",
                           ErrorScope::Service, RetryPolicy::Never,
                           "The iOS device policy prohibits the requested service." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::EscrowLocked ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-device-locked",
                           ErrorScope::Device, RetryPolicy::AwaitUser,
                           "Unlock the iOS device to continue.", AwaitingUserReason::Unlock );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::PairingProhibited ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-pairing-prohibited",
                           ErrorScope::Device, RetryPolicy::Never,
                           "Pairing is prohibited for this iOS connection." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::FindMyProtected )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown,
                    NativeCode::Lockdown::MobileConfigurationProtected ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-policy-protected",
                           ErrorScope::Device, RetryPolicy::Never,
                           "The iOS device is protected by a configuration policy." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown,
                 NativeCode::Lockdown::MobileConfigurationChallengeRequired ) ) {
        return classified( nativeError, ErrorCategory::Device, "ios-policy-challenge-required",
                           ErrorScope::Device, RetryPolicy::AwaitUser,
                           "Authorize the iOS device policy challenge to continue.",
                           AwaitingUserReason::Authorize );
    }

    if ( isCode( nativeError, IosNativeErrorDomain::Idevice, NativeCode::Idevice::Ssl )
         || isCode( nativeError, IosNativeErrorDomain::Lockdown, NativeCode::Lockdown::Ssl )
         || isCode( nativeError, IosNativeErrorDomain::Service, NativeCode::Service::Ssl )
         || isCode( nativeError, IosNativeErrorDomain::OsTrace, NativeCode::OsTrace::Ssl )
         || isCode( nativeError, IosNativeErrorDomain::SyslogRelay,
                    NativeCode::SyslogRelay::Ssl ) ) {
        return classified( nativeError, ErrorCategory::Backend, "ios-ssl-error",
                           ErrorScope::Service, RetryPolicy::Backoff,
                           "The secure iOS service connection failed." );
    }

    if ( isCode( nativeError, IosNativeErrorDomain::OsTrace,
                 NativeCode::OsTrace::NotEnoughData ) ) {
        return classified( nativeError, ErrorCategory::Stream, "ios-ostrace-truncated-packet",
                           ErrorScope::Stream, RetryPolicy::Backoff,
                           "The iOS trace relay returned a truncated packet." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::OsTrace,
                 NativeCode::OsTrace::RequestFailed ) ) {
        return classified( nativeError, ErrorCategory::Stream, "ios-ostrace-malformed-packet",
                           ErrorScope::Stream, RetryPolicy::Backoff,
                           "The iOS trace relay returned an invalid packet." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::SyslogRelay,
                 NativeCode::SyslogRelay::NotEnoughData ) ) {
        return classified( nativeError, ErrorCategory::Stream, "ios-syslog-truncated-record",
                           ErrorScope::Stream, RetryPolicy::Backoff,
                           "The iOS syslog relay returned a truncated record." );
    }

    if ( isCode( nativeError, IosNativeErrorDomain::Idevice, NativeCode::Idevice::Timeout ) ) {
        return classified( nativeError, ErrorCategory::Stream, "ios-timeout", ErrorScope::Stream,
                           RetryPolicy::Backoff, "The iOS device connection timed out." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::Lockdown, NativeCode::Lockdown::Timeout )
         || isCode( nativeError, IosNativeErrorDomain::Service, NativeCode::Service::Timeout ) ) {
        return classified( nativeError, ErrorCategory::Service, "ios-timeout", ErrorScope::Service,
                           RetryPolicy::Backoff, "The iOS service request timed out." );
    }
    if ( isCode( nativeError, IosNativeErrorDomain::OsTrace, NativeCode::OsTrace::Timeout )
         || isCode( nativeError, IosNativeErrorDomain::SyslogRelay,
                    NativeCode::SyslogRelay::Timeout ) ) {
        return classified( nativeError, ErrorCategory::Stream, "ios-timeout", ErrorScope::Stream,
                           RetryPolicy::Backoff, "The iOS log stream timed out." );
    }

    return classified( nativeError, ErrorCategory::Backend, "ios-native-error", ErrorScope::Service,
                       RetryPolicy::Backoff, "The native iOS backend reported an error." );
}

} // namespace klogg::livecapture::ios
