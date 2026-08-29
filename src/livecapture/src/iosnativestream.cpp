/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "iosnativestream.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "boundedserialexecutor.h"
#include "iosostraceprotocol.h"

namespace klogg::livecapture::ios {
namespace {

constexpr const char* OsTraceService = "com.apple.os_trace_relay";
constexpr const char* SyslogService = "com.apple.syslog_relay";

ClassifiedIosNativeError localError( ErrorCategory category, const char* code, ErrorScope scope,
                                     RetryPolicy retry, const char* message, std::string detail,
                                     std::optional<AwaitingUserReason> reason = std::nullopt )
{
    return { LiveSourceError{ category, code, scope, retry, message, std::move( detail ) },
             reason };
}

std::string nativeDetail( const char* operation, std::int32_t code )
{
    return std::string{ operation } + " failed with native error " + std::to_string( code );
}

std::string sanitizedDecodeDetail( const OsTraceDecodeError& error )
{
    auto detail = std::string{ "os_trace packet decode error " }
                  + std::to_string( static_cast<unsigned>( error.code ) ) + " field "
                  + std::to_string( static_cast<unsigned>( error.field ) );
    if ( !error.structure ) {
        return detail;
    }
    const auto& structure = *error.structure;
    detail += " packet_bytes=" + std::to_string( structure.packetByteCount )
              + " marker=" + std::to_string( structure.marker )
              + " packet_type=" + std::to_string( structure.wirePacketType )
              + " header_bytes=" + std::to_string( structure.declaredHeaderByteCount )
              + " spans=" + std::to_string( structure.fieldLengths[ 0u ] ) + ","
              + std::to_string( structure.fieldLengths[ 1u ] ) + ","
              + std::to_string( structure.fieldLengths[ 2u ] ) + ","
              + std::to_string( structure.fieldLengths[ 3u ] ) + ","
              + std::to_string( structure.fieldLengths[ 4u ] )
              + " declared_span_bytes=" + std::to_string( structure.declaredSpanByteCount )
              + " available_span_bytes=" + std::to_string( structure.availableVariableByteCount );
    return detail;
}

bool usesOsTrace( const IosNativeStreamConfig& config, const std::string& productVersion )
{
    if ( config.servicePolicy == IosNativeServicePolicy::OsTrace ) {
        return true;
    }
    if ( config.servicePolicy == IosNativeServicePolicy::LegacySyslog ) {
        return false;
    }

    std::size_t parsed = 0u;
    while ( parsed < productVersion.size() && productVersion[ parsed ] >= '0'
            && productVersion[ parsed ] <= '9' ) {
        ++parsed;
    }
    if ( parsed == 0u ) {
        return true;
    }
    try {
        return std::stoul( productVersion.substr( 0u, parsed ) ) >= 9u;
    } catch ( ... ) {
        return true;
    }
}

} // namespace

struct IosNativeStreamWorker::State final : public std::enable_shared_from_this<State> {
    State( IosNativeApi nativeApi, IosNativeStreamExecutor streamExecutor,
           IosNativeStreamConfig streamConfig, IosNativeStreamCallbacks streamCallbacks,
           IosNativeSessionLease streamAdmissionLease )
        : api( nativeApi )
        , executor( std::move( streamExecutor ) )
        , config( std::move( streamConfig ) )
        , callbacks( std::move( streamCallbacks ) )
        , admissionLease( std::move( streamAdmissionLease ) )
        , queue( config.queueLimits, config.generation, [ this ] { publishBytesAvailable(); } )
    {
    }

    struct CallbackGuard {
        explicit CallbackGuard( State* value ) noexcept
        {
            try {
                if ( value != nullptr ) {
                    state = value->shared_from_this();
                    active = state->beginCallback();
                }
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
            }
        }

        ~CallbackGuard()
        {
            if ( active ) {
                state->endCallback();
            }
        }

        std::shared_ptr<State> state;
        bool active{ false };
    };

    bool beginCallback() noexcept
    {
        std::lock_guard<std::mutex> lock( controlMutex );
        if ( retired || terminal || !acceptingCallbacks ) {
            return false;
        }
        ++callbacksInFlight;
        return true;
    }

    void endCallback() noexcept
    {
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            if ( callbacksInFlight != 0u ) {
                --callbacksInFlight;
            }
        }
        callbacksChanged.notify_all();
    }

