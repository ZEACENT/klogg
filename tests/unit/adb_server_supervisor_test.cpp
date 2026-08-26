/*
 * Copyright (C) 2026 ZEACENT and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <catch2/catch.hpp>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHostAddress>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "adbserversupervisor.h"
#include "livestate.h"

namespace {

using namespace std::chrono_literals;
using klogg::livecapture::Generation;
using klogg::livecapture::InfrastructureOwnership;
using klogg::livecapture::InfrastructureStatus;
using klogg::livecapture::LiveSourceError;
using namespace klogg::livecapture::adb;

constexpr Generation FirstGeneration = 101u;
constexpr Generation SecondGeneration = 102u;
constexpr std::uint32_t SupportedAdbProtocolVersion = 0x29u;
constexpr auto BundledAdbPath = "/opt/klogg/libexec/android-platform-tools/adb";
constexpr auto StartupLockPath = "/test-user/state/klogg/adb-server-5037.lock";
constexpr auto FirstIdentity = "adb-server:first";
constexpr auto ReplacementIdentity = "adb-server:replacement";

AdbServerProbeResult absentProbe( std::string diagnostic = "connection refused" )
{
    return AdbServerProbeResult{ AdbServerProbeState::Absent, 0u, {}, {}, std::move( diagnostic ) };
}

AdbServerProbeResult failedProbe( std::string diagnostic )
{
    return AdbServerProbeResult{ AdbServerProbeState::Failed, 0u, {}, {}, std::move( diagnostic ) };
}

AdbServerProbeResult readyProbe( std::string identity = FirstIdentity,
                                 std::uint32_t version = SupportedAdbProtocolVersion,
                                 std::vector<std::string> features = { "shell_v2", "cmd" } )
{
    return AdbServerProbeResult{
        AdbServerProbeState::Ready, version, std::move( features ), std::move( identity ), {}
    };
}

AdbServerSupervisorConfig supervisorConfig()
{
    AdbServerSupervisorConfig config;
    config.endpoint = AdbServerEndpoint{ QHostAddress::LocalHost, 5037 };
    config.packagedServerPath = QString::fromLatin1( BundledAdbPath );
    config.lockPath = QString::fromLatin1( StartupLockPath );
    config.minimumProtocolVersion = SupportedAdbProtocolVersion;
    config.requiredFeatures = { "shell_v2" };
    config.readinessProbeInterval = 25ms;
    config.startupTimeout = 250ms;
    config.healthProbeInterval = 1s;
    config.reconnectBackoff = { 10ms, 20ms, 40ms };
    return config;
}

class ManualProbe final : public AdbServerProbe {
public:
    struct Request {
        AdbServerToken token{ 0 };
        AdbServerEndpoint endpoint;
        Callback callback;
        bool active{ true };
    };

    AdbServerToken probe( const AdbServerEndpoint& endpoint, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, endpoint, std::move( callback ), true } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        for ( auto& request : requests ) {
            if ( request.token == token ) {
                request.active = false;
            }
        }
    }

    void complete( std::size_t index, AdbServerProbeResult result )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( std::move( result ) );
    }

    void completeEvenIfCancelled( std::size_t index, AdbServerProbeResult result )
    {
        REQUIRE( index < requests.size() );
        const auto callback = requests.at( index ).callback;
        callback( std::move( result ) );
    }

    bool allRequestsUseStandardLoopbackEndpoint() const
    {
        return std::all_of( requests.begin(), requests.end(), []( const Request& request ) {
            return request.endpoint.address == QHostAddress::LocalHost
                   && request.endpoint.port == 5037;
        } );
    }

    std::vector<Request> requests;

private:
    AdbServerToken nextToken_{ 0 };
};

class ManualLauncher final : public AdbServerLauncher {
public:
    struct Launch {
        AdbServerToken token{ 0 };
        AdbServerLaunchRequest request;
        Callback callback;
        bool active{ true };
    };

    AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback ) override
    {
        const auto token = ++nextToken_;
        launches.push_back( Launch{ token, request, std::move( callback ), true } );
        return token;
    }

    void cleanup( AdbServerToken token ) override
    {
        cleanupTokens.push_back( token );
        for ( auto& launch : launches ) {
            if ( launch.token == token ) {
                launch.active = false;
            }
        }
    }

    void release( AdbServerToken token ) override
    {
        releasedTokens.push_back( token );
        for ( auto& launch : launches ) {
            if ( launch.token == token ) {
                launch.active = false;
            }
        }
    }

    void emitResult( std::size_t index, AdbServerLaunchResult event )
    {
        REQUIRE( index < launches.size() );
        REQUIRE( launches.at( index ).active );
        const auto callback = launches.at( index ).callback;
        callback( std::move( event ) );
    }

    void emitResultEvenIfCleaned( std::size_t index, AdbServerLaunchResult event )
    {
        REQUIRE( index < launches.size() );
        const auto callback = launches.at( index ).callback;
        callback( std::move( event ) );
    }

    bool wasCleaned( std::size_t index ) const
    {
        REQUIRE( index < launches.size() );
        const auto token = launches.at( index ).token;
        return std::find( cleanupTokens.begin(), cleanupTokens.end(), token )
               != cleanupTokens.end();
    }

    bool wasReleased( std::size_t index ) const
    {
        REQUIRE( index < launches.size() );
        const auto token = launches.at( index ).token;
        return std::find( releasedTokens.begin(), releasedTokens.end(), token )
               != releasedTokens.end();
    }

    std::vector<Launch> launches;
    std::vector<AdbServerToken> cleanupTokens;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class ManualStartupLock final : public AdbServerStartupLock {
public:
    struct Request {
        AdbServerToken token{ 0 };
        QString lockPath;
        Callback callback;
        bool active{ true };
    };

    AdbServerToken acquire( const QString& lockPath, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, lockPath, std::move( callback ), true } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        for ( auto& request : requests ) {
            if ( request.token == token ) {
                request.active = false;
            }
        }
    }

    void release( AdbServerToken token ) override
    {
        releasedTokens.push_back( token );
    }

    void complete( std::size_t index, bool acquired )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( AdbServerStartupLockResult{
            acquired ? AdbServerStartupLockState::Acquired : AdbServerStartupLockState::Contended,
            acquired ? std::string{} : std::string{ "startup lock is held" },
        } );
    }

    void completeWithFailure( std::size_t index, std::string diagnostic )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( AdbServerStartupLockResult{ AdbServerStartupLockState::Failed,
                                              std::move( diagnostic ) } );
    }

    void completeEvenIfCancelled( std::size_t index, bool acquired )
    {
        REQUIRE( index < requests.size() );
        const auto callback = requests.at( index ).callback;
        callback( AdbServerStartupLockResult{
            acquired ? AdbServerStartupLockState::Acquired : AdbServerStartupLockState::Contended,
            acquired ? std::string{} : std::string{ "startup lock is held" },
        } );
    }

    bool wasReleased( std::size_t index ) const
    {
        REQUIRE( index < requests.size() );
        const auto token = requests.at( index ).token;
        return std::find( releasedTokens.begin(), releasedTokens.end(), token )
               != releasedTokens.end();
    }

    std::vector<Request> requests;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class ImmediateStartupLock final : public AdbServerStartupLock {
public:
    AdbServerToken acquire( const QString& lockPath, Callback callback ) override
    {
        acquiredPaths.push_back( lockPath );
        const auto token = ++nextToken_;
        callback( AdbServerStartupLockResult{ AdbServerStartupLockState::Acquired, {} } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        cancelledTokens.push_back( token );
    }

    void release( AdbServerToken token ) override
    {
        releasedTokens.push_back( token );
    }

    std::vector<QString> acquiredPaths;
    std::vector<AdbServerToken> cancelledTokens;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class ImmediateOwnedFailureLauncher final : public AdbServerLauncher {
public:
    AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback ) override
    {
        requests.push_back( request );
        const auto token = ++nextToken_;
        callback( AdbServerLaunchResult{ AdbServerLaunchState::Failed, true,
                                         "synchronous packaged adb launch failure" } );
        return token;
    }

    void cleanup( AdbServerToken token ) override
    {
        cleanupTokens.push_back( token );
    }

    void release( AdbServerToken token ) override
    {
        releasedTokens.push_back( token );
    }

    std::vector<AdbServerLaunchRequest> requests;
    std::vector<AdbServerToken> cleanupTokens;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class FakeAdbKeyStore final : public AdbKeyStore {
public:
    AdbServerKeyInspection inspectStandardKey() override
    {
        ++inspectionCount;
        return inspection;
    }

    AdbServerKeyGenerationResult generateStandardKey() override
    {
        ++generationCount;
        if ( generationResult.generated ) {
            inspection = AdbServerKeyInspection{ AdbServerStandardKeyState::Present, {} };
        }
        return generationResult;
    }

    AdbServerKeyInspection inspection{ AdbServerStandardKeyState::Present, {} };
    AdbServerKeyGenerationResult generationResult{ true, {} };
    int inspectionCount{ 0 };
    int generationCount{ 0 };
};

class ImmediateStartupTimeoutScheduler final : public AdbServerScheduler {
public:
    AdbServerToken schedule( AdbServerScheduleKind kind, std::chrono::milliseconds,
                             Callback callback ) override
    {
        const auto token = ++nextToken_;
        callbacks.push_back( callback );
        kinds.push_back( kind );
        if ( kind == AdbServerScheduleKind::StartupTimeout ) {
            callback();
        }
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        cancelledTokens.push_back( token );
    }

    std::size_t count( AdbServerScheduleKind kind ) const
    {
        return static_cast<std::size_t>( std::count( kinds.begin(), kinds.end(), kind ) );
    }

    std::vector<Callback> callbacks;
    std::vector<AdbServerScheduleKind> kinds;
    std::vector<AdbServerToken> cancelledTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class ManualScheduler final : public AdbServerScheduler {
public:
    struct Entry {
        AdbServerToken token{ 0 };
        AdbServerScheduleKind kind{ AdbServerScheduleKind::ReadinessProbe };
        std::chrono::milliseconds delay{ 0 };
        Callback callback;
        bool active{ true };
    };

    AdbServerToken schedule( AdbServerScheduleKind kind, std::chrono::milliseconds delay,
                             Callback callback ) override
    {
        const auto token = ++nextToken_;
        entries.push_back( Entry{ token, kind, delay, std::move( callback ), true } );
        return token;
    }

    void cancel( AdbServerToken token ) override
    {
        for ( auto& entry : entries ) {
            if ( entry.token == token ) {
                entry.active = false;
            }
        }
    }

    std::size_t activeCount( AdbServerScheduleKind kind ) const
    {
        return static_cast<std::size_t>(
            std::count_if( entries.begin(), entries.end(), [ kind ]( const Entry& entry ) {
                return entry.active && entry.kind == kind;
            } ) );
    }

    std::chrono::milliseconds lastDelay( AdbServerScheduleKind kind ) const
    {
        const auto found
            = std::find_if( entries.rbegin(), entries.rend(),
                            [ kind ]( const Entry& entry ) { return entry.kind == kind; } );
        REQUIRE( found != entries.rend() );
        return found->delay;
    }

    std::size_t lastIndex( AdbServerScheduleKind kind ) const
    {
        for ( auto index = entries.size(); index > 0; --index ) {
            if ( entries.at( index - 1 ).kind == kind ) {
                return index - 1;
            }
        }
        FAIL( "scheduled callback not found" );
        return 0;
    }

    void fire( AdbServerScheduleKind kind )
    {
        const auto index = lastIndex( kind );
        REQUIRE( entries.at( index ).active );
        entries.at( index ).active = false;
        const auto callback = entries.at( index ).callback;
        callback();
    }

    void fireEvenIfCancelled( std::size_t index )
    {
        REQUIRE( index < entries.size() );
        const auto callback = entries.at( index ).callback;
        callback();
    }

    std::vector<Entry> entries;

private:
    AdbServerToken nextToken_{ 0 };
};

struct SupervisorEventRecorder {
    struct StateEvent {
        Generation generation{ 0 };
        std::uint64_t epoch{ 0 };
        AdbServerSupervisorSnapshot snapshot;
    };
    struct ConsentEvent {
        Generation generation{ 0 };
        std::uint64_t epoch{ 0 };
    };
    struct ErrorEvent {
        Generation generation{ 0 };
        std::uint64_t epoch{ 0 };
        LiveSourceError error;
    };

    explicit SupervisorEventRecorder( AdbServerSupervisor& supervisor )
    {
        QObject::connect( &supervisor, &AdbServerSupervisor::stateChanged,
                          [ this ]( Generation generation, std::uint64_t epoch,
                                    const AdbServerSupervisorSnapshot& snapshot ) {
                              states.push_back( StateEvent{ generation, epoch, snapshot } );
                          } );
        QObject::connect( &supervisor, &AdbServerSupervisor::consentRequired,
                          [ this ]( Generation generation, std::uint64_t epoch ) {
                              consentRequests.push_back( ConsentEvent{ generation, epoch } );
                          } );
        QObject::connect(
            &supervisor, &AdbServerSupervisor::errorOccurred,
            [ this ]( Generation generation, std::uint64_t epoch, const LiveSourceError& error ) {
                errors.push_back( ErrorEvent{ generation, epoch, error } );
            } );
    }

    std::vector<StateEvent> states;
    std::vector<ConsentEvent> consentRequests;
    std::vector<ErrorEvent> errors;
};

const std::string& diagnostic( const AdbServerSupervisorSnapshot& snapshot )
{
    REQUIRE( snapshot.error.has_value() );
    return snapshot.error->nativeDetail;
}

struct SupervisorHarness {
    ManualProbe probe;
    ManualLauncher launcher;
    ManualStartupLock startupLock;
    FakeAdbKeyStore keyStore;
    ManualScheduler scheduler;
    AdbServerSupervisor supervisor;
    SupervisorEventRecorder events;

    explicit SupervisorHarness( AdbServerSupervisorConfig config = supervisorConfig() )
        : supervisor( std::move( config ), probe, launcher, startupLock, keyStore, scheduler )
        , events( supervisor )
    {
    }

    void reachLaunch( bool standardKeyPresent = true, Generation generation = FirstGeneration )
    {
        keyStore.inspection.state = standardKeyPresent ? AdbServerStandardKeyState::Present
                                                       : AdbServerStandardKeyState::Absent;
        supervisor.start( generation );
        probe.complete( 0, absentProbe() );
        REQUIRE( startupLock.requests.size() == 1 );
        startupLock.complete( 0, true );
        REQUIRE( probe.requests.size() == 2 );
        probe.complete( 1, absentProbe() );
        if ( !standardKeyPresent ) {
            REQUIRE( supervisor.snapshot().status
                     == AdbServerSupervisorStatus::AwaitingKeyGenerationConsent );
            supervisor.grantKeyGenerationConsent( generation, true );
        }
        REQUIRE( launcher.launches.size() == 1 );
    }

    void publishAppOwnedReady( std::string identity = FirstIdentity )
    {
        reachLaunch();
        launcher.emitResult( 0, AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
        REQUIRE( scheduler.activeCount( AdbServerScheduleKind::ReadinessProbe ) == 1 );
        scheduler.fire( AdbServerScheduleKind::ReadinessProbe );
        REQUIRE( probe.requests.size() == 3 );
        probe.complete( 2, readyProbe( std::move( identity ) ) );
        REQUIRE( supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
    }
};

void requireNoEnvironmentChangingWork( const SupervisorHarness& harness )
{
    CHECK( harness.startupLock.requests.empty() );
    CHECK( harness.keyStore.inspectionCount == 0 );
    CHECK( harness.keyStore.generationCount == 0 );
    CHECK( harness.launcher.launches.empty() );
    CHECK( harness.launcher.cleanupTokens.empty() );
    CHECK( harness.launcher.releasedTokens.empty() );
}

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable( QByteArray name, const QByteArray& value )
        : name_( std::move( name ) )
        , wasSet_( qEnvironmentVariableIsSet( name_.constData() ) )
        , previous_( qgetenv( name_.constData() ) )
    {
        qputenv( name_.constData(), value );
    }

    ~ScopedEnvironmentVariable()
    {
        if ( wasSet_ ) {
            qputenv( name_.constData(), previous_ );
        }
        else {
            qunsetenv( name_.constData() );
        }
    }

private:
    QByteArray name_;
    bool wasSet_{ false };
    QByteArray previous_;
};

class AdbHelperStopGuard final {
public:
    explicit AdbHelperStopGuard( QString stopPath )
        : stopPath_( std::move( stopPath ) )
    {
        QFile::remove( stopPath_ );
    }

    ~AdbHelperStopGuard()
    {
        QFile stopFile( stopPath_ );
        if ( stopFile.open( QIODevice::WriteOnly ) ) {
            stopFile.write( "stop" );
        }
    }

    void requestStop()
    {
        QFile stopFile( stopPath_ );
        REQUIRE( stopFile.open( QIODevice::WriteOnly ) );
        REQUIRE( stopFile.write( "stop" ) == 4 );
        stopFile.close();
    }

private:
    QString stopPath_;
};

std::uint64_t readHeartbeat( const QString& path )
{
    QFile heartbeatFile( path );
    if ( !heartbeatFile.open( QIODevice::ReadOnly ) ) {
        return 0u;
    }
    bool parsed = false;
    const auto heartbeat = heartbeatFile.readAll().trimmed().toULongLong( &parsed );
    return parsed ? heartbeat : 0u;
}

} // namespace

TEST_CASE(
    "ADB server supervisor rejects invalid endpoint or executable configuration before probing",
    "[livecapture][adb][supervisor][validation]" )
{
    const std::vector<AdbServerEndpoint> invalidEndpoints{
        { QHostAddress( QStringLiteral( "192.0.2.10" ) ), 5037 },
        { QHostAddress::AnyIPv4, 5037 },
        { QHostAddress::LocalHost, 5038 },
        { QHostAddress::LocalHost, 0 },
    };

    for ( const auto& endpoint : invalidEndpoints ) {
        DYNAMIC_SECTION( endpoint.address.toString().toStdString() << ':' << endpoint.port )
        {
            auto config = supervisorConfig();
            config.endpoint = endpoint;
            SupervisorHarness harness( config );

            harness.supervisor.start( FirstGeneration );

            CHECK( harness.supervisor.snapshot().generation == FirstGeneration );
            CHECK( harness.supervisor.snapshot().status
                   == AdbServerSupervisorStatus::InvalidConfiguration );
            REQUIRE( harness.supervisor.snapshot().error.has_value() );
            CHECK_FALSE( diagnostic( harness.supervisor.snapshot() ).empty() );
            CHECK( harness.probe.requests.empty() );
            requireNoEnvironmentChangingWork( harness );
        }
    }

    SECTION( "packaged server must be an explicit absolute path" )
    {
        auto config = supervisorConfig();
        config.packagedServerPath = QStringLiteral( "adb" );
        SupervisorHarness harness( config );

        harness.supervisor.start( FirstGeneration );

        CHECK( harness.supervisor.snapshot().status
               == AdbServerSupervisorStatus::InvalidConfiguration );
        CHECK( harness.probe.requests.empty() );
        requireNoEnvironmentChangingWork( harness );
    }
}

TEST_CASE( "compatible server already on the standard endpoint is adopted as external shared",
           "[livecapture][adb][supervisor][external]" )
{
    SupervisorHarness harness;

    harness.supervisor.start( FirstGeneration );
    REQUIRE( harness.probe.requests.size() == 1 );
    CHECK( harness.probe.allRequestsUseStandardLoopbackEndpoint() );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Probing );

    harness.probe.complete( 0, readyProbe() );

    const auto ready = harness.supervisor.snapshot();
    CHECK( ready.generation == FirstGeneration );
    CHECK( ready.status == AdbServerSupervisorStatus::Ready );
    CHECK( ready.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( ready.infrastructure.ownership.has_value() );
    CHECK( *ready.infrastructure.ownership == InfrastructureOwnership::ExternalShared );
    CHECK( ready.serverIdentity == FirstIdentity );
    CHECK( ready.protocolVersion == SupportedAdbProtocolVersion );
    CHECK( ready.epoch > 0u );
    REQUIRE_FALSE( harness.events.states.empty() );
    CHECK( harness.events.states.back().generation == FirstGeneration );
    CHECK( harness.events.states.back().epoch == ready.epoch );
    CHECK( harness.events.states.back().snapshot.generation == FirstGeneration );
    requireNoEnvironmentChangingWork( harness );

    harness.supervisor.stop( FirstGeneration );
    CHECK( harness.launcher.cleanupTokens.empty() );
}

TEST_CASE( "absent server uses one per-user lock re-probe consent and the exact bundled executable",
           "[livecapture][adb][supervisor][startup][keys]" )
{
    SupervisorHarness harness;
    harness.keyStore.inspection.state = AdbServerStandardKeyState::Absent;

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    REQUIRE( harness.startupLock.requests.size() == 1 );
    CHECK( harness.startupLock.requests.front().lockPath
           == QString::fromLatin1( StartupLockPath ) );

    harness.startupLock.complete( 0, true );
    REQUIRE( harness.probe.requests.size() == 2 );
    CHECK( harness.launcher.launches.empty() );
    harness.probe.complete( 1, absentProbe( "still absent under lock" ) );

    REQUIRE( harness.supervisor.snapshot().status
             == AdbServerSupervisorStatus::AwaitingKeyGenerationConsent );
    CHECK( harness.supervisor.snapshot().keyConsent == AdbServerKeyConsentState::Required );
    REQUIRE( harness.events.consentRequests.size() == 1 );
    CHECK( harness.events.consentRequests.front().generation == FirstGeneration );
    CHECK( harness.events.consentRequests.front().epoch == harness.supervisor.snapshot().epoch );
    CHECK( harness.keyStore.inspectionCount == 1 );
    CHECK( harness.keyStore.generationCount == 0 );
    CHECK( harness.launcher.launches.empty() );

    harness.supervisor.grantKeyGenerationConsent( FirstGeneration, true );

    CHECK( harness.supervisor.snapshot().keyConsent == AdbServerKeyConsentState::Granted );
    CHECK( harness.keyStore.generationCount == 1 );
    REQUIRE( harness.launcher.launches.size() == 1 );
    const auto& request = harness.launcher.launches.front().request;
    CHECK( request.executable == QString::fromLatin1( BundledAdbPath ) );
    CHECK( request.arguments
           == QStringList{ QStringLiteral( "server" ), QStringLiteral( "nodaemon" ) } );
    CHECK_FALSE( request.allowPathLookup );
    CHECK( harness.probe.allRequestsUseStandardLoopbackEndpoint() );

    harness.launcher.emitResult( 0,
                                 AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Starting );
    CHECK( harness.scheduler.activeCount( AdbServerScheduleKind::StartupTimeout ) == 1 );
    CHECK( harness.scheduler.activeCount( AdbServerScheduleKind::ReadinessProbe ) == 1 );
    CHECK( harness.probe.requests.size() == 2 );

    harness.scheduler.fire( AdbServerScheduleKind::ReadinessProbe );
    REQUIRE( harness.probe.requests.size() == 3 );
    harness.probe.complete( 2, readyProbe() );

    const auto ready = harness.supervisor.snapshot();
    CHECK( ready.generation == FirstGeneration );
    CHECK( ready.status == AdbServerSupervisorStatus::Ready );
    CHECK( ready.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( ready.infrastructure.ownership.has_value() );
    CHECK( *ready.infrastructure.ownership == InfrastructureOwnership::AppShared );
    CHECK( harness.startupLock.wasReleased( 0 ) );
    CHECK( harness.launcher.cleanupTokens.empty() );

    harness.supervisor.stop( FirstGeneration );
    CHECK( harness.launcher.cleanupTokens.empty() );
}

TEST_CASE( "existing standard adb key needs no consent and is never overwritten or rotated",
           "[livecapture][adb][supervisor][keys]" )
{
    SupervisorHarness harness;
    harness.reachLaunch( true );

    CHECK( harness.keyStore.inspectionCount == 1 );
    CHECK( harness.keyStore.generationCount == 0 );
    CHECK( harness.supervisor.snapshot().keyConsent == AdbServerKeyConsentState::NotRequired );

    harness.launcher.emitResult( 0,
                                 AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
    harness.scheduler.fire( AdbServerScheduleKind::ReadinessProbe );
    harness.probe.complete( 2, absentProbe( "server not ready yet" ) );
    harness.scheduler.fire( AdbServerScheduleKind::ReadinessProbe );
    harness.probe.complete( 3, readyProbe() );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
    CHECK( harness.keyStore.generationCount == 0 );
}

TEST_CASE( "missing standard adb key requires an explicit decision and denial never launches",
           "[livecapture][adb][supervisor][keys][consent]" )
{
    SupervisorHarness harness;
    harness.keyStore.inspection.state = AdbServerStandardKeyState::Absent;

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    harness.startupLock.complete( 0, true );
    harness.probe.complete( 1, absentProbe() );

    REQUIRE( harness.supervisor.snapshot().status
             == AdbServerSupervisorStatus::AwaitingKeyGenerationConsent );
    CHECK( harness.keyStore.generationCount == 0 );
    harness.supervisor.grantKeyGenerationConsent( FirstGeneration, false );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
    CHECK( harness.supervisor.snapshot().keyConsent == AdbServerKeyConsentState::Denied );
    CHECK( harness.keyStore.generationCount == 0 );
    CHECK( harness.launcher.launches.empty() );
    CHECK( harness.startupLock.wasReleased( 0 ) );
}

TEST_CASE( "competing server that wins after lock acquisition is adopted without a launch",
           "[livecapture][adb][supervisor][race]" )
{
    SupervisorHarness harness;

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    harness.startupLock.complete( 0, true );
    REQUIRE( harness.probe.requests.size() == 2 );
    harness.probe.complete( 1, readyProbe( ReplacementIdentity ) );

    const auto ready = harness.supervisor.snapshot();
    CHECK( ready.generation == FirstGeneration );
    CHECK( ready.status == AdbServerSupervisorStatus::Ready );
    CHECK( ready.infrastructure.status == InfrastructureStatus::Ready );
    REQUIRE( ready.infrastructure.ownership.has_value() );
    CHECK( *ready.infrastructure.ownership == InfrastructureOwnership::ExternalShared );
    CHECK( ready.serverIdentity == ReplacementIdentity );
    CHECK( harness.launcher.launches.empty() );
    CHECK( harness.keyStore.inspectionCount == 0 );
    CHECK( harness.startupLock.wasReleased( 0 ) );
}

TEST_CASE( "concurrent supervisors serialize startup and launch at most once",
           "[livecapture][adb][supervisor][race][lock]" )
{
    ManualStartupLock sharedLock;
    ManualProbe firstProbe;
    ManualProbe secondProbe;
    ManualLauncher firstLauncher;
    ManualLauncher secondLauncher;
    FakeAdbKeyStore firstKeys;
    FakeAdbKeyStore secondKeys;
    ManualScheduler firstScheduler;
    ManualScheduler secondScheduler;
    const auto config = supervisorConfig();
    AdbServerSupervisor first( config, firstProbe, firstLauncher, sharedLock, firstKeys,
                               firstScheduler );
    AdbServerSupervisor second( config, secondProbe, secondLauncher, sharedLock, secondKeys,
                                secondScheduler );

    first.start( FirstGeneration );
    second.start( FirstGeneration );
    firstProbe.complete( 0, absentProbe() );
    secondProbe.complete( 0, absentProbe() );
    REQUIRE( sharedLock.requests.size() == 2 );

    sharedLock.complete( 0, true );
    sharedLock.complete( 1, false );
    firstProbe.complete( 1, absentProbe() );
    REQUIRE( firstLauncher.launches.size() == 1 );
    CHECK( secondLauncher.launches.empty() );
    CHECK( secondScheduler.activeCount( AdbServerScheduleKind::LockRetry ) == 1 );

    firstLauncher.emitResult( 0, AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
    firstScheduler.fire( AdbServerScheduleKind::ReadinessProbe );
    firstProbe.complete( 2, readyProbe() );
    REQUIRE( first.snapshot().status == AdbServerSupervisorStatus::Ready );

    secondScheduler.fire( AdbServerScheduleKind::LockRetry );
    REQUIRE( secondProbe.requests.size() == 2 );
    secondProbe.complete( 1, readyProbe() );

    CHECK( firstLauncher.launches.size() + secondLauncher.launches.size() == 1 );
    CHECK( second.snapshot().status == AdbServerSupervisorStatus::Ready );
    REQUIRE( second.snapshot().infrastructure.ownership.has_value() );
    CHECK( *second.snapshot().infrastructure.ownership == InfrastructureOwnership::ExternalShared );
}

TEST_CASE( "synchronous startup lock acquisition retains and releases the acquired lease",
           "[livecapture][adb][supervisor][race][lock][reentrant]" )
{
    ManualProbe probe;
    ManualLauncher launcher;
    ImmediateStartupLock startupLock;
    FakeAdbKeyStore keyStore;
    ManualScheduler scheduler;
    AdbServerSupervisor supervisor( supervisorConfig(), probe, launcher, startupLock, keyStore,
                                    scheduler );

    supervisor.start( FirstGeneration );
    probe.complete( 0, absentProbe() );

    REQUIRE( startupLock.acquiredPaths.size() == 1 );
    CHECK( startupLock.acquiredPaths.front() == QString::fromLatin1( StartupLockPath ) );
    REQUIRE( probe.requests.size() == 2 );
    probe.complete( 1, readyProbe( ReplacementIdentity ) );

    CHECK( supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
    CHECK( startupLock.releasedTokens == std::vector<AdbServerToken>{ 1u } );
    CHECK( startupLock.cancelledTokens.empty() );
    CHECK( launcher.launches.empty() );
}

TEST_CASE( "old or feature-incompatible ADB servers are reported without modifying the environment",
           "[livecapture][adb][supervisor][compatibility]" )
{
    const std::vector<AdbServerProbeResult> incompatible{
        readyProbe( FirstIdentity, SupportedAdbProtocolVersion - 1u ),
        readyProbe( FirstIdentity, SupportedAdbProtocolVersion, { "cmd" } ),
    };

    for ( const auto& result : incompatible ) {
        SupervisorHarness harness;
        harness.supervisor.start( FirstGeneration );
        harness.probe.complete( 0, result );

        CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Incompatible );
        CHECK_FALSE( diagnostic( harness.supervisor.snapshot() ).empty() );
        requireNoEnvironmentChangingWork( harness );
    }
}

TEST_CASE( "a synchronous startup timeout cannot arm a readiness probe after failure",
           "[livecapture][adb][supervisor][startup][timeout][reentrant]" )
{
    ManualProbe probe;
    ManualLauncher launcher;
    ManualStartupLock startupLock;
    FakeAdbKeyStore keyStore;
    ImmediateStartupTimeoutScheduler scheduler;
    AdbServerSupervisor supervisor( supervisorConfig(), probe, launcher, startupLock, keyStore,
                                    scheduler );

    supervisor.start( FirstGeneration );
    probe.complete( 0, absentProbe() );
    startupLock.complete( 0, true );
    probe.complete( 1, absentProbe() );
    REQUIRE( launcher.launches.size() == 1 );
    launcher.emitResult( 0, AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );

    CHECK( supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
    REQUIRE( supervisor.snapshot().error.has_value() );
    CHECK( supervisor.snapshot().error->code == "startup-timeout" );
    CHECK( scheduler.count( AdbServerScheduleKind::StartupTimeout ) == 1 );
    CHECK( scheduler.count( AdbServerScheduleKind::ReadinessProbe ) == 0 );
    CHECK( launcher.wasCleaned( 0 ) );
}

TEST_CASE( "launch failure and readiness timeout preserve actionable diagnostics",
           "[livecapture][adb][supervisor][startup][error]" )
{
    SECTION( "launch failure" )
    {
        SupervisorHarness harness;
        harness.reachLaunch();
        harness.launcher.emitResult( 0, AdbServerLaunchResult{ AdbServerLaunchState::Failed, false,
                                                               "codesign rejected bundled adb" } );

        CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
        REQUIRE( harness.supervisor.snapshot().error.has_value() );
        CHECK( diagnostic( harness.supervisor.snapshot() ).find( "codesign" )
               != std::string::npos );
        REQUIRE( harness.events.errors.size() == 1 );
        CHECK( harness.events.errors.front().generation == FirstGeneration );
        CHECK( harness.events.errors.front().epoch == harness.supervisor.snapshot().epoch );
        CHECK( harness.events.errors.front().error.nativeDetail.find( "codesign" )
               != std::string::npos );
        CHECK( harness.startupLock.wasReleased( 0 ) );
    }

    SECTION( "timeout retains last probe failure and cleans an owned pre-ready child" )
    {
        SupervisorHarness harness;
        harness.reachLaunch();
        harness.launcher.emitResult(
            0, AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
        harness.scheduler.fire( AdbServerScheduleKind::ReadinessProbe );
        harness.probe.complete( 2, failedProbe( "protocol handshake truncated" ) );
        REQUIRE( harness.scheduler.activeCount( AdbServerScheduleKind::ReadinessProbe ) == 1 );

        harness.scheduler.fire( AdbServerScheduleKind::StartupTimeout );

        CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
        CHECK( diagnostic( harness.supervisor.snapshot() ).find( "timed out" )
               != std::string::npos );
        CHECK( diagnostic( harness.supervisor.snapshot() ).find( "handshake truncated" )
               != std::string::npos );
        CHECK( harness.launcher.wasCleaned( 0 ) );
        CHECK( harness.startupLock.wasReleased( 0 ) );
    }
}

TEST_CASE( "synchronous owned launch failure is cleaned after the launcher returns its token",
           "[livecapture][adb][supervisor][startup][ownership][reentrant]" )
{
    ManualProbe probe;
    ImmediateOwnedFailureLauncher launcher;
    ManualStartupLock startupLock;
    FakeAdbKeyStore keyStore;
    ManualScheduler scheduler;
    AdbServerSupervisor supervisor( supervisorConfig(), probe, launcher, startupLock, keyStore,
                                    scheduler );

    supervisor.start( FirstGeneration );
    probe.complete( 0, absentProbe() );
    startupLock.complete( 0, true );
    probe.complete( 1, absentProbe() );

    CHECK( supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
    REQUIRE( supervisor.snapshot().error.has_value() );
    CHECK( supervisor.snapshot().error->nativeDetail.find( "synchronous" ) != std::string::npos );
    CHECK( launcher.cleanupTokens == std::vector<AdbServerToken>{ 1u } );
    CHECK( startupLock.wasReleased( 0 ) );
}

TEST_CASE( "pre-ready child exit is cleaned only when launcher ownership permits",
           "[livecapture][adb][supervisor][startup][ownership]" )
{
    for ( const bool cleanupPermitted : { false, true } ) {
        DYNAMIC_SECTION( "cleanup permitted " << cleanupPermitted )
        {
            SupervisorHarness harness;
            harness.reachLaunch();
            harness.launcher.emitResult(
                0, AdbServerLaunchResult{ AdbServerLaunchState::Started, cleanupPermitted, {} } );
            harness.launcher.emitResult(
                0, AdbServerLaunchResult{ AdbServerLaunchState::Exited, cleanupPermitted,
                                          "bundled adb exited with status 7" } );

            CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
            CHECK( diagnostic( harness.supervisor.snapshot() ).find( "status 7" )
                   != std::string::npos );
            CHECK( harness.launcher.wasCleaned( 0 ) == cleanupPermitted );
        }
    }
}

TEST_CASE( "published app-owned server is shared and never killed by tab or app shutdown",
           "[livecapture][adb][supervisor][ownership][shutdown]" )
{
    SupervisorHarness harness;
    harness.publishAppOwnedReady();
    const auto publishedEpoch = harness.supervisor.snapshot().epoch;

    harness.supervisor.stop( FirstGeneration );
    harness.supervisor.stop( FirstGeneration );

    CHECK( harness.launcher.cleanupTokens.empty() );
    REQUIRE( harness.launcher.launches.size() == 1 );
    CHECK( harness.launcher.wasReleased( 0 ) );
    CHECK( harness.supervisor.snapshot().epoch == publishedEpoch );
    REQUIRE( harness.supervisor.snapshot().infrastructure.ownership.has_value() );
    CHECK( *harness.supervisor.snapshot().infrastructure.ownership
           == InfrastructureOwnership::AppShared );
}

TEST_CASE( "Qt launcher refuses PATH resolution even when an adb-named executable is available",
           "[livecapture][adb][supervisor][packaged-path][adapter]" )
{
    const QFileInfo helperInfo( QString::fromUtf8( KLOGG_ADB_SERVER_TEST_HELPER_PATH ) );
    REQUIRE( helperInfo.isAbsolute() );
    ScopedEnvironmentVariable pathEnvironment( QByteArrayLiteral( "PATH" ),
                                               helperInfo.absolutePath().toUtf8() );
    QtAdbServerLauncher launcher;
    std::optional<AdbServerLaunchResult> result;
    const AdbServerLaunchRequest request{
        helperInfo.fileName(),
        { QStringLiteral( "server" ), QStringLiteral( "nodaemon" ) },
        false,
        AdbServerEndpoint{ QHostAddress::LocalHost, 5037 },
    };

    launcher.launch( request,
                     [ &result ]( AdbServerLaunchResult event ) { result = std::move( event ); } );

    QTRY_VERIFY( result.has_value() );
    CHECK( result->state == AdbServerLaunchState::Failed );
    CHECK_FALSE( result->cleanupPermitted );
    CHECK( result->diagnostic.find( "explicit executable" ) != std::string::npos );
}

TEST_CASE( "published Qt launcher process survives launcher destruction and drains output",
           "[livecapture][adb][supervisor][ownership][shutdown][adapter]" )
{
    QTemporaryDir temporaryDirectory;
    REQUIRE( temporaryDirectory.isValid() );
    const auto heartbeatPath
        = temporaryDirectory.filePath( QStringLiteral( "adb-helper-heartbeat" ) );
    const auto stopPath = temporaryDirectory.filePath( QStringLiteral( "adb-helper-stop" ) );
    ScopedEnvironmentVariable heartbeatEnvironment(
        QByteArrayLiteral( "KLOGG_ADB_HELPER_HEARTBEAT" ), heartbeatPath.toUtf8() );
    ScopedEnvironmentVariable stopEnvironment( QByteArrayLiteral( "KLOGG_ADB_HELPER_STOP" ),
                                               stopPath.toUtf8() );
    AdbHelperStopGuard stopGuard( stopPath );
    std::optional<AdbServerLaunchResult> result;

    {
        QtAdbServerLauncher launcher;
        const AdbServerLaunchRequest request{
            QString::fromUtf8( KLOGG_ADB_SERVER_TEST_HELPER_PATH ),
            { QStringLiteral( "server" ), QStringLiteral( "nodaemon" ) },
            false,
            AdbServerEndpoint{ QHostAddress::LocalHost, 5037 },
        };
        const auto token = launcher.launch(
            request, [ &result ]( AdbServerLaunchResult event ) { result = std::move( event ); } );
        QTRY_VERIFY( result.has_value() );
        REQUIRE( result->state == AdbServerLaunchState::Started );
        QTRY_VERIFY( readHeartbeat( heartbeatPath ) > 0u );
        launcher.release( token );
    }

    const auto heartbeatAfterLauncherDestruction = readHeartbeat( heartbeatPath );
    QTRY_VERIFY( readHeartbeat( heartbeatPath ) > heartbeatAfterLauncherDestruction );
    stopGuard.requestStop();
    QTest::qWait( 100 );
}

TEST_CASE( "server disappearance and replacement advance epoch and use bounded injected backoff",
           "[livecapture][adb][supervisor][reconnect][epoch]" )
{
    SupervisorHarness harness;
    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, readyProbe() );
    const auto firstEpoch = harness.supervisor.snapshot().epoch;
    REQUIRE( harness.scheduler.activeCount( AdbServerScheduleKind::HealthProbe ) == 1 );

    harness.scheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.complete( 1, absentProbe( "server disappeared" ) );
    CHECK( harness.supervisor.snapshot().epoch == firstEpoch + 1u );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::RetryWait );
    CHECK( harness.scheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff ) == 10ms );

    harness.scheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    harness.probe.complete( 2, failedProbe( "still unavailable" ) );
    CHECK( harness.scheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff ) == 20ms );
    harness.scheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    harness.probe.complete( 3, failedProbe( "still unavailable" ) );
    CHECK( harness.scheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff ) == 40ms );
    harness.scheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    harness.probe.complete( 4, failedProbe( "still unavailable" ) );
    CHECK( harness.scheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff ) == 40ms );
    harness.scheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    harness.probe.complete( 5, readyProbe( ReplacementIdentity ) );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
    CHECK( harness.supervisor.snapshot().epoch == firstEpoch + 1u );
    CHECK( harness.supervisor.snapshot().serverIdentity == ReplacementIdentity );
    REQUIRE( harness.supervisor.snapshot().infrastructure.ownership.has_value() );
    CHECK( *harness.supervisor.snapshot().infrastructure.ownership
           == InfrastructureOwnership::ExternalShared );

    harness.scheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.complete( 6, readyProbe( FirstIdentity ) );
    CHECK( harness.supervisor.snapshot().epoch == firstEpoch + 2u );
    CHECK( harness.supervisor.snapshot().serverIdentity == FirstIdentity );
}

TEST_CASE( "stale probe timer and launch callbacks cannot mutate a newer supervisor run",
           "[livecapture][adb][supervisor][generation][stale]" )
{
    SECTION( "probe callback" )
    {
        SupervisorHarness harness;
        harness.supervisor.start( FirstGeneration );
        harness.supervisor.stop( FirstGeneration );
        harness.supervisor.start( SecondGeneration );
        REQUIRE( harness.probe.requests.size() == 2 );
        const auto stateEventCountBeforeStaleProbe = harness.events.states.size();

        harness.probe.completeEvenIfCancelled( 0, readyProbe( "stale" ) );
        CHECK( harness.events.states.size() == stateEventCountBeforeStaleProbe );
        CHECK( harness.supervisor.snapshot().generation == SecondGeneration );
        CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Probing );
        CHECK( harness.supervisor.snapshot().serverIdentity.empty() );

        harness.probe.complete( 1, readyProbe( "current" ) );
        CHECK( harness.supervisor.snapshot().generation == SecondGeneration );
        CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
        CHECK( harness.supervisor.snapshot().serverIdentity == "current" );
        REQUIRE_FALSE( harness.events.states.empty() );
        CHECK( harness.events.states.back().generation == SecondGeneration );
    }

    SECTION( "readiness timer and launch callback" )
    {
        SupervisorHarness harness;
        harness.reachLaunch();
        harness.launcher.emitResult(
            0, AdbServerLaunchResult{ AdbServerLaunchState::Started, true, {} } );
        const auto staleTimer
            = harness.scheduler.lastIndex( AdbServerScheduleKind::ReadinessProbe );

        harness.supervisor.stop( FirstGeneration );
        REQUIRE( harness.launcher.wasCleaned( 0 ) );
        harness.supervisor.start( SecondGeneration );
        const auto currentProbeCount = harness.probe.requests.size();
        const auto stateEventCountBeforeStaleCallbacks = harness.events.states.size();
        const auto errorBeforeStaleCallbacks = harness.supervisor.snapshot().error;

        harness.scheduler.fireEvenIfCancelled( staleTimer );
        harness.launcher.emitResultEvenIfCleaned(
            0,
            AdbServerLaunchResult{ AdbServerLaunchState::Failed, true, "stale child callback" } );

        CHECK( harness.probe.requests.size() == currentProbeCount );
        CHECK( harness.events.states.size() == stateEventCountBeforeStaleCallbacks );
        CHECK( harness.supervisor.snapshot().generation == SecondGeneration );
        CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Probing );
        CHECK( harness.supervisor.snapshot().error.has_value()
               == errorBeforeStaleCallbacks.has_value() );
        if ( errorBeforeStaleCallbacks.has_value() ) {
            CHECK( harness.supervisor.snapshot().error->nativeDetail
                   == errorBeforeStaleCallbacks->nativeDetail );
        }
        CHECK( harness.events.errors.empty() );
    }
}

TEST_CASE( "health probes preserve the published ready state until a result arrives",
           "[livecapture][adb][supervisor][health][state]" )
{
    SupervisorHarness harness;
    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, readyProbe() );
    const auto readyEpoch = harness.supervisor.snapshot().epoch;
    const auto readyStateCount = harness.events.states.size();

    harness.scheduler.fire( AdbServerScheduleKind::HealthProbe );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
    CHECK( harness.supervisor.snapshot().infrastructure.status == InfrastructureStatus::Ready );
    CHECK( harness.supervisor.snapshot().epoch == readyEpoch );
    CHECK( harness.events.states.size() == readyStateCount );
    REQUIRE( harness.probe.requests.size() == 2 );
    harness.probe.complete( 1, readyProbe() );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
}

TEST_CASE( "an incompatible health response invalidates the previously published server",
           "[livecapture][adb][supervisor][health][compatibility][epoch]" )
{
    SupervisorHarness harness;
    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, readyProbe() );
    const auto readyEpoch = harness.supervisor.snapshot().epoch;

    harness.scheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.complete( 1,
                            readyProbe( ReplacementIdentity, SupportedAdbProtocolVersion - 1u ) );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Incompatible );
    CHECK( harness.supervisor.snapshot().epoch == readyEpoch + 1u );
    CHECK( harness.supervisor.snapshot().serverIdentity.empty() );
    CHECK( harness.supervisor.snapshot().protocolVersion == 0u );
    CHECK_FALSE( harness.supervisor.snapshot().infrastructure.ownership.has_value() );
}

TEST_CASE( "stopping a launch before its started callback cleans the owned process token",
           "[livecapture][adb][supervisor][startup][ownership][race]" )
{
    SupervisorHarness harness;
    harness.reachLaunch();
    REQUIRE( harness.launcher.launches.size() == 1 );

    harness.supervisor.stop( FirstGeneration );

    CHECK( harness.launcher.wasCleaned( 0 ) );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Stopped );
    harness.launcher.emitResultEvenIfCleaned(
        0, AdbServerLaunchResult{ AdbServerLaunchState::Started, true, "stale start" } );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Stopped );
}

TEST_CASE( "a stale acquired startup lock callback releases the lease after cancellation",
           "[livecapture][adb][supervisor][lock][generation][stale]" )
{
    SupervisorHarness harness;
    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    REQUIRE( harness.startupLock.requests.size() == 1 );

    harness.supervisor.stop( FirstGeneration );
    harness.startupLock.completeEvenIfCancelled( 0, true );

    CHECK( harness.startupLock.wasReleased( 0 ) );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Stopped );
}

TEST_CASE( "a startup lock acquired after supervisor destruction is still released",
           "[livecapture][adb][supervisor][lock][shutdown][stale]" )
{
    ManualProbe probe;
    ManualLauncher launcher;
    ManualStartupLock startupLock;
    FakeAdbKeyStore keyStore;
    ManualScheduler scheduler;
    {
        AdbServerSupervisor supervisor( supervisorConfig(), probe, launcher, startupLock, keyStore,
                                        scheduler );
        supervisor.start( FirstGeneration );
        probe.complete( 0, absentProbe() );
        REQUIRE( startupLock.requests.size() == 1 );
    }

    startupLock.completeEvenIfCancelled( 0, true );

    CHECK( startupLock.wasReleased( 0 ) );
}

TEST_CASE( "permanent startup lock failures stop instead of retrying as contention",
           "[livecapture][adb][supervisor][lock][error]" )
{
    SupervisorHarness harness;
    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    REQUIRE( harness.startupLock.requests.size() == 1 );

    harness.startupLock.completeWithFailure( 0, "permission denied creating lock file" );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Failed );
    REQUIRE( harness.supervisor.snapshot().error.has_value() );
    CHECK( harness.supervisor.snapshot().error->code == "startup-lock-failed" );
    CHECK( harness.supervisor.snapshot().error->nativeDetail.find( "permission denied" )
           != std::string::npos );
    CHECK( harness.scheduler.activeCount( AdbServerScheduleKind::LockRetry ) == 0 );
    CHECK( harness.probe.requests.size() == 1 );
}

TEST_CASE( "reentrant stop from a starting state prevents packaged process creation",
           "[livecapture][adb][supervisor][startup][reentrant]" )
{
    SupervisorHarness harness;
    QObject::connect( &harness.supervisor, &AdbServerSupervisor::stateChanged,
                      [ &harness ]( Generation generation, std::uint64_t,
                                    const AdbServerSupervisorSnapshot& snapshot ) {
                          if ( snapshot.status == AdbServerSupervisorStatus::Starting ) {
                              harness.supervisor.stop( generation );
                          }
                      } );

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    harness.startupLock.complete( 0, true );
    harness.probe.complete( 1, absentProbe() );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Stopped );
    CHECK( harness.launcher.launches.empty() );
    CHECK( harness.startupLock.wasReleased( 0 ) );
}

TEST_CASE( "reentrant stop while requesting key consent suppresses the stale consent signal",
           "[livecapture][adb][supervisor][keys][consent][reentrant]" )
{
    SupervisorHarness harness;
    harness.keyStore.inspection.state = AdbServerStandardKeyState::Absent;
    QObject::connect( &harness.supervisor, &AdbServerSupervisor::stateChanged,
                      [ &harness ]( Generation generation, std::uint64_t,
                                    const AdbServerSupervisorSnapshot& snapshot ) {
                          if ( snapshot.status
                               == AdbServerSupervisorStatus::AwaitingKeyGenerationConsent ) {
                              harness.supervisor.stop( generation );
                          }
                      } );

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, absentProbe() );
    harness.startupLock.complete( 0, true );
    harness.probe.complete( 1, absentProbe() );

    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Stopped );
    CHECK( harness.events.consentRequests.empty() );
    CHECK( harness.launcher.launches.empty() );
    CHECK( harness.startupLock.wasReleased( 0 ) );
}

TEST_CASE( "reentrant restart from ready state does not arm a stale health probe",
           "[livecapture][adb][supervisor][health][reentrant][generation]" )
{
    SupervisorHarness harness;
    QObject::connect(
        &harness.supervisor, &AdbServerSupervisor::stateChanged,
        [ &harness ]( Generation, std::uint64_t, const AdbServerSupervisorSnapshot& snapshot ) {
            if ( snapshot.generation == FirstGeneration
                 && snapshot.status == AdbServerSupervisorStatus::Ready ) {
                harness.supervisor.start( SecondGeneration );
            }
        } );

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, readyProbe() );

    CHECK( harness.supervisor.snapshot().generation == SecondGeneration );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Probing );
    CHECK( harness.scheduler.activeCount( AdbServerScheduleKind::HealthProbe ) == 0 );
    REQUIRE( harness.probe.requests.size() == 2 );
    harness.probe.complete( 1, readyProbe( ReplacementIdentity ) );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
}

TEST_CASE( "reentrant restart after server loss does not arm a stale reconnect",
           "[livecapture][adb][supervisor][reconnect][reentrant][generation]" )
{
    SupervisorHarness harness;
    QObject::connect(
        &harness.supervisor, &AdbServerSupervisor::stateChanged,
        [ &harness ]( Generation, std::uint64_t, const AdbServerSupervisorSnapshot& snapshot ) {
            if ( snapshot.generation == FirstGeneration
                 && snapshot.status == AdbServerSupervisorStatus::RetryWait ) {
                harness.supervisor.start( SecondGeneration );
            }
        } );

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, readyProbe() );
    harness.scheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.complete( 1, absentProbe( "server disappeared" ) );

    CHECK( harness.supervisor.snapshot().generation == SecondGeneration );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Probing );
    CHECK( harness.scheduler.activeCount( AdbServerScheduleKind::ReconnectBackoff ) == 0 );
    REQUIRE( harness.probe.requests.size() == 3 );
    harness.probe.complete( 2, readyProbe( ReplacementIdentity ) );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
}

TEST_CASE( "reentrant restart from failed state suppresses the stale error signal",
           "[livecapture][adb][supervisor][error][reentrant][generation]" )
{
    SupervisorHarness harness;
    QObject::connect(
        &harness.supervisor, &AdbServerSupervisor::stateChanged,
        [ &harness ]( Generation, std::uint64_t, const AdbServerSupervisorSnapshot& snapshot ) {
            if ( snapshot.generation == FirstGeneration
                 && snapshot.status == AdbServerSupervisorStatus::Failed ) {
                harness.supervisor.start( SecondGeneration );
            }
        } );

    harness.supervisor.start( FirstGeneration );
    harness.probe.complete( 0, failedProbe( "malformed smart-socket response" ) );

    CHECK( harness.supervisor.snapshot().generation == SecondGeneration );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Probing );
    CHECK( harness.events.errors.empty() );
    REQUIRE( harness.probe.requests.size() == 2 );
    harness.probe.complete( 1, readyProbe( ReplacementIdentity ) );
    CHECK( harness.supervisor.snapshot().status == AdbServerSupervisorStatus::Ready );
}

TEST_CASE( "invalid supervisor configuration uses configuration error taxonomy",
           "[livecapture][adb][supervisor][validation][error]" )
{
    auto config = supervisorConfig();
    config.packagedServerPath = QStringLiteral( "adb" );
    SupervisorHarness harness( config );

    harness.supervisor.start( FirstGeneration );

    REQUIRE( harness.supervisor.snapshot().error.has_value() );
    CHECK( harness.supervisor.snapshot().error->category
           == klogg::livecapture::ErrorCategory::Configuration );
    CHECK( harness.supervisor.snapshot().error->scope
           == klogg::livecapture::ErrorScope::Infrastructure );
    CHECK( harness.supervisor.snapshot().error->retryPolicy
           == klogg::livecapture::RetryPolicy::Never );
}

TEST_CASE( "startup lock never steals an old lease from a live process",
           "[livecapture][adb][supervisor][lock][stale][adapter]" )
{
    QTemporaryDir temporaryDirectory;
    REQUIRE( temporaryDirectory.isValid() );
    const auto lockPath = temporaryDirectory.filePath( QStringLiteral( "adb-server.lock" ) );
    QLockFile heldLock( lockPath );
    heldLock.setStaleLockTime( 0 );
    REQUIRE( heldLock.tryLock( 0 ) );
    QFile lockFile( lockPath );
    REQUIRE( lockFile.open( QIODevice::ReadOnly ) );
    REQUIRE( lockFile.setFileTime( QDateTime::currentDateTimeUtc().addSecs( -60 ),
                                   QFileDevice::FileModificationTime ) );
    lockFile.close();

    QtAdbServerStartupLock startupLock;
    std::optional<AdbServerStartupLockResult> result;
    const auto token
        = startupLock.acquire( lockPath, [ &result ]( AdbServerStartupLockResult completed ) {
              result = std::move( completed );
          } );

    QTRY_VERIFY( result.has_value() );
    CHECK( result->state == AdbServerStartupLockState::Contended );
    startupLock.cancel( token );
    heldLock.unlock();
}

TEST_CASE( "startup lock adapter distinguishes permanent path failures from contention",
           "[livecapture][adb][supervisor][lock][error][adapter]" )
{
    QTemporaryDir temporaryDirectory;
    REQUIRE( temporaryDirectory.isValid() );
    const auto blockerPath = temporaryDirectory.filePath( QStringLiteral( "not-a-directory" ) );
    QFile blocker( blockerPath );
    REQUIRE( blocker.open( QIODevice::WriteOnly ) );
    blocker.close();

    QtAdbServerStartupLock startupLock;
    std::optional<AdbServerStartupLockResult> result;
    startupLock.acquire(
        QDir( blockerPath ).filePath( QStringLiteral( "adb-server.lock" ) ),
        [ &result ]( AdbServerStartupLockResult completed ) { result = std::move( completed ); } );

    QTRY_VERIFY( result.has_value() );
    CHECK( result->state == AdbServerStartupLockState::Failed );
    CHECK_FALSE( result->diagnostic.empty() );
}

TEST_CASE( "standard ADB private keys are restricted to owner read and write permissions",
           "[livecapture][adb][supervisor][keys][security]" )
{
    QTemporaryDir temporaryDirectory;
    REQUIRE( temporaryDirectory.isValid() );
    const auto keyDirectory = temporaryDirectory.filePath( QStringLiteral( ".android" ) );
    REQUIRE( QDir().mkpath( keyDirectory ) );
    const auto keyPath = QDir( keyDirectory ).filePath( QStringLiteral( "adbkey" ) );
    QFile keyFile( keyPath );
    REQUIRE( keyFile.open( QIODevice::WriteOnly ) );
    REQUIRE( keyFile.write( "private-key-material" ) > 0 );
    keyFile.close();
    REQUIRE( QFile::setPermissions( keyPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                 | QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                                 | QFileDevice::ReadOther ) );

    StandardAdbKeyStore keyStore( keyPath );
    const auto inspection = keyStore.inspectStandardKey();

    CHECK( inspection.state == AdbServerStandardKeyState::Present );
    const auto permissions = QFileInfo( keyPath ).permissions();
    CHECK( permissions.testFlag( QFileDevice::ReadOwner ) );
    CHECK( permissions.testFlag( QFileDevice::WriteOwner ) );
    const auto nonOwnerPermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup
                                     | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                     | QFileDevice::WriteOther | QFileDevice::ExeOther;
    CHECK( ( permissions & nonOwnerPermissions ) == QFileDevice::Permissions{} );
}
