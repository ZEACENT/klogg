/*
 * Copyright (C) 2026
 *
 * This file is part of klogg.
 *
 * Task 6 cycle 3 RED: deterministic contracts for the source-neutral,
 * per-tab live-log controller. The controller is the only owner of the live
 * reducer snapshot and reducer effects. Tests use an injected monotonic clock
 * and manual scheduler; there are no event-loop waits or real timers.
 */

#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "adbprotocol.h"
#include "livelogcontroller.h"

namespace {

namespace live = klogg::livecapture;
namespace adb = klogg::livecapture::adb;
namespace ios = klogg::livecapture::ios;
namespace livelog = klogg::livelog;
using namespace std::chrono_literals;

constexpr live::Timestamp at( std::int64_t milliseconds )
{
    return live::Timestamp{ milliseconds };
}

livelog::LiveLogSessionSpec androidSpec()
{
    livelog::LiveLogSessionSpec spec;
    spec.captureId = QStringLiteral( "controller-android" );
    spec.sourceKind = livelog::SourceKind::AndroidLogcat;
    spec.androidBackend = livelog::AndroidBackend::SmartSocket;
    spec.device.deviceId = QStringLiteral( "R58NC123ABC" );
    spec.device.displayName = QStringLiteral( "Pixel" );
    spec.runIntent = live::RunIntent::Running;
    // Qt 5 + GCC: brace-list assignment to QStringList is ambiguous.
    spec.android.buffers
        = QStringList{ QStringLiteral( "main" ), QStringLiteral( "system" ),
                       QStringLiteral( "crash" ) };
    spec.android.filterSpec = QStringLiteral( "ActivityManager:I" );
    spec.android.priority = QStringLiteral( "debug" );
    spec.android.pid = 4242;
    spec.capture.ansiOutputEnabled = true;
    spec.capture.autoReconnectEnabled = true;
    spec.capture.maxReconnectAttempts = 3;
    return spec;
}

livelog::LiveLogSessionSpec iosSpec()
{
    livelog::LiveLogSessionSpec spec;
    spec.captureId = QStringLiteral( "controller-ios" );
    spec.sourceKind = livelog::SourceKind::IosSyslog;
    spec.iosBackend = livelog::IosBackend::Native;
    spec.device.deviceId = QStringLiteral( "00008101-001A2B3C4D5E" );
    spec.device.displayName = QStringLiteral( "iPhone" );
    spec.device.connection = livelog::DeviceIdentity::Connection::Network;
    spec.runIntent = live::RunIntent::Running;
    spec.ios.level = QStringLiteral( "debug" );
    spec.ios.categories
        = QStringList{ QStringLiteral( "network" ), QStringLiteral( "signpost" ) };
    spec.ios.subsystem = QStringLiteral( "com.example.app" );
    spec.ios.outputFormat = livelog::IosOptions::OutputFormat::Json;
    spec.capture.autoReconnectEnabled = true;
    spec.capture.maxReconnectAttempts = 3;
    return spec;
}

live::LiveSourceError retryableStreamError( std::string code = "stream-lost" )
{
    return live::LiveSourceError{ live::ErrorCategory::Stream,
                                  std::move( code ),
                                  live::ErrorScope::Stream,
                                  live::RetryPolicy::Backoff,
                                  "The stream was interrupted.",
                                  "deterministic test failure" };
}

live::LiveSourceError outputBindingError()
{
    return live::LiveSourceError{ live::ErrorCategory::Capture,
                                  "output-write-failed",
                                  live::ErrorScope::Capture,
                                  live::RetryPolicy::Never,
                                  "The output file could not be written.",
                                  "disk full" };
}

class ManualClock final : public livelog::LiveLogClock {
public:
    live::Timestamp now() const noexcept override
    {
        return now_;
    }

    void set( live::Timestamp value )
    {
        now_ = value;
    }

private:
    live::Timestamp now_{ 0 };
};

class ManualScheduler final : public livelog::LiveLogScheduler {
public:
    Token schedule( live::Timestamp deadline, std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        entries.push_back( Entry{ token, deadline, std::move( callback ), false } );
        return token;
    }

    void cancel( Token token ) override
    {
        const auto found = find( token );
        if ( found != entries.end() ) {
            found->cancelled = true;
        }
    }

    std::size_t activeCount() const
    {
        return static_cast<std::size_t>( std::count_if(
            entries.cbegin(), entries.cend(), []( const Entry& entry ) { return !entry.cancelled; } ) );
    }

    Token latestToken() const
    {
        REQUIRE_FALSE( entries.empty() );
        return entries.back().token;
    }