    bool cancelled() const noexcept
    {
        std::lock_guard<std::mutex> lock( controlMutex );
        return retired;
    }

    void publishReady() noexcept
    {
        CallbackGuard guard( this );
        if ( !guard.active ) {
            return;
        }
        try {
            if ( callbacks.ready ) {
                callbacks.ready( config.generation );
            }
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }
    }

    void publishBytesAvailable() const noexcept
    {
        try {
            if ( callbacks.bytesAvailable ) {
                callbacks.bytesAvailable( config.generation );
            }
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }
    }

    bool scheduleCleanup() noexcept
    {
        {
            std::unique_lock<std::mutex> lock( controlMutex );
            if ( cleanupRunning ) {
                return true;
            }
            callbacksChanged.wait( lock, [ this ] { return !cleanupDispatching; } );
            if ( cleanupScheduled ) {
                return true;
            }
            cleanupScheduled = true;
            cleanupDispatching = true;
        }

        bool queued = false;
        try {
            const auto state = shared_from_this();
            executor( [ state ] { state->runCleanup(); } );
            queued = true;
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }

        {
            std::lock_guard<std::mutex> lock( controlMutex );
            cleanupDispatching = false;
            if ( !queued ) {
                cleanupScheduled = false;
            }
        }
        callbacksChanged.notify_all();
        return queued;
    }

