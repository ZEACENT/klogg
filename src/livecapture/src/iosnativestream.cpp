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
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

class SerialExecutor final {
public:
    explicit SerialExecutor( std::chrono::milliseconds shutdownDeadline )
        : state_( std::make_shared<State>() )
        , shutdownDeadline_( shutdownDeadline )
        , thread_( [ state = state_ ] { run( state ); } )
    {
    }

    ~SerialExecutor()
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            state_->stopping = true;
        }
        state_->changed.notify_all();
        if ( !thread_.joinable() ) {
            return;
        }
        if ( thread_.get_id() == std::this_thread::get_id() ) {
            // The worker loop owns State independently, so a callback may retire
            // the final session on this thread without self-joining or UAF.
            thread_.detach();
            return;
        }

        bool finished = false;
        {
            std::unique_lock<std::mutex> lock( state_->mutex );
            const auto deadline = std::max( shutdownDeadline_, std::chrono::milliseconds::zero() );
            finished
                = state_->changed.wait_for( lock, deadline, [ this ] { return state_->finished; } );
        }
        if ( finished ) {
            thread_.join();
        }
        else {
            // Tasks capture every native owner through shared State. Detaching after
            // the public deadline is therefore safe: the blocked callback/cleanup
            // can finish later without retaining this session or its caller thread.
            thread_.detach();
        }
    }

    void post( IosNativeStreamTask task )
    {
        {
            std::lock_guard<std::mutex> lock( state_->mutex );
            if ( state_->stopping ) {
                return;
            }
            state_->tasks.push_back( std::move( task ) );
        }
        state_->changed.notify_one();
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<IosNativeStreamTask> tasks;
        bool stopping{ false };
        bool finished{ false };
    };

    static void run( const std::shared_ptr<State>& state ) noexcept
    {
        for ( ;; ) {
            IosNativeStreamTask task;
            {
                std::unique_lock<std::mutex> lock( state->mutex );
                state->changed.wait( lock,
                                     [ & ] { return state->stopping || !state->tasks.empty(); } );
                if ( state->tasks.empty() ) {
                    if ( state->stopping ) {
                        break;
                    }
                    continue;
                }
                task = std::move( state->tasks.front() );
                state->tasks.pop_front();
            }
            try {
                task();
            } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                // Native worker tasks are an exception boundary. A failed observer or
                // allocation must never terminate the application worker thread.
            }
        }
        {
            std::lock_guard<std::mutex> lock( state->mutex );
            state->finished = true;
        }
        state->changed.notify_all();
    }

    std::shared_ptr<State> state_;
    std::chrono::milliseconds shutdownDeadline_;
    std::thread thread_;
};

} // namespace

struct IosNativeStreamWorker::State final : public std::enable_shared_from_this<State> {
    State( IosNativeApi nativeApi, IosNativeStreamExecutor streamExecutor,
           IosNativeStreamConfig streamConfig, IosNativeStreamCallbacks streamCallbacks )
        : api( nativeApi )
        , executor( std::move( streamExecutor ) )
        , config( std::move( streamConfig ) )
        , callbacks( std::move( streamCallbacks ) )
        , queue( config.queueLimits, config.generation, [ this ] { publishBytesAvailable(); } )
    {
    }

    struct CallbackGuard {
        explicit CallbackGuard( State* value ) noexcept
            : state( value )
            , active( value != nullptr && value->beginCallback() )
        {
        }

        ~CallbackGuard()
        {
            if ( active ) {
                state->endCallback();
            }
        }

        State* state;
        bool active;
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

    void publishFailure( const ClassifiedIosNativeError& failure ) noexcept
    {
        bool shouldPublish = false;
        bool shouldScheduleCleanup = false;
        {
            std::lock_guard<std::mutex> lock( controlMutex );
            if ( !terminal && !retired ) {
                terminal = true;
                acceptingCallbacks = false;
                shouldPublish = true;
                if ( !cleanupScheduled ) {
                    cleanupScheduled = true;
                    shouldScheduleCleanup = true;
                }
            }
        }
        if ( !shouldPublish ) {
            return;
        }
        try {
            if ( callbacks.failed ) {
                callbacks.failed( config.generation, failure );
            }
        } catch ( ... ) { // NOLINT(bugprone-empty-catch)
        }
        if ( shouldScheduleCleanup ) {
            try {
                const auto state = shared_from_this();
                executor( [ state ] { state->runCleanup(); } );
            } catch ( ... ) {
                publishStopped();
            }
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
            std::vector<std::uint8_t> completed;
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
                    completed.assign( state->syslogRecord.begin(), state->syslogRecord.end() );
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
            if ( !completed.empty() ) {
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
            // Public stop is asynchronous, so cleanup can honor the stronger
            // lifetime invariant: never stop/free a callback context until every
            // callback that already entered has returned. The vendor receive
            // timeout bounds the subsequent stop/join of a partial frame.
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
    }

    IosNativeApi api{};
    IosNativeStreamExecutor executor;
    IosNativeStreamConfig config;
    IosNativeStreamCallbacks callbacks;
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
    bool cleanupScheduled{ false };
    bool acceptingCallbacks{ false };
    bool streamStarted{ false };
    bool streamIsOsTrace{ false };
    bool retired{ false };
    bool terminal{ false };
    bool stoppedPublished{ false };
};

IosNativeStreamWorker::IosNativeStreamWorker( IosNativeApi api, IosNativeStreamExecutor executor,
                                              IosNativeStreamConfig config,
                                              IosNativeStreamCallbacks callbacks )
    : state_( std::make_shared<State>( api, std::move( executor ), std::move( config ),
                                       std::move( callbacks ) ) )
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
        if ( state->cleanupScheduled ) {
            return;
        }
        state->retired = true;
        state->acceptingCallbacks = false;
        state->cleanupScheduled = true;
    }
    try {
        state->executor( [ state ] { state->runCleanup(); } );
    } catch ( ... ) {
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

namespace {

class OwnedIosNativeStreamSession final : public IosNativeStreamSession {
public:
    OwnedIosNativeStreamSession( IosNativeApi api, IosNativeStreamConfig config,
                                 IosNativeStreamCallbacks callbacks )
        : executor_( std::make_shared<SerialExecutor>( config.cleanupDeadline ) )
        , worker_(
              api,
              [ executor
                = std::weak_ptr<SerialExecutor>( executor_ ) ]( IosNativeStreamTask task ) {
                  if ( const auto owned = executor.lock() ) {
                      owned->post( std::move( task ) );
                  }
              },
              std::move( config ), std::move( callbacks ) )
    {
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
    std::shared_ptr<SerialExecutor> executor_;
    IosNativeStreamWorker worker_;
};

} // namespace

DefaultIosNativeStreamWorkerFactory::DefaultIosNativeStreamWorkerFactory( IosNativeApi api )
    : api_( api )
{
}

std::unique_ptr<IosNativeStreamSession>
DefaultIosNativeStreamWorkerFactory::create( const IosNativeStreamConfig& config,
                                             IosNativeStreamCallbacks callbacks ) const
{
    return std::make_unique<OwnedIosNativeStreamSession>( api_, config, std::move( callbacks ) );
}

} // namespace klogg::livecapture::ios