    live::Timestamp deadline( Token token ) const
    {
        const auto found = find( token );
        REQUIRE( found != entries.cend() );
        return found->deadline;
    }

    void fire( Token token )
    {
        auto found = find( token );
        REQUIRE( found != entries.end() );
        REQUIRE_FALSE( found->cancelled );
        found->cancelled = true;
        auto callback = std::move( found->callback );
        callback();
    }

    void fireEvenIfCancelled( Token token )
    {
        auto found = find( token );
        REQUIRE( found != entries.end() );
        auto callback = std::move( found->callback );
        callback();
    }

private:
    struct Entry {
        Token token{ 0 };
        live::Timestamp deadline{ 0 };
        std::function<void()> callback;
        bool cancelled{ false };
    };

    std::vector<Entry>::iterator find( Token token )
    {
        return std::find_if( entries.begin(), entries.end(),
                             [ token ]( const Entry& entry ) { return entry.token == token; } );
    }

    std::vector<Entry>::const_iterator find( Token token ) const
    {
        return std::find_if( entries.cbegin(), entries.cend(),
                             [ token ]( const Entry& entry ) { return entry.token == token; } );
    }

public:
    std::vector<Entry> entries;

private:
    Token nextToken_{ 0 };
};

class RecordingEffects final : public livelog::LiveLogControllerEffects {
public:
    enum class Kind : std::uint8_t {
        InvalidateGeneration,
        CancelStream,
        StartInfrastructure,
        OpenStream,
        AppendBytes
    };

    struct Record {
        Kind kind{ Kind::OpenStream };
        live::Generation generation{ 0 };
        LiveSourceTransportConfig config;
        QByteArray bytes;
    };

    void invalidateGeneration( live::Generation generation ) override
    {
        observe( Kind::InvalidateGeneration, generation );
    }

    void cancelStream( live::Generation generation ) override
    {
        observe( Kind::CancelStream, generation );
    }

    void startInfrastructure( live::Generation generation ) override
    {
        observe( Kind::StartInfrastructure, generation );
    }

    void openStream( live::Generation generation,
                     const LiveSourceTransportConfig& config ) override
    {
        observe( Kind::OpenStream, generation, config );
    }

    void appendBytes( live::Generation generation, const QByteArray& bytes ) override
    {
        observe( Kind::AppendBytes, generation, {}, bytes );
    }

    std::size_t count( Kind kind ) const
    {
        return static_cast<std::size_t>( std::count_if(
            records.cbegin(), records.cend(),
            [ kind ]( const Record& record ) { return record.kind == kind; } ) );
    }

    const Record& latest( Kind kind ) const
    {
        const auto found = std::find_if( records.crbegin(), records.crend(),
                                         [ kind ]( const Record& record ) {
                                             return record.kind == kind;
                                         } );
        REQUIRE( found != records.crend() );
        return *found;
    }

    std::function<void( Kind, live::Generation )> beforeRecord;
    std::vector<Record> records;

private:
    void observe( Kind kind, live::Generation generation,
                  LiveSourceTransportConfig config = {}, QByteArray bytes = {} )
    {
        if ( beforeRecord ) {
            beforeRecord( kind, generation );
        }
        records.push_back(
            Record{ kind, generation, std::move( config ), std::move( bytes ) } );
    }
};

livelog::LiveLogControllerConfig controllerConfig()
{
    livelog::LiveLogControllerConfig config;
    config.reducer.maxRetryAttempts = 3u;
    config.reducer.stabilityInterval = 10s;
    config.initialRetryDelay = 1s;
    config.maximumRetryDelay = 30s;
    return config;
}

void armToOpening( livelog::LiveLogController& controller, ManualClock& clock )
{
    controller.armRunIntent();
    clock.set( at( 10 ) );
    controller.infrastructureChanged( live::InfrastructureStatus::Ready,
                                      live::InfrastructureOwnership::ExternalShared );
    const auto generation = controller.snapshot().generation;
    clock.set( at( 20 ) );
    controller.deviceAvailable( generation );
    REQUIRE( controller.snapshot().source.status == live::SourceStatus::OpeningStream );
}

void armToStreaming( livelog::LiveLogController& controller, ManualClock& clock )
{
    armToOpening( controller, clock );
    const auto generation = controller.snapshot().generation;
    clock.set( at( 30 ) );
    controller.protocolServiceReady( generation );
    clock.set( at( 40 ) );
    controller.streamHandleOpened( generation );
    clock.set( at( 50 ) );
    controller.streamReadArmed( generation );
    REQUIRE( controller.snapshot().source.status == live::SourceStatus::Streaming );
}

void checkProjectionMatches( const livelog::LiveLogController& controller )
{
    const auto expected = live::projectLiveState( controller.snapshot() );
    const auto actual = controller.presentation();
    CHECK( actual.status == expected.status );
    CHECK( actual.disconnectEnabled == expected.disconnectEnabled );
    CHECK( actual.reconnectEnabled == expected.reconnectEnabled );
    CHECK( actual.retryCountdownVisible == expected.retryCountdownVisible );
    CHECK( actual.retryRemaining == expected.retryRemaining );
    CHECK( actual.retryAttempt == expected.retryAttempt );
    CHECK( actual.awaitingUserReason == expected.awaitingUserReason );
    CHECK( actual.failureMessage == expected.failureMessage );
    CHECK( actual.outputBinding == expected.outputBinding );
}

} // namespace