    void publishFailure( const ClassifiedIosNativeError& failure ) noexcept
    {
        bool shouldPublish = false;
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            if ( !terminal && !retired ) {
                terminal = true;
                acceptingCallbacks = false;
                shouldPublish = true;
            }
        }
        if ( !shouldPublish ) {
            return;
        }
        const auto state = weak_from_this().lock();
        if ( state == nullptr ) {
            return;
        }
        try {
            if ( state->callbacks.failed ) {
                state->callbacks.failed( state->config.generation, failure );
            }
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }
        if ( !state->scheduleCleanup() ) {
            state->publishStopped();
        }
    }

    void publishBoundaryFailure( ErrorCategory category, const char* code, ErrorScope scope,
                                 RetryPolicy retry, const char* message,
                                 const char* detail ) noexcept
    {
        try {
            publishFailure( localError( category, code, scope, retry, message, detail ) );
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }
    }

    void publishStopped() noexcept
    {
        bool shouldPublish = false;
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            if ( !stoppedPublished ) {
                stoppedPublished = true;
                shouldPublish = true;
            }
        }
        if ( !shouldPublish ) {
            return;
        }
        try {
            if ( callbacks.stopped ) {
                callbacks.stopped( config.generation );
            }
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }
    }

    void enqueue( std::vector<std::uint8_t> bytes ) noexcept
    {
        LiveDataEnqueueResult result = LiveDataEnqueueResult::Closed;
        try {
            result = queue.tryEnqueue( LiveDataChunk{ config.generation, std::move( bytes ) } );
        } catch ( ... ) {
            publishBoundaryFailure( ErrorCategory::Backend, "ios-live-queue-failed",
                                    ErrorScope::Stream, RetryPolicy::Backoff,
                                    "The iOS live-data queue failed.",
                                    "Unable to allocate queued iOS log bytes." );
            return;
        }
        if ( result == LiveDataEnqueueResult::Backpressure ) {
            publishBoundaryFailure( ErrorCategory::Stream, "ios-live-queue-saturated",
                                    ErrorScope::Stream, RetryPolicy::Backoff,
                                    "The iOS live-data queue is saturated.",
                                    "The bounded queue rejected a complete log record." );
        }
    }

    static void osTraceActivity( const void* bytes, std::size_t byteCount, void* context ) noexcept
    {
        auto* const state = static_cast<State*>( context );
        CallbackGuard guard( state );
        if ( !guard.active ) {
            return;
        }
        try {
            if ( bytes == nullptr && byteCount != 0u ) {
                state->publishFailure( localError(
                    ErrorCategory::Stream, "ios-ostrace-malformed-packet", ErrorScope::Stream,
                    RetryPolicy::Backoff, "The iOS trace relay returned a malformed packet.",
                    "The activity callback returned a null borrowed buffer." ) );
                return;
            }
            if ( byteCount > DefaultMaximumOsTraceRecordSize ) {
                state->publishFailure( localError(
                    ErrorCategory::Stream, "ios-ostrace-malformed-packet", ErrorScope::Stream,
                    RetryPolicy::Backoff, "The iOS trace relay returned a malformed packet.",
                    "The borrowed os_trace packet exceeded the 16 MiB limit before copying." ) );
                return;
            }
            const auto* first = static_cast<const std::uint8_t*>( bytes );
            ByteBuffer owned;
            if ( byteCount != 0u ) {
                owned.assign( first, first + byteCount );
            }
            const auto payloadKind = classifyOsTraceCallbackPayload( owned );
            if ( payloadKind == OsTraceCallbackPayloadKind::ControlPlist ) {
                return;
            }
            if ( payloadKind == OsTraceCallbackPayloadKind::Unknown ) {
                state->publishFailure( localError(
                    ErrorCategory::Stream, "ios-ostrace-malformed-packet", ErrorScope::Stream,
                    RetryPolicy::Backoff, "The iOS trace relay returned a malformed packet.",
                    "os_trace callback payload kind unknown packet_bytes="
                        + std::to_string( byteCount ) ) );
                return;
            }
            const auto decoded = decodeOsTracePacket( owned );
            if ( !decoded.record ) {
                const auto detail = decoded.error
                                        ? sanitizedDecodeDetail( *decoded.error )
                                        : std::string{ "os_trace packet decode error unknown" };
                state->publishFailure(
                    localError( ErrorCategory::Stream, "ios-ostrace-malformed-packet",
                                ErrorScope::Stream, RetryPolicy::Backoff,
                                "The iOS trace relay returned a malformed packet.", detail ) );
                return;
            }
            auto formatted = formatOsTraceRecord(
                *decoded.record,
                OsTraceFormatOptions{ state->config.ansiOutputEnabled, true, true } );
            formatted.bytes.push_back( '\n' );
            state->enqueue(
                std::vector<std::uint8_t>( formatted.bytes.begin(), formatted.bytes.end() ) );
        } catch ( ... ) {
            state->publishBoundaryFailure(
                ErrorCategory::Backend, "ios-ostrace-callback-failed", ErrorScope::Stream,
                RetryPolicy::Backoff, "The iOS trace callback failed.",
                "An exception was contained at the native os_trace callback boundary." );
        }
    }

    static void osTraceError( std::int32_t code, void* context ) noexcept
    {
        auto* const state = static_cast<State*>( context );
        CallbackGuard guard( state );
        if ( !guard.active ) {
            return;
        }
        try {
            state->publishFailure( classifyIosNativeError( IosNativeError{
                IosNativeErrorDomain::OsTrace, code, nativeDetail( "os_trace receive", code ) } ) );
        } catch ( ... ) {
            state->publishBoundaryFailure(
                ErrorCategory::Backend, "ios-ostrace-callback-failed", ErrorScope::Stream,
                RetryPolicy::Backoff, "The iOS trace callback failed.",
                "An exception was contained at the native os_trace error callback boundary." );
        }
    }

    static void syslogByte( char byte, void* context ) noexcept
    {
        auto* const state = static_cast<State*>( context );
        CallbackGuard guard( state );
        if ( !guard.active ) {
            return;
        }
        try {
            std::string completedRecord;
            bool recordTooLarge = false;
            {
                std::lock_guard<std::mutex> lock( state->syslogMutex );
                if ( byte != '\0' ) {
                    if ( state->syslogRecord.size() >= state->config.maximumSyslogRecordBytes ) {
                        state->syslogRecord.clear();
                        recordTooLarge = true;
                    }
                    else {
                        state->syslogRecord.push_back( byte );
                    }
                }
                else {
                    state->syslogRecord.push_back( '\n' );
                    completedRecord = std::move( state->syslogRecord );
                    state->syslogRecord.clear();
                }
            }
            if ( recordTooLarge ) {
                state->publishFailure( localError(
                    ErrorCategory::Stream, "ios-syslog-record-too-large", ErrorScope::Stream,
                    RetryPolicy::Backoff, "The iOS syslog relay returned an oversized record.",
                    "An unterminated syslog record exceeded the configured byte limit." ) );
                return;
            }
            if ( !completedRecord.empty() ) {
                std::vector<std::uint8_t> completed( completedRecord.size() );
                std::memcpy( completed.data(), completedRecord.data(), completed.size() );
                state->enqueue( std::move( completed ) );
            }
        } catch ( ... ) {
            state->publishBoundaryFailure(
                ErrorCategory::Backend, "ios-syslog-callback-failed", ErrorScope::Stream,
                RetryPolicy::Backoff, "The iOS syslog callback failed.",
                "An exception was contained at the native syslog callback boundary." );
        }
    }

    static void syslogError( std::int32_t code, void* context ) noexcept
    {
        auto* const state = static_cast<State*>( context );
        CallbackGuard guard( state );
        if ( !guard.active ) {
            return;
        }
        try {
            state->publishFailure( classifyIosNativeError(
                IosNativeError{ IosNativeErrorDomain::SyslogRelay, code,
                                nativeDetail( "syslog receive", code ) } ) );
        } catch ( ... ) {
            state->publishBoundaryFailure(
                ErrorCategory::Backend, "ios-syslog-callback-failed", ErrorScope::Stream,
                RetryPolicy::Backoff, "The iOS syslog callback failed.",
                "An exception was contained at the native syslog error callback boundary." );
        }
    }

    void runStartup() noexcept
    {
        try {
            runStartupImpl();
        } catch ( ... ) {
            try {
                publishFailure( localError(
                    ErrorCategory::Backend, "ios-native-startup-exception", ErrorScope::Stream,
                    RetryPolicy::Backoff, "The native iOS stream worker failed during startup.",
                    "A C++ exception was contained at the native startup task boundary." ) );
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
            }
        }
    }

    void runStartupImpl()
    {
        if ( cancelled() ) {
            return;
        }

        if ( config.endpoint.udid.empty()
             || ( config.endpoint.connectionType != NativeConnectionType::Usb
                  && config.endpoint.connectionType != NativeConnectionType::Network ) ) {
            publishFailure( localError(
                ErrorCategory::Configuration, "ios-endpoint-required", ErrorScope::Device,
                RetryPolicy::Never, "Select a supported iOS device endpoint.",
                "The native worker rejected an empty or unknown endpoint." ) );
            return;
        }

        if ( api.deviceNewWithOptions == nullptr || api.deviceFree == nullptr
             || api.readPairRecord == nullptr || api.pairRecordFree == nullptr
             || api.lockdownClientNewWithExistingPair == nullptr
             || api.lockdownClientFree == nullptr || api.lockdownStartService == nullptr
             || api.serviceDescriptorFree == nullptr || api.lockdownGetStringValue == nullptr
             || api.nativeStringFree == nullptr ) {
            publishFailure( localError( ErrorCategory::Configuration, "ios-native-abi-incomplete",
                                        ErrorScope::Infrastructure, RetryPolicy::Never,
                                        "The bundled native iOS ABI is incomplete.",
                                        "Required native stream symbols are missing." ) );
            return;
        }

        NativeIdevice rawDevice = nullptr;
        const auto deviceCode
            = api.deviceNewWithOptions( &rawDevice, config.endpoint.udid.c_str(),
                                        config.endpoint.connectionType == NativeConnectionType::Usb
                                            ? NativeConnectionOption::Usb
                                            : NativeConnectionOption::Network );
        NativeDeviceOwner openedDevice( api, rawDevice );
        if ( deviceCode != 0 || rawDevice == nullptr ) {
            publishFailure( classifyIosNativeError( IosNativeError{
                IosNativeErrorDomain::Idevice, deviceCode,
                deviceCode != 0 ? nativeDetail( "idevice_new_with_options", deviceCode )
                                : "idevice_new_with_options returned a null device" } ) );
            return;
        }
        if ( cancelled() ) {
            return;
        }

        char* rawPairRecord = nullptr;
        std::uint32_t pairRecordSize = 0u;
        const auto pairResult
            = api.readPairRecord( config.endpoint.udid.c_str(), &rawPairRecord, &pairRecordSize );
        NativePairRecordOwner openedPairRecord( api, rawPairRecord );
        if ( pairResult == NativePairRecordResult::Missing ) {
            publishFailure( localError(
                ErrorCategory::Device, "ios-pair-required", ErrorScope::Device,
                RetryPolicy::AwaitUser, "Pair the iOS device with this computer first.",
                "No host pair record exists for the selected device.", AwaitingUserReason::Pair ) );
            return;
        }
        if ( pairResult != NativePairRecordResult::Present || rawPairRecord == nullptr
             || pairRecordSize == 0u ) {
            publishFailure( localError( ErrorCategory::Backend, "ios-pair-record-read-failed",
                                        ErrorScope::Device, RetryPolicy::Backoff,
                                        "The iOS pair record could not be read.",
                                        "Pair-record preflight did not return owned data." ) );
            return;
        }
        openedPairRecord.reset();
        if ( cancelled() ) {
            return;
        }

        NativeLockdownClient rawLockdown = nullptr;
        const auto handshakeCode
            = api.lockdownClientNewWithExistingPair( rawDevice, &rawLockdown, "klogg" );
        NativeLockdownOwner openedLockdown( api, rawLockdown );
        if ( handshakeCode != 0 || rawLockdown == nullptr ) {
            publishFailure( classifyIosNativeError( IosNativeError{
                IosNativeErrorDomain::Lockdown, handshakeCode,
                handshakeCode != 0 ? nativeDetail( "lockdownd passive handshake", handshakeCode )
                                   : "lockdownd handshake returned a null client" } ) );
            return;
        }
        if ( cancelled() ) {
            return;
        }

        char* rawVersion = nullptr;
        const auto versionCode
            = api.lockdownGetStringValue( rawLockdown, nullptr, "ProductVersion", &rawVersion );
        NativeStringOwner openedVersion( api, rawVersion );
        if ( versionCode != 0 || rawVersion == nullptr ) {
            publishFailure( classifyIosNativeError( IosNativeError{
                IosNativeErrorDomain::Lockdown, versionCode,
                versionCode != 0 ? nativeDetail( "read ProductVersion", versionCode )
                                 : "ProductVersion returned a null string" } ) );
            return;
        }
        const std::string productVersion( rawVersion );
        openedVersion.reset();
        if ( cancelled() ) {
            return;
        }

        const bool osTrace = usesOsTrace( config, productVersion );
        if ( osTrace
             && ( api.osTraceClientNew == nullptr || api.osTraceStart == nullptr
                  || api.osTraceStop == nullptr || api.osTraceClientFree == nullptr ) ) {
            publishFailure( localError( ErrorCategory::Configuration, "ios-native-abi-incomplete",
                                        ErrorScope::Infrastructure, RetryPolicy::Never,
                                        "The bundled native iOS ABI is incomplete.",
                                        "Required os_trace symbols are missing." ) );
            return;
        }
        if ( !osTrace
             && ( api.syslogRelayClientNew == nullptr || api.syslogRelayStart == nullptr
                  || api.syslogRelayStop == nullptr || api.syslogRelayClientFree == nullptr ) ) {
            publishFailure( localError( ErrorCategory::Configuration, "ios-native-abi-incomplete",
                                        ErrorScope::Infrastructure, RetryPolicy::Never,
                                        "The bundled native iOS ABI is incomplete.",
                                        "Required syslog relay symbols are missing." ) );
            return;
        }

        NativeServiceDescriptor rawService = nullptr;
        const auto serviceCode = api.lockdownStartService(
            rawLockdown, osTrace ? OsTraceService : SyslogService, &rawService );
        NativeServiceOwner openedService( api, rawService );
        if ( serviceCode != 0 || rawService == nullptr ) {
            publishFailure( classifyIosNativeError( IosNativeError{
                IosNativeErrorDomain::Lockdown, serviceCode,
                serviceCode != 0 ? nativeDetail( "lockdownd_start_service", serviceCode )
                                 : "lockdownd_start_service returned a null descriptor" } ) );
            return;
        }
        if ( cancelled() ) {
            return;
        }

        NativeOsTraceOwner openedOsTrace;
        NativeSyslogRelayOwner openedSyslog;
        std::int32_t startCode = 0;
        if ( osTrace ) {
            NativeOsTraceClient rawClient = nullptr;
            const auto newCode = api.osTraceClientNew( rawDevice, rawService, &rawClient );
            openedOsTrace = NativeOsTraceOwner( api, rawClient );
            if ( newCode != 0 || rawClient == nullptr ) {
                publishFailure( classifyIosNativeError( IosNativeError{
                    IosNativeErrorDomain::OsTrace, newCode,
                    newCode != 0 ? nativeDetail( "ostrace_client_new", newCode )
                                 : "ostrace_client_new returned a null client" } ) );
                return;
            }
            {
                std::lock_guard<std::mutex> lock( controlMutex );
                acceptingCallbacks = !retired;
            }
            if ( cancelled() ) {
                return;
            }
            startCode = api.osTraceStart( rawClient, &State::osTraceActivity, &State::osTraceError,
                                          this );
        }
        else {
            NativeSyslogRelayClient rawClient = nullptr;
            const auto newCode = api.syslogRelayClientNew( rawDevice, rawService, &rawClient );
            openedSyslog = NativeSyslogRelayOwner( api, rawClient );
            if ( newCode != 0 || rawClient == nullptr ) {
                publishFailure( classifyIosNativeError( IosNativeError{
                    IosNativeErrorDomain::SyslogRelay, newCode,
                    newCode != 0 ? nativeDetail( "syslog_relay_client_new", newCode )
                                 : "syslog_relay_client_new returned a null client" } ) );
                return;
            }
            {
                std::lock_guard<std::mutex> lock( controlMutex );
                acceptingCallbacks = !retired;
            }
            if ( cancelled() ) {
                return;
            }
            startCode
                = api.syslogRelayStart( rawClient, &State::syslogByte, &State::syslogError, this );
        }

        if ( startCode != 0 ) {
            {
                std::lock_guard<std::mutex> lock( controlMutex );
                acceptingCallbacks = false;
            }
            publishFailure( classifyIosNativeError( IosNativeError{
                osTrace ? IosNativeErrorDomain::OsTrace : IosNativeErrorDomain::SyslogRelay,
                startCode,
                nativeDetail( osTrace ? "ostrace_start_activity" : "syslog_relay_start_capture",
                              startCode ) } ) );
            return;
        }
        const auto startedOsTrace = openedOsTrace.get();
        const auto startedSyslog = openedSyslog.get();
        bool committed = false;
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            if ( !retired && !terminal ) {
                device = std::move( openedDevice );
                lockdown = std::move( openedLockdown );
                service = std::move( openedService );
                osTraceClient = std::move( openedOsTrace );
                syslogClient = std::move( openedSyslog );
                streamIsOsTrace = osTrace;
                streamStarted = true;
                committed = true;
            }
            else {
                acceptingCallbacks = false;
            }
        }
        if ( !committed ) {
            {
                std::unique_lock<std::mutex> lock( controlMutex );
                callbacksChanged.wait( lock, [ this ] { return callbacksInFlight == 0u; } );
            }
            if ( osTrace ) {
                api.osTraceStop( startedOsTrace );
            }
            else {
                api.syslogRelayStop( startedSyslog );
            }
            return;
        }
        publishReady();
    }

    void runCleanup() noexcept
    {
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            cleanupRunning = true;
        }
        NativeDeviceOwner closingDevice;
        NativeLockdownOwner closingLockdown;
        NativeServiceOwner closingService;
        NativeOsTraceOwner closingOsTrace;
        NativeSyslogRelayOwner closingSyslog;
        bool stopOsTrace = false;
        bool stopSyslog = false;
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            acceptingCallbacks = false;
            stopOsTrace = streamStarted && streamIsOsTrace && osTraceClient.get() != nullptr;
            stopSyslog = streamStarted && !streamIsOsTrace && syslogClient.get() != nullptr;
            closingOsTrace = std::move( osTraceClient );
            closingSyslog = std::move( syslogClient );
            closingService = std::move( service );
            closingLockdown = std::move( lockdown );
            closingDevice = std::move( device );
            streamStarted = false;
        }

        {
            std::unique_lock<std::mutex> lock( controlMutex );
            // Public stop is asynchronous, so cleanup waits until every entered
            // callback body reaches its exit barrier. The subsequent vendor
            // stop/join supplies full native callback-return quiescence before
            // any callback context or client is freed.
            callbacksChanged.wait( lock, [ this ] { return callbacksInFlight == 0u; } );
        }

        if ( stopOsTrace && api.osTraceStop != nullptr ) {
            api.osTraceStop( closingOsTrace.get() );
        }
        if ( stopSyslog && api.syslogRelayStop != nullptr ) {
            api.syslogRelayStop( closingSyslog.get() );
        }

        // One-shot ownership order is deliberate: callback completion first,
        // then stop/join, stream client, service descriptor, lockdownd client,
        // and finally idevice.
        closingOsTrace.reset();
        closingSyslog.reset();
        closingService.reset();
        closingLockdown.reset();
        closingDevice.reset();
        queue.close();
        publishStopped();
        admissionLease.reset();
    }

    IosNativeApi api{};
    IosNativeStreamExecutor executor;
    IosNativeStreamConfig config;
    IosNativeStreamCallbacks callbacks;
    IosNativeSessionLease admissionLease;
    LiveDataQueue queue;

    mutable std::mutex controlMutex;
    std::condition_variable callbacksChanged;
    std::mutex syslogMutex;
    std::string syslogRecord;
    NativeDeviceOwner device;
    NativeLockdownOwner lockdown;
    NativeServiceOwner service;
    NativeOsTraceOwner osTraceClient;
    NativeSyslogRelayOwner syslogClient;
    std::size_t callbacksInFlight{ 0u };
    bool startScheduled{ false };
    bool cleanupDispatching{ false };
    bool cleanupScheduled{ false };
    bool cleanupRunning{ false };
    bool acceptingCallbacks{ false };
    bool streamStarted{ false };
    bool streamIsOsTrace{ false };
    bool retired{ false };
    bool terminal{ false };
    bool stoppedPublished{ false };
};

