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

#include <catch2/catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "iosnativeerrors.h"
#include "livestate.h"

namespace {
using namespace klogg::livecapture;
using namespace klogg::livecapture::ios;

struct ErrorCase {
    const char* name;
    IosNativeErrorDomain domain;
    std::int32_t nativeCode;
    ErrorCategory category;
    const char* stableCode;
    ErrorScope scope;
    RetryPolicy retryPolicy;
    std::optional<AwaitingUserReason> awaitingUserReason;
};

constexpr std::array MappingCases{
    ErrorCase{ "lockdownd trust dialog pending", IosNativeErrorDomain::Lockdown, -19,
               ErrorCategory::Device, "ios-trust-pending", ErrorScope::Device,
               RetryPolicy::AwaitUser, AwaitingUserReason::Trust },
    ErrorCase{ "lockdownd trust denied", IosNativeErrorDomain::Lockdown, -18, ErrorCategory::Device,
               "ios-trust-denied", ErrorScope::Device, RetryPolicy::AwaitUser,
               AwaitingUserReason::Trust },
    ErrorCase{ "lockdownd password protected", IosNativeErrorDomain::Lockdown, -17,
               ErrorCategory::Device, "ios-device-locked", ErrorScope::Device,
               RetryPolicy::AwaitUser, AwaitingUserReason::Unlock },
    ErrorCase{ "lockdownd missing pair record", IosNativeErrorDomain::Lockdown, -29,
               ErrorCategory::Device, "ios-stale-pair", ErrorScope::Device, RetryPolicy::AwaitUser,
               AwaitingUserReason::Pair },
    ErrorCase{ "lockdownd invalid pair record", IosNativeErrorDomain::Lockdown, -31,
               ErrorCategory::Device, "ios-stale-pair", ErrorScope::Device, RetryPolicy::AwaitUser,
               AwaitingUserReason::Pair },
    ErrorCase{ "lockdownd missing host id", IosNativeErrorDomain::Lockdown, -20,
               ErrorCategory::Device, "ios-stale-pair", ErrorScope::Device, RetryPolicy::AwaitUser,
               AwaitingUserReason::Pair },
    ErrorCase{ "lockdownd invalid host id", IosNativeErrorDomain::Lockdown, -21,
               ErrorCategory::Device, "ios-stale-pair", ErrorScope::Device, RetryPolicy::AwaitUser,
               AwaitingUserReason::Pair },

    ErrorCase{ "idevice no device", IosNativeErrorDomain::Idevice, -3, ErrorCategory::Device,
               "ios-device-disconnected", ErrorScope::Device, RetryPolicy::WaitForDevice,
               std::nullopt },
    ErrorCase{ "lockdownd mux loss", IosNativeErrorDomain::Lockdown, -8, ErrorCategory::Device,
               "ios-device-disconnected", ErrorScope::Device, RetryPolicy::WaitForDevice,
               std::nullopt },
    ErrorCase{ "service mux loss", IosNativeErrorDomain::Service, -3, ErrorCategory::Device,
               "ios-device-disconnected", ErrorScope::Device, RetryPolicy::WaitForDevice,
               std::nullopt },
    ErrorCase{ "os trace mux loss", IosNativeErrorDomain::OsTrace, -2, ErrorCategory::Device,
               "ios-device-disconnected", ErrorScope::Device, RetryPolicy::WaitForDevice,
               std::nullopt },
    ErrorCase{ "syslog relay mux loss", IosNativeErrorDomain::SyslogRelay, -2,
               ErrorCategory::Device, "ios-device-disconnected", ErrorScope::Device,
               RetryPolicy::WaitForDevice, std::nullopt },

    ErrorCase{ "requested service missing", IosNativeErrorDomain::Lockdown, -26,
               ErrorCategory::Service, "ios-service-missing", ErrorScope::Service,
               RetryPolicy::Never, std::nullopt },
    ErrorCase{ "invalid requested service", IosNativeErrorDomain::Lockdown, -27,
               ErrorCategory::Service, "ios-service-missing", ErrorScope::Service,
               RetryPolicy::Never, std::nullopt },
    ErrorCase{ "service instance limit", IosNativeErrorDomain::Lockdown, -28,
               ErrorCategory::Service, "ios-service-limit", ErrorScope::Service,
               RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "service prohibited by policy", IosNativeErrorDomain::Lockdown, -34,
               ErrorCategory::Service, "ios-service-prohibited", ErrorScope::Service,
               RetryPolicy::Never, std::nullopt },
    ErrorCase{ "escrow bag locked", IosNativeErrorDomain::Lockdown, -35, ErrorCategory::Device,
               "ios-device-locked", ErrorScope::Device, RetryPolicy::AwaitUser,
               AwaitingUserReason::Unlock },
    ErrorCase{ "pairing prohibited on connection", IosNativeErrorDomain::Lockdown, -36,
               ErrorCategory::Device, "ios-pairing-prohibited", ErrorScope::Device,
               RetryPolicy::Never, std::nullopt },
    ErrorCase{ "find-my protection policy", IosNativeErrorDomain::Lockdown, -37,
               ErrorCategory::Device, "ios-policy-protected", ErrorScope::Device,
               RetryPolicy::Never, std::nullopt },
    ErrorCase{ "mobile configuration policy", IosNativeErrorDomain::Lockdown, -38,
               ErrorCategory::Device, "ios-policy-protected", ErrorScope::Device,
               RetryPolicy::Never, std::nullopt },
    ErrorCase{ "mobile configuration challenge", IosNativeErrorDomain::Lockdown, -39,
               ErrorCategory::Device, "ios-policy-challenge-required", ErrorScope::Device,
               RetryPolicy::AwaitUser, AwaitingUserReason::Authorize },

    ErrorCase{ "idevice ssl", IosNativeErrorDomain::Idevice, -6, ErrorCategory::Backend,
               "ios-ssl-error", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "lockdownd ssl", IosNativeErrorDomain::Lockdown, -5, ErrorCategory::Backend,
               "ios-ssl-error", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "service ssl", IosNativeErrorDomain::Service, -4, ErrorCategory::Backend,
               "ios-ssl-error", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "os trace ssl", IosNativeErrorDomain::OsTrace, -3, ErrorCategory::Backend,
               "ios-ssl-error", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "os trace truncated packet", IosNativeErrorDomain::OsTrace, -4,
               ErrorCategory::Stream, "ios-ostrace-truncated-packet", ErrorScope::Stream,
               RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "os trace invalid packet", IosNativeErrorDomain::OsTrace, -7, ErrorCategory::Stream,
               "ios-ostrace-malformed-packet", ErrorScope::Stream, RetryPolicy::Backoff,
               std::nullopt },
    ErrorCase{ "syslog relay ssl", IosNativeErrorDomain::SyslogRelay, -3, ErrorCategory::Backend,
               "ios-ssl-error", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "syslog relay truncated record", IosNativeErrorDomain::SyslogRelay, -4,
               ErrorCategory::Stream, "ios-syslog-truncated-record", ErrorScope::Stream,
               RetryPolicy::Backoff, std::nullopt },

    ErrorCase{ "idevice timeout", IosNativeErrorDomain::Idevice, -7, ErrorCategory::Stream,
               "ios-timeout", ErrorScope::Stream, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "lockdownd timeout", IosNativeErrorDomain::Lockdown, -7, ErrorCategory::Service,
               "ios-timeout", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "service timeout", IosNativeErrorDomain::Service, -7, ErrorCategory::Service,
               "ios-timeout", ErrorScope::Service, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "os trace timeout", IosNativeErrorDomain::OsTrace, -5, ErrorCategory::Stream,
               "ios-timeout", ErrorScope::Stream, RetryPolicy::Backoff, std::nullopt },
    ErrorCase{ "syslog relay timeout", IosNativeErrorDomain::SyslogRelay, -5, ErrorCategory::Stream,
               "ios-timeout", ErrorScope::Stream, RetryPolicy::Backoff, std::nullopt },
};
} // namespace