TEST_CASE( "Running session intent is reduced before any startup effect executes",
           "[livelog-controller][red][ordering]" )
{
    ManualClock clock;
    clock.set( at( 100 ) );
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );

    effects.beforeRecord = [ &controller ]( RecordingEffects::Kind,
                                            live::Generation generation ) {
        CHECK( controller.snapshot().runIntent == live::RunIntent::Running );
        CHECK( controller.snapshot().generation == generation );
        CHECK( controller.snapshot().source.status
               == live::SourceStatus::WaitingForInfrastructure );
    };

    controller.armRunIntent();

    REQUIRE( effects.records.size() == 2u );
    CHECK( effects.records.at( 0 ).kind == RecordingEffects::Kind::InvalidateGeneration );
    CHECK( effects.records.at( 1 ).kind == RecordingEffects::Kind::StartInfrastructure );
    CHECK( controller.snapshot().now == at( 100 ) );
}

TEST_CASE( "Stopped session intent restores inert and creates no infrastructure effect",
           "[livelog-controller][red][restore]" )
{
    auto spec = iosSpec();
    spec.runIntent = live::RunIntent::Stopped;
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( spec, controllerConfig(), clock, scheduler, effects );

    controller.armRunIntent();

    CHECK( controller.snapshot().runIntent == live::RunIntent::Stopped );
    CHECK( controller.snapshot().source.status == live::SourceStatus::Stopped );
    CHECK( effects.records.empty() );
    CHECK( scheduler.activeCount() == 0u );
}

TEST_CASE( "Infrastructure device service stream and capture callbacks flow through the reducer",
           "[livelog-controller][red][reducer]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );

    auto expected = live::initialLiveState();
    expected = live::reduce( expected, live::StartRequested{ at( 0 ) },
                             controllerConfig().reducer )
                   .snapshot;
    controller.armRunIntent();
    CHECK( controller.snapshot().source.status == expected.source.status );
    CHECK( controller.snapshot().generation == expected.generation );

    clock.set( at( 10 ) );
    const live::InfrastructureChanged infrastructure{ live::InfrastructureStatus::Ready,
                                                      live::InfrastructureOwnership::ExternalShared,
                                                      at( 10 ) };
    expected = live::reduce( expected, infrastructure, controllerConfig().reducer ).snapshot;
    controller.infrastructureChanged( infrastructure.status, infrastructure.ownership );
    CHECK( controller.snapshot().source.status == expected.source.status );

    const auto generation = expected.generation;
    clock.set( at( 20 ) );
    expected = live::reduce( expected, live::DeviceAvailable{ generation, at( 20 ) },
                             controllerConfig().reducer )
                   .snapshot;
    controller.deviceAvailable( generation );
    CHECK( controller.snapshot().source.status == expected.source.status );
    CHECK( effects.latest( RecordingEffects::Kind::OpenStream ).generation == generation );

    clock.set( at( 30 ) );
    expected = live::reduce( expected, live::ProtocolServiceReady{ generation, at( 30 ) },
                             controllerConfig().reducer )
                   .snapshot;
    controller.protocolServiceReady( generation );
    clock.set( at( 40 ) );
    expected = live::reduce( expected, live::StreamHandleOpened{ generation, at( 40 ) },
                             controllerConfig().reducer )
                   .snapshot;
    controller.streamHandleOpened( generation );
    clock.set( at( 50 ) );
    expected = live::reduce( expected, live::StreamReadArmed{ generation, at( 50 ) },
                             controllerConfig().reducer )
                   .snapshot;
    controller.streamReadArmed( generation );
    CHECK( controller.snapshot().source.status == expected.source.status );

    const auto outputError = outputBindingError();
    clock.set( at( 60 ) );
    expected = live::reduce( expected,
                             live::OutputBindingChanged{ live::OutputBindingState::Degraded,
                                                         outputError, at( 60 ) },
                             controllerConfig().reducer )
                   .snapshot;
    controller.outputBindingChanged( live::OutputBindingState::Degraded, outputError );
    CHECK( controller.snapshot().outputBinding == expected.outputBinding );
    CHECK( controller.snapshot().source.status == expected.source.status );
    checkProjectionMatches( controller );
}