IosNativeStreamWorker::IosNativeStreamWorker( IosNativeApi api, IosNativeStreamExecutor executor,
                                              IosNativeStreamConfig config,
                                              IosNativeStreamCallbacks callbacks,
                                              IosNativeSessionLease admissionLease )
    : state_( std::make_shared<State>( api, std::move( executor ), std::move( config ),
                                       std::move( callbacks ), std::move( admissionLease ) ) )
{
}

IosNativeStreamWorker::~IosNativeStreamWorker()
{
    shutdown();
}

bool IosNativeStreamWorker::start()
{
    const auto state = state_;
    if ( state == nullptr || !state->executor ) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock( state->controlMutex );
        if ( state->startScheduled || state->retired ) {
            return false;
        }
        state->startScheduled = true;
    }
    try {
        state->executor( [ state ] { state->runStartup(); } );
    } catch ( ... ) {
        std::lock_guard<std::mutex> lock( state->controlMutex );
        state->startScheduled = false;
        return false;
    }
    return true;
}

void IosNativeStreamWorker::stop( Generation generation ) noexcept
{
    const auto state = state_;
    if ( state == nullptr || generation != state->config.generation ) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock( state->controlMutex );
        state->retired = true;
        state->acceptingCallbacks = false;
    }
    if ( !state->scheduleCleanup() ) {
        state->publishStopped();
    }
}