TEST_CASE( "native iOS errors map exactly to source-neutral errors and awaiting-user gates",
           "[livecapture][ios][error][mapping]" )
{
    for ( const auto& expected : MappingCases ) {
        INFO( expected.name );
        const std::string nativeDetail = std::string{ expected.name } + " native detail";
        const auto actual = classifyIosNativeError(
            IosNativeError{ expected.domain, expected.nativeCode, nativeDetail } );

        CHECK( actual.error.category == expected.category );
        CHECK( actual.error.code == expected.stableCode );
        CHECK( actual.error.scope == expected.scope );
        CHECK( actual.error.retryPolicy == expected.retryPolicy );
        CHECK_FALSE( actual.error.message.empty() );
        CHECK( actual.error.nativeDetail == nativeDetail );
        CHECK( actual.awaitingUserReason == expected.awaitingUserReason );
    }
}

TEST_CASE( "unknown native iOS errors stay diagnosable and never invent user action",
           "[livecapture][ios][error][mapping]" )
{
    for ( const auto domain : { IosNativeErrorDomain::Idevice, IosNativeErrorDomain::Lockdown,
                                IosNativeErrorDomain::Service, IosNativeErrorDomain::OsTrace,
                                IosNativeErrorDomain::SyslogRelay } ) {
        const auto actual
            = classifyIosNativeError( IosNativeError{ domain, -12345, "opaque native detail" } );
        INFO( "domain=" << static_cast<unsigned>( domain ) );
        CHECK( actual.error.category == ErrorCategory::Backend );
        CHECK( actual.error.code == "ios-native-error" );
        CHECK( actual.error.retryPolicy == RetryPolicy::Backoff );
        CHECK( actual.error.nativeDetail == "opaque native detail" );
        CHECK_FALSE( actual.awaitingUserReason.has_value() );
    }

    const auto nonEnumeratedServiceCode = classifyIosNativeError(
        IosNativeError{ IosNativeErrorDomain::Service, -2, "not a public 1.4.0 service code" } );
    CHECK( nonEnumeratedServiceCode.error.code == "ios-native-error" );
    CHECK_FALSE( nonEnumeratedServiceCode.awaitingUserReason.has_value() );
}