TEST_CASE( "Await-user device gates are reducer events, not controller-local status writes",
           "[livelog-controller][red][reducer][await-user]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( iosSpec(), controllerConfig(), clock, scheduler, effects );
    armToOpening( controller, clock );

    const auto before = controller.snapshot();
    const auto generation = before.generation;
    clock.set( at( 25 ) );
    const auto expected = live::reduce(
        before,
        live::UserActionRequired{ generation, live::AwaitingUserReason::Trust, clock.now() },
        controllerConfig().reducer );

    controller.userActionRequired( generation, live::AwaitingUserReason::Trust );

    CHECK( expected.accepted );
    CHECK( controller.snapshot().source.status == expected.snapshot.source.status );
    CHECK( controller.snapshot().source.awaitingUserReason
           == expected.snapshot.source.awaitingUserReason );
    CHECK( controller.snapshot().generation == expected.snapshot.generation );
    CHECK( controller.presentation().status == live::PresentationStatus::AwaitingUser );
}

TEST_CASE( "Device authorization snapshots can gate a waiting live tab",
           "[livelog-controller][red][await-user][device]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( iosSpec(), controllerConfig(), clock, scheduler, effects );

    controller.armRunIntent();
    controller.infrastructureChanged( live::InfrastructureStatus::Ready,
                                      live::InfrastructureOwnership::AppShared );
    const auto generation = controller.snapshot().generation;
    REQUIRE( controller.snapshot().source.status == live::SourceStatus::WaitingForDevice );

    controller.userActionRequired( generation, live::AwaitingUserReason::Trust );

    CHECK( controller.snapshot().source.status == live::SourceStatus::AwaitingUser );
    CHECK( controller.snapshot().source.awaitingUserReason == live::AwaitingUserReason::Trust );
    CHECK( effects.count( RecordingEffects::Kind::OpenStream ) == 0u );

    controller.userActionRequired( generation, live::AwaitingUserReason::Unlock );

    CHECK( controller.snapshot().source.status == live::SourceStatus::AwaitingUser );
    CHECK( controller.snapshot().source.awaitingUserReason == live::AwaitingUserReason::Unlock );
    CHECK( controller.presentation().awaitingUserReason == live::AwaitingUserReason::Unlock );
    CHECK( effects.count( RecordingEffects::Kind::OpenStream ) == 0u );
}

TEST_CASE( "Connecting and Opening never present Connected while an idle armed stream does",
           "[livelog-controller][red][presentation]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( iosSpec(), controllerConfig(), clock, scheduler, effects );

    controller.armRunIntent();
    clock.set( at( 10 ) );
    controller.infrastructureChanged( live::InfrastructureStatus::Connecting,
                                      live::InfrastructureOwnership::AppShared );
    CHECK( controller.presentation().status
           == live::PresentationStatus::WaitingForInfrastructure );
    CHECK( controller.presentation().status != live::PresentationStatus::Connected );

    clock.set( at( 20 ) );
    controller.infrastructureChanged( live::InfrastructureStatus::Ready,
                                      live::InfrastructureOwnership::AppShared );
    const auto generation = controller.snapshot().generation;
    controller.deviceAvailable( generation );
    CHECK( controller.presentation().status == live::PresentationStatus::OpeningStream );
    CHECK( controller.presentation().status != live::PresentationStatus::Connected );

    clock.set( at( 30 ) );
    controller.protocolServiceReady( generation );
    CHECK( controller.presentation().status == live::PresentationStatus::OpeningStream );
    clock.set( at( 40 ) );
    controller.streamHandleOpened( generation );
    CHECK( controller.presentation().status == live::PresentationStatus::OpeningStream );
    clock.set( at( 50 ) );
    controller.streamReadArmed( generation );

    CHECK( controller.snapshot().source.status == live::SourceStatus::Streaming );
    CHECK( controller.presentation().status == live::PresentationStatus::Connected );
    CHECK( effects.count( RecordingEffects::Kind::AppendBytes ) == 0u );
    checkProjectionMatches( controller );
}