void IosNativeStreamWorker::shutdown() noexcept
{
    const auto state = state_;
    if ( state != nullptr ) {
        stop( state->config.generation );
    }
}

std::optional<LiveDataBatch> IosNativeStreamWorker::drain()
{
    return state_ != nullptr ? state_->queue.drain() : std::nullopt;
}

LiveDataStatistics IosNativeStreamWorker::statistics() const
{
    if ( state_ != nullptr ) {
        return state_->queue.statistics();
    }
    return {};
}

IosNativeSessionLease::IosNativeSessionLease( std::shared_ptr<void> token )
    : token_( std::move( token ) )
{
}

void IosNativeSessionLease::reset() noexcept
{
    token_.reset();
}

IosNativeSessionLease::operator bool() const noexcept
{
    return token_ != nullptr;
}

struct DefaultIosNativeStreamWorkerFactory::AdmissionState final
    : public std::enable_shared_from_this<AdmissionState> {
    explicit AdmissionState( std::size_t maximumConcurrentSessions )
        : maximumConcurrentSessions( maximumConcurrentSessions )
    {
    }

    struct Lease final {
        Lease( std::shared_ptr<AdmissionState> owner, IosEndpointKey endpoint )
            : owner( std::move( owner ) )
            , endpoint( std::move( endpoint ) )
        {
        }

        ~Lease()
        {
            owner->release( endpoint );
        }

        std::shared_ptr<AdmissionState> owner;
        IosEndpointKey endpoint;
    };

    std::shared_ptr<void> tryAcquire( const IosEndpointKey& endpoint, bool& endpointBusy )
    {
        std::lock_guard<std::mutex> lock( mutex );
        endpointBusy = std::find( activeEndpoints.cbegin(), activeEndpoints.cend(), endpoint )
                       != activeEndpoints.cend();
        if ( endpointBusy || activeEndpoints.size() >= maximumConcurrentSessions ) {
            return nullptr;
        }
        activeEndpoints.push_back( endpoint );
        try {
            return std::make_shared<Lease>( shared_from_this(), endpoint );
        } catch ( ... ) {
            activeEndpoints.pop_back();
            throw;
        }
    }

    void release( const IosEndpointKey& endpoint )
    {
        std::lock_guard<std::mutex> lock( mutex );
        const auto found = std::find( activeEndpoints.cbegin(), activeEndpoints.cend(), endpoint );
        if ( found != activeEndpoints.cend() ) {
            activeEndpoints.erase( found );
        }
    }

    const std::size_t maximumConcurrentSessions;
    std::mutex mutex;
    std::vector<IosEndpointKey> activeEndpoints;
};