TEST_CASE( "Retry countdown belongs to each tab and is driven by injected time",
           "[livelog-controller][red][retry][per-tab]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects firstEffects;
    RecordingEffects secondEffects;
    livelog::LiveLogController first( androidSpec(), controllerConfig(), clock, scheduler,
                                      firstEffects );
    livelog::LiveLogController second( iosSpec(), controllerConfig(), clock, scheduler,
                                       secondEffects );

    armToOpening( first, clock );
    armToOpening( second, clock );
    const auto firstGeneration = first.snapshot().generation;
    const auto secondGeneration = second.snapshot().generation;

    clock.set( at( 1000 ) );
    first.streamFailed( firstGeneration, retryableStreamError( "first" ) );
    const auto firstTimer = scheduler.latestToken();
    CHECK( scheduler.deadline( firstTimer ) == at( 2000 ) );

    clock.set( at( 1400 ) );
    second.streamFailed( secondGeneration, retryableStreamError( "second" ) );
    const auto secondTimer = scheduler.latestToken();
    CHECK( firstTimer != secondTimer );
    CHECK( scheduler.deadline( secondTimer ) == at( 2400 ) );

    clock.set( at( 1700 ) );
    first.refreshPresentationTime();
    second.refreshPresentationTime();
    CHECK( first.presentation().retryRemaining == at( 300 ) );
    CHECK( second.presentation().retryRemaining == at( 700 ) );

    clock.set( at( 2000 ) );
    scheduler.fire( firstTimer );
    CHECK( first.snapshot().source.status == live::SourceStatus::OpeningStream );
    CHECK( second.snapshot().source.status == live::SourceStatus::RetryWait );
    CHECK( second.presentation().retryCountdownVisible );
}

TEST_CASE( "Stable interval resets exponential backoff but first bytes do not",
           "[livelog-controller][red][retry][stability]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );

    armToStreaming( controller, clock );
    const auto firstGeneration = controller.snapshot().generation;
    clock.set( at( 100 ) );
    controller.streamFailed( firstGeneration, retryableStreamError() );
    const auto firstRetry = scheduler.latestToken();
    CHECK( scheduler.deadline( firstRetry ) == at( 1100 ) );

    clock.set( at( 1100 ) );
    scheduler.fire( firstRetry );
    const auto secondGeneration = controller.snapshot().generation;
    controller.protocolServiceReady( secondGeneration );
    controller.streamHandleOpened( secondGeneration );
    controller.streamReadArmed( secondGeneration );
    controller.streamBytesReceived( secondGeneration, QByteArrayLiteral( "one line\n" ) );
    CHECK( controller.snapshot().consecutiveFailures == 1u );

    clock.set( at( 11100 ) );
    controller.streamStable( secondGeneration );
    CHECK( controller.snapshot().consecutiveFailures == 0u );

    clock.set( at( 11200 ) );
    controller.streamFailed( secondGeneration, retryableStreamError() );
    const auto resetRetry = scheduler.latestToken();
    CHECK( scheduler.deadline( resetRetry ) == at( 12200 ) );
}

TEST_CASE( "Retry exhaustion removes the countdown and leaves no active retry callback",
           "[livelog-controller][red][retry][exhaustion]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );

    armToOpening( controller, clock );
    for ( unsigned failure = 1u; failure <= 3u; ++failure ) {
        const auto generation = controller.snapshot().generation;
        clock.set( at( static_cast<std::int64_t>( failure ) * 1000 ) );
        controller.streamFailed( generation, retryableStreamError() );
        if ( failure < 3u ) {
            CHECK( controller.presentation().retryCountdownVisible );
            const auto retry = scheduler.latestToken();
            clock.set( scheduler.deadline( retry ) );
            scheduler.fire( retry );
        }
    }

    CHECK( controller.snapshot().source.status == live::SourceStatus::Failed );
    CHECK_FALSE( controller.snapshot().retryTimer.has_value() );
    CHECK_FALSE( controller.presentation().retryCountdownVisible );
    CHECK( controller.presentation().retryRemaining == at( 0 ) );
    CHECK( scheduler.activeCount() == 0u );
}

TEST_CASE( "Manual reconnect invalidates first and never pre-writes a reconnected marker",
           "[livelog-controller][red][manual-reconnect]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );
    armToStreaming( controller, clock );
    const auto oldGeneration = controller.snapshot().generation;
    effects.records.clear();

    clock.set( at( 500 ) );
    controller.reconnectRequested();

    REQUIRE_FALSE( effects.records.empty() );
    CHECK( effects.records.front().kind == RecordingEffects::Kind::InvalidateGeneration );
    CHECK( effects.count( RecordingEffects::Kind::AppendBytes ) == 0u );
    CHECK( std::none_of( effects.records.cbegin(), effects.records.cend(), []( const auto& record ) {
        return record.bytes.contains( QByteArrayLiteral( "reconnected" ) );
    } ) );
    CHECK( effects.count( RecordingEffects::Kind::CancelStream ) == 1u );
    CHECK( effects.latest( RecordingEffects::Kind::CancelStream ).generation == oldGeneration );
    CHECK( controller.snapshot().runIntent == live::RunIntent::Running );
}

TEST_CASE( "Output binding degradation does not change stream connectivity",
           "[livelog-controller][red][capture][output]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( iosSpec(), controllerConfig(), clock, scheduler, effects );
    armToStreaming( controller, clock );

    clock.set( at( 100 ) );
    controller.outputBindingChanged( live::OutputBindingState::Degraded, outputBindingError() );

    CHECK( controller.snapshot().outputBinding == live::OutputBindingState::Degraded );
    CHECK( controller.snapshot().source.status == live::SourceStatus::Streaming );
    CHECK( controller.presentation().status == live::PresentationStatus::Connected );
    CHECK( controller.presentation().outputBinding == live::OutputBindingState::Degraded );
    REQUIRE( controller.snapshot().outputBindingError.has_value() );
    CHECK( controller.snapshot().outputBindingError->message
           == "The output file could not be written." );
    CHECK( controller.presentation().disconnectEnabled );
}

TEST_CASE( "Typed Android options reach transport config and command on every generation",
           "[livelog-controller][red][android-options]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );

    armToOpening( controller, clock );
    const auto first = effects.latest( RecordingEffects::Kind::OpenStream );
    CHECK( first.config.androidBuffers
           == QStringList{ QStringLiteral( "main" ), QStringLiteral( "system" ),
                           QStringLiteral( "crash" ) } );
    CHECK( first.config.androidFilterSpec == QStringLiteral( "ActivityManager:I" ) );
    CHECK( first.config.androidPriority == QStringLiteral( "debug" ) );
    CHECK( first.config.androidPid == std::optional<int>{ 4242 } );

    const auto backend = livelog::makeAdbSmartSocketTransportConfig( first.config );
    REQUIRE( backend.has_value() );
    const auto service = adb::buildLogcatService( backend->logcatOptions );
    REQUIRE( service.value.has_value() );
    CHECK( *service.value
           == "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec -v color -b main "
              "-b system -b crash --pid 4242 'ActivityManager:I' '*:D'" );

    clock.set( at( 100 ) );
    controller.reconnectRequested();
    const auto reconnectGeneration = controller.snapshot().generation;
    controller.deviceAvailable( reconnectGeneration );
    const auto second = effects.latest( RecordingEffects::Kind::OpenStream );
    CHECK( second.generation == reconnectGeneration );
    CHECK( second.config.androidBuffers == first.config.androidBuffers );
    CHECK( second.config.androidFilterSpec == first.config.androidFilterSpec );
    CHECK( second.config.androidPriority == first.config.androidPriority );
    CHECK( second.config.androidPid == first.config.androidPid );
}

TEST_CASE( "Typed iOS options reach native worker config unchanged on reconnect",
           "[livelog-controller][red][ios-options]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( iosSpec(), controllerConfig(), clock, scheduler, effects );

    armToOpening( controller, clock );
    const auto first = effects.latest( RecordingEffects::Kind::OpenStream );
    CHECK( first.config.iosLevel == QStringLiteral( "debug" ) );
    CHECK( first.config.iosCategories
           == QStringList{ QStringLiteral( "network" ), QStringLiteral( "signpost" ) } );
    CHECK( first.config.iosSubsystem == QStringLiteral( "com.example.app" ) );
    CHECK( first.config.iosJsonOutput );

    const auto worker = livelog::makeIosNativeStreamConfig( first.config );
    REQUIRE( worker.has_value() );
    CHECK( worker->logOptions.level == "debug" );
    CHECK( worker->logOptions.categories
           == std::vector<std::string>{ "network", "signpost" } );
    CHECK( worker->logOptions.subsystem == "com.example.app" );
    CHECK( worker->logOptions.outputFormat == ios::IosLogOutputFormat::Json );

    clock.set( at( 100 ) );
    controller.reconnectRequested();
    const auto reconnectGeneration = controller.snapshot().generation;
    controller.deviceAvailable( reconnectGeneration );
    const auto second = effects.latest( RecordingEffects::Kind::OpenStream );
    CHECK( second.generation == reconnectGeneration );
    CHECK( second.config.iosLevel == first.config.iosLevel );
    CHECK( second.config.iosCategories == first.config.iosCategories );
    CHECK( second.config.iosSubsystem == first.config.iosSubsystem );
    CHECK( second.config.iosJsonOutput == first.config.iosJsonOutput );
}