namespace {

class OwnedIosNativeStreamSession final : public IosNativeStreamSession {
public:
    // IosNativeApi is a small function-pointer table copied into the worker.
    // cppcheck-suppress passedByValue
    OwnedIosNativeStreamSession( IosNativeApi api, IosNativeStreamConfig config,
                                 IosNativeStreamCallbacks callbacks,
                                 IosNativeSessionLease admissionLease )
        : executor_( std::make_shared<BoundedSerialExecutor>( config.cleanupDeadline ) )
        , worker_(
              api,
              [ executor = executor_ ]( IosNativeStreamTask task ) {
                  if ( !executor->postBeforeFinished( std::move( task ) ) ) {
                      throw std::runtime_error( "native iOS executor is stopping" );
                  }
              },
              std::move( config ), std::move( callbacks ), std::move( admissionLease ) )
    {
    }

    ~OwnedIosNativeStreamSession() override
    {
        worker_.shutdown();
        // Cleanup already queued by shutdown retains the executor state. Detach
        // its thread so destroying this wrapper never consumes the bounded wait
        // deadline on a caller such as the Qt GUI thread.
        executor_->shutdownAsync();
    }

    bool start() override
    {
        return worker_.start();
    }
    void stop( Generation generation ) noexcept override
    {
        worker_.stop( generation );
    }
    void shutdown() noexcept override
    {
        worker_.shutdown();
    }
    std::optional<LiveDataBatch> drain() override
    {
        return worker_.drain();
    }
    LiveDataStatistics statistics() const override
    {
        return worker_.statistics();
    }

private:
    std::shared_ptr<BoundedSerialExecutor> executor_;
    IosNativeStreamWorker worker_;
};

} // namespace