TEST_CASE( "Malformed Android filter options are rejected before command construction",
           "[livelog-controller][android-options][validation]" )
{
    LiveSourceTransportConfig config;
    config.sourceType = LiveLogSourceType::AdbLogcat;
    config.adbBackend = AdbTransportBackend::SmartSocket;
    config.deviceId = QStringLiteral( "R58NC123ABC" );
    config.androidBuffers = QStringList{ QStringLiteral( "main" ) };
    config.androidFilterSpec = QStringLiteral( "ActivityManager:I\n$(reboot):E" );
    CHECK_FALSE( livelog::makeAdbSmartSocketTransportConfig( config ).has_value() );

    config.androidFilterSpec = QStringLiteral( "ActivityManager:I" );
    config.androidPriority = QStringLiteral( "debug;reboot" );
    CHECK_FALSE( livelog::makeAdbSmartSocketTransportConfig( config ).has_value() );

    config.androidPriority = QStringLiteral( "debug" );
    config.androidBuffers = QStringList{ QStringLiteral( "main;reboot" ) };
    CHECK_FALSE( livelog::makeAdbSmartSocketTransportConfig( config ).has_value() );
}

TEST_CASE( "Stale generation callbacks and cancelled scheduled effects are ignored",
           "[livelog-controller][red][generation]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );
    armToOpening( controller, clock );
    const auto failedGeneration = controller.snapshot().generation;

    clock.set( at( 1000 ) );
    controller.streamFailed( failedGeneration, retryableStreamError() );
    const auto staleTimer = scheduler.latestToken();
    controller.reconnectRequested();
    const auto currentGeneration = controller.snapshot().generation;
    const auto effectCount = effects.records.size();

    controller.protocolServiceReady( failedGeneration );
    controller.streamHandleOpened( failedGeneration );
    controller.streamReadArmed( failedGeneration );
    controller.streamBytesReceived( failedGeneration, QByteArrayLiteral( "stale\n" ) );
    controller.streamFailed( failedGeneration, retryableStreamError( "late-error" ) );
    scheduler.fireEvenIfCancelled( staleTimer );

    CHECK( controller.snapshot().generation == currentGeneration );
    CHECK( controller.snapshot().source.status == live::SourceStatus::WaitingForDevice );
    CHECK( effects.records.size() == effectCount );
    CHECK( effects.count( RecordingEffects::Kind::AppendBytes ) == 0u );
    CHECK_FALSE( controller.presentation().retryCountdownVisible );
}

TEST_CASE( "Reentrant effect callbacks wait until the current effect batch is retired",
           "[livelog-controller][red][reentrant][ordering]" )
{
    ManualClock clock;
    ManualScheduler scheduler;
    RecordingEffects effects;
    livelog::LiveLogController controller( androidSpec(), controllerConfig(), clock, scheduler,
                                           effects );
    armToStreaming( controller, clock );
    const auto oldGeneration = controller.snapshot().generation;
    effects.records.clear();

    effects.beforeRecord = [ &controller, &effects ]( RecordingEffects::Kind kind,
                                                      live::Generation generation ) {
        if ( kind != RecordingEffects::Kind::InvalidateGeneration ) {
            return;
        }
        effects.beforeRecord = {};
        controller.deviceAvailable( generation );
    };

    clock.set( at( 500 ) );
    controller.reconnectRequested();

    REQUIRE( effects.records.size() == 4u );
    CHECK( effects.records.at( 0 ).kind == RecordingEffects::Kind::InvalidateGeneration );
    CHECK( effects.records.at( 1 ).kind == RecordingEffects::Kind::CancelStream );
    CHECK( effects.records.at( 1 ).generation == oldGeneration );
    CHECK( effects.records.at( 2 ).kind == RecordingEffects::Kind::StartInfrastructure );
    CHECK( effects.records.at( 2 ).generation == controller.snapshot().generation );
    CHECK( effects.records.at( 3 ).kind == RecordingEffects::Kind::OpenStream );
    CHECK( effects.records.at( 3 ).generation == controller.snapshot().generation );
    CHECK( controller.snapshot().source.status == live::SourceStatus::OpeningStream );
}

TEST_CASE( "Retry policies route infrastructure device and user gates without backoff timers",
           "[livelog-controller][red][retry-policy]" )
{
    const auto runPolicy = []( live::RetryPolicy policy,
                               std::optional<live::AwaitingUserReason> reason = std::nullopt ) {
        ManualClock clock;
        ManualScheduler scheduler;
        RecordingEffects effects;
        livelog::LiveLogController controller( iosSpec(), controllerConfig(), clock, scheduler,
                                               effects );
        armToStreaming( controller, clock );
        auto error = retryableStreamError();
        error.retryPolicy = policy;
        error.awaitingUserReason = reason;
        effects.records.clear();

        controller.streamFailed( controller.snapshot().generation, std::move( error ) );
        return std::make_tuple( controller.snapshot(), scheduler.activeCount(), effects.records );
    };

    const auto [ infrastructure, infrastructureTimers, infrastructureEffects ]
        = runPolicy( live::RetryPolicy::WaitForInfrastructure );
    CHECK( infrastructure.source.status == live::SourceStatus::WaitingForInfrastructure );
    CHECK( infrastructureTimers == 0u );
    CHECK( std::none_of( infrastructureEffects.cbegin(), infrastructureEffects.cend(),
                         []( const auto& effect ) {
                             return effect.kind == RecordingEffects::Kind::OpenStream;
                         } ) );

    const auto [ device, deviceTimers, deviceEffects ]
        = runPolicy( live::RetryPolicy::WaitForDevice );
    CHECK( device.source.status == live::SourceStatus::WaitingForDevice );
    CHECK( deviceTimers == 0u );
    CHECK( std::none_of( deviceEffects.cbegin(), deviceEffects.cend(), []( const auto& effect ) {
        return effect.kind == RecordingEffects::Kind::StartInfrastructure;
    } ) );

    const auto [ awaitingUser, userTimers, userEffects ]
        = runPolicy( live::RetryPolicy::AwaitUser, live::AwaitingUserReason::Trust );
    CHECK( awaitingUser.source.status == live::SourceStatus::AwaitingUser );
    CHECK( awaitingUser.source.awaitingUserReason == live::AwaitingUserReason::Trust );
    CHECK( userTimers == 0u );
    CHECK( std::none_of( userEffects.cbegin(), userEffects.cend(), []( const auto& effect ) {
        return effect.kind == RecordingEffects::Kind::OpenStream;
    } ) );
}

TEST_CASE( "A scheduler that completes inline cannot leave a stale retry token",
           "[livelog-controller][red][retry][scheduler]" )
{
    class InlineScheduler final : public livelog::LiveLogScheduler {
    public:
        Token schedule( live::Timestamp, std::function<void()> callback ) override
        {
            ++scheduleCalls;
            callback();
            return 77u;
        }
        void cancel( Token token ) override
        {
            cancelled.push_back( token );
        }

        int scheduleCalls{ 0 };
        std::vector<Token> cancelled;
    };

    ManualClock clock;
    InlineScheduler scheduler;
    RecordingEffects effects;
    auto config = controllerConfig();
    config.initialRetryDelay = 0ms;
    livelog::LiveLogController controller( androidSpec(), config, clock, scheduler, effects );
    armToOpening( controller, clock );

    controller.streamFailed( controller.snapshot().generation, retryableStreamError() );

    CHECK( scheduler.scheduleCalls == 1 );
    CHECK( controller.snapshot().source.status == live::SourceStatus::OpeningStream );
    CHECK_FALSE( controller.snapshot().retryTimer.has_value() );
    CHECK_FALSE( controller.presentation().retryCountdownVisible );
}

TEST_CASE( "Android streams own ordered wall-time modifiers with optional ANSI color",
           "[livelog-controller][red][ansi][wall-time]" )
{
    LiveSourceTransportConfig config;
    config.sourceType = LiveLogSourceType::AdbLogcat;
    config.adbBackend = AdbTransportBackend::SmartSocket;
    config.deviceId = QStringLiteral( "SERIAL" );

    const auto plain = livelog::makeAdbSmartSocketTransportConfig( config );
    REQUIRE( plain.has_value() );
    const auto plainService = adb::buildLogcatService( plain->logcatOptions );
    REQUIRE( plainService.value.has_value() );
    CHECK( *plainService.value
           == "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec" );

    config.ansiOutputEnabled = true;
    const auto colored = livelog::makeAdbSmartSocketTransportConfig( config );
    REQUIRE( colored.has_value() );
    const auto coloredService = adb::buildLogcatService( colored->logcatOptions );
    REQUIRE( coloredService.value.has_value() );
    CHECK( *coloredService.value
           == "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec -v color" );
}