// IosNativeApi is a small function-pointer table copied into the factory.
// cppcheck-suppress passedByValue
DefaultIosNativeStreamWorkerFactory::DefaultIosNativeStreamWorkerFactory(
    IosNativeApi api, std::size_t maximumConcurrentSessions )
    : api_( api )
    , admission_( std::make_shared<AdmissionState>( maximumConcurrentSessions ) )
{
}

IosNativeStreamSessionCreation
DefaultIosNativeStreamWorkerFactory::create( const IosNativeStreamConfig& config,
                                             IosNativeStreamCallbacks callbacks ) const
{
    bool endpointBusy = false;
    auto lease = admission_->tryAcquire( config.endpoint, endpointBusy );
    if ( lease == nullptr ) {
        return IosNativeStreamSessionCreation{
            nullptr,
            endpointBusy
                ? localError( ErrorCategory::Backend, "ios-native-endpoint-busy",
                              ErrorScope::Device, RetryPolicy::Backoff,
                              "The selected iOS endpoint already has an active native session.",
                              "Per-endpoint native session admission is already occupied." )
                : localError( ErrorCategory::Backend, "ios-native-session-capacity-exhausted",
                              ErrorScope::Infrastructure, RetryPolicy::Backoff,
                              "Native iOS session capacity is currently occupied.",
                              "The global native session admission limit was reached." )
        };
    }
    return IosNativeStreamSessionCreation{
        std::make_unique<OwnedIosNativeStreamSession>(
            api_, config, std::move( callbacks ),
            IosNativeSessionLease{ std::move( lease ) } ),
        std::nullopt
    };
}

} // namespace klogg::livecapture::ios
