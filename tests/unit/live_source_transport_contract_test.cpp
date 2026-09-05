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

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>

#include "adblogcatsource.h"
#include "ioslogprocesstransport.h"
#include "livesourcetransport.h"
#include "livestate.h"
#include "streaminglogdata.h"
#include "test_utils.h"

namespace {
using Generation = klogg::livecapture::Generation;
using ClearRequestId = LiveSourceTransport::ClearRequestId;

static_assert(
    std::is_same<decltype( std::declval<const LiveSourceTransport&>().statistics() ),
                 klogg::livecapture::LiveDataStatistics>::value,
    "All live-source transports must expose the same source-neutral statistics snapshot." );

class DeterministicLiveSourceTransport final : public LiveSourceTransport {
public:
    explicit DeterministicLiveSourceTransport( bool failStart = false )
        : failStart_( failStart )
    {
    }

    void start( Generation generation ) override
    {
        startGenerations.push_back( generation );
        Q_EMIT stateChanged( generation, State::Connecting );
        if ( failStart_ ) {
            lastError_ = QStringLiteral( "deterministic-start-failure" );
            Q_EMIT stateChanged( generation, State::Error );
            Q_EMIT errorOccurred( generation, lastError_ );
        }
    }

    void stop( Generation generation ) override
    {
        stopGenerations.push_back( generation );
        if ( emitRetiredCallbacksDuringStop ) {
            Q_EMIT stateChanged( generation, State::Connected );
            Q_EMIT bytesReceived( generation, QByteArrayLiteral( "retired-during-stop\n" ) );
            Q_EMIT errorOccurred( generation, QStringLiteral( "intentional-cancellation" ) );
        }
        Q_EMIT stateChanged( generation, State::Disconnected );
    }

    void clearRemoteAsync( Generation generation, ClearRequestId requestId ) override
    {
        clearRequests.push_back( { generation, requestId } );
    }

    QString lastError() const override
    {
        return lastError_;
    }

    std::optional<klogg::livecapture::LiveSourceError> lastStructuredError() const override
    {
        return structuredError;
    }

    void publishState( Generation generation, State state )
    {
        Q_EMIT stateChanged( generation, state );
    }

    void publishBytes( Generation generation, QByteArray bytes )
    {
        Q_EMIT bytesReceived( generation, bytes );
    }

    void publishError( Generation generation, QString error )
    {
        lastError_ = error;
        Q_EMIT errorOccurred( generation, error );
    }

    void publishTerminalError( Generation generation,
                               klogg::livecapture::LiveSourceError error )
    {
        structuredError = std::move( error );
        lastError_ = QString::fromStdString( structuredError->message );
        QPointer<DeterministicLiveSourceTransport> guard( this );
        Q_EMIT stateChanged( generation, State::Error );
        if ( guard ) {
            Q_EMIT guard->errorOccurred( generation, lastError_ );
        }
    }

    void completeClear( Generation generation, ClearRequestId requestId, bool succeeded,
                        QString error = {} )
    {
        Q_EMIT clearRemoteFinished( generation, requestId, succeeded, error );
    }

    void beginStatisticsGeneration( Generation generation )
    {
        resetStatistics( generation );
    }

    void recordDeliveredForTest( Generation generation, std::size_t byteCount )
    {
        recordDeliveredChunk( generation, byteCount );
    }

    struct ClearRequest {
        Generation generation;
        ClearRequestId requestId;

        bool operator==( const ClearRequest& other ) const
        {
            return generation == other.generation && requestId == other.requestId;
        }
    };

    bool emitRetiredCallbacksDuringStop = false;
    std::optional<klogg::livecapture::LiveSourceError> structuredError;
    std::vector<Generation> startGenerations;
    std::vector<Generation> stopGenerations;
    std::vector<ClearRequest> clearRequests;

private:
    bool failStart_ = false;
    QString lastError_;
};

class DeferredProcessLiveSourceTransport final : public ProcessLiveSourceTransport {
public:
    bool hasPendingStart() const
    {
        return pendingProcess_ != nullptr;
    }

    int startRequestCount() const
    {
        return startRequestCount_;
    }

    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "ping" ),
                 { QStringLiteral( "-n" ),
                   QStringLiteral( "60" ),
                   QStringLiteral( "127.0.0.1" ) } };
#else
        return { QStringLiteral( "sleep" ), { QStringLiteral( "60" ) } };
#endif
    }

    Command clearCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "cmd" ),
                 { QStringLiteral( "/c" ),
                   QStringLiteral( "exit" ),
                   QStringLiteral( "0" ) } };
#else
        return { QStringLiteral( "true" ), {} };
#endif
    }

protected:
    void startProcessAsync( QProcess& process ) override
    {
        pendingProcess_ = &process;
        ++startRequestCount_;
    }

private:
    QProcess* pendingProcess_ = nullptr;
    int startRequestCount_ = 0;
};

class FailingClearProcessLiveSourceTransport final : public ProcessLiveSourceTransport {
public:
    Command streamingCommand() const override
    {
#ifdef Q_OS_WIN
        return { QStringLiteral( "ping" ),
                 { QStringLiteral( "-n" ),
                   QStringLiteral( "60" ),
                   QStringLiteral( "127.0.0.1" ) } };
#else
        return { QStringLiteral( "sleep" ), { QStringLiteral( "60" ) } };
#endif
    }

    Command clearCommand() const override
    {
        return { QStringLiteral( "/path/that/does/not/exist/klogg-clear" ), {} };
    }
};

class DeferredIosLogProcessTransport final : public IosLogProcessTransport {
public:
    using IosLogProcessTransport::IosLogProcessTransport;

    void filterReceivedBytesForTest( QByteArray& data )
    {
        filterReceivedBytes( data );
    }

protected:
    void startProcessAsync( QProcess& ) override {}
};

class RecordingLiveSourceTransportFactory final : public LiveSourceTransportFactory {
public:
    explicit RecordingLiveSourceTransportFactory( bool failStart = false )
        : failStart_( failStart )
    {
    }

    std::unique_ptr<LiveSourceTransport>
    create( const LiveSourceTransportConfig& config ) const override
    {
        requestedConfigs.push_back( config );
        auto transport = std::make_unique<DeterministicLiveSourceTransport>( failStart_ );
        lastTransport = transport.get();
        return transport;
    }

    bool failStart_ = false;
    mutable DeterministicLiveSourceTransport* lastTransport = nullptr;
    mutable std::vector<LiveSourceTransportConfig> requestedConfigs;
};

QString makeCaptureId()
{
    return QUuid::createUuid().toString( QUuid::WithoutBraces );
}
} // namespace

TEST_CASE( "ProcessLiveSourceTransport start is nonblocking and immediately Connecting",
           "[livecapture][transport][contract]" )
{
    DeferredProcessLiveSourceTransport transport;
    std::vector<std::pair<Generation, LiveSourceTransport::State>> states;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation generation, LiveSourceTransport::State state ) {
                          states.emplace_back( generation, state );
                      } );

    constexpr Generation generation = 41u;
    transport.start( generation );

    REQUIRE( transport.hasPendingStart() );
    REQUIRE( states
             == std::vector<std::pair<Generation, LiveSourceTransport::State>>{
                 { generation, LiveSourceTransport::State::Connecting } } );

    transport.stop( generation );
}

TEST_CASE( "ProcessLiveSourceTransport does not start after a reentrant stop",
           "[livecapture][transport][contract][generation]" )
{
    DeferredProcessLiveSourceTransport transport;
    constexpr Generation generation = 50u;

    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ & ]( Generation observedGeneration, LiveSourceTransport::State state ) {
                          if ( observedGeneration == generation
                               && state == LiveSourceTransport::State::Connecting ) {
                              transport.stop( generation );
                          }
                      } );

    transport.start( generation );

    REQUIRE( transport.startRequestCount() == 0 );
}

TEST_CASE( "ProcessLiveSourceTransport does not resume a superseded reentrant start",
           "[livecapture][transport][contract][generation]" )
{
    DeferredProcessLiveSourceTransport transport;
    constexpr Generation firstGeneration = 51u;
    constexpr Generation replacementGeneration = 52u;
    bool replaced = false;

    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ & ]( Generation generation, LiveSourceTransport::State state ) {
                          if ( !replaced && generation == firstGeneration
                               && state == LiveSourceTransport::State::Connecting ) {
                              replaced = true;
                              transport.stop( firstGeneration );
                              transport.start( replacementGeneration );
                          }
                      } );

    transport.start( firstGeneration );

    REQUIRE( replaced );
    REQUIRE( transport.startRequestCount() == 1 );
    transport.stop( replacementGeneration );
}

TEST_CASE( "IosLogProcessTransport duplicate generation start preserves stream parsing state",
           "[livecapture][transport][contract][ios]" )
{
#ifdef Q_OS_MAC
    DeferredIosLogProcessTransport transport( QStringLiteral( "pymobiledevice3" ),
                                              QStringLiteral( "device" ), QString{}, true );
    constexpr Generation generation = 61u;
    transport.start( generation );

    QByteArray ordinaryBytes = QByteArrayLiteral( "ordinary-log-data\n" );
    transport.filterReceivedBytesForTest( ordinaryBytes );
    REQUIRE_FALSE( ordinaryBytes.isEmpty() );

    transport.start( generation );
    QByteArray prefixLikeBytes = QByteArrayLiteral( "^D" );
    transport.filterReceivedBytesForTest( prefixLikeBytes );
    REQUIRE( prefixLikeBytes == QByteArrayLiteral( "^D" ) );

    transport.stop( generation );
#else
    SUCCEED( "PTY prefix parsing is macOS-only." );
#endif
}

TEST_CASE( "ProcessLiveSourceTransport stop is idempotent nonblocking and intentional",
           "[livecapture][transport][contract]" )
{
    DeferredProcessLiveSourceTransport transport;
    std::vector<std::pair<Generation, LiveSourceTransport::State>> states;
    std::vector<QString> errors;
    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &states ]( Generation generation, LiveSourceTransport::State state ) {
                          states.emplace_back( generation, state );
                      } );
    QObject::connect(
        &transport, &LiveSourceTransport::errorOccurred,
        [ &errors ]( Generation, const QString& error ) { errors.push_back( error ); } );

    constexpr Generation generation = 77u;
    transport.start( generation );
    transport.stop( generation );
    transport.stop( generation );

    REQUIRE( errors.empty() );
    REQUIRE( states.back()
             == std::pair<Generation, LiveSourceTransport::State>{
                 generation, LiveSourceTransport::State::Disconnected } );
    REQUIRE( std::count( states.begin(), states.end(),
                         std::pair<Generation, LiveSourceTransport::State>{
                             generation, LiveSourceTransport::State::Disconnected } )
             == 1 );
}

TEST_CASE( "LiveSourceTransport clear is asynchronous and correlates results by request id",
           "[livecapture][transport][contract][clear]" )
{
    DeterministicLiveSourceTransport transport;
    struct Result {
        Generation generation;
        ClearRequestId requestId;
        bool succeeded;
        QString error;
    };
    std::vector<Result> results;
    QObject::connect( &transport, &LiveSourceTransport::clearRemoteFinished,
                      [ &results ]( Generation generation, ClearRequestId requestId, bool succeeded,
                                    const QString& error ) {
                          results.push_back( Result{ generation, requestId, succeeded, error } );
                      } );

    constexpr Generation generation = 9u;
    constexpr ClearRequestId first = 1001u;
    constexpr ClearRequestId second = 1002u;
    transport.clearRemoteAsync( generation, first );
    transport.clearRemoteAsync( generation, second );

    REQUIRE( transport.clearRequests
             == std::vector<DeterministicLiveSourceTransport::ClearRequest>{
                 { generation, first }, { generation, second } } );
    REQUIRE( results.empty() );

    transport.completeClear( generation, second, false, QStringLiteral( "clear-failed" ) );
    transport.completeClear( generation, first, true );

    REQUIRE( results.size() == 2u );
    REQUIRE( results.at( 0 ).requestId == second );
    REQUIRE_FALSE( results.at( 0 ).succeeded );
    REQUIRE( results.at( 0 ).error == QStringLiteral( "clear-failed" ) );
    REQUIRE( results.at( 1 ).requestId == first );
    REQUIRE( results.at( 1 ).succeeded );
    REQUIRE( results.at( 1 ).error.isEmpty() );
}

TEST_CASE( "LiveSourceTransport statistics reject delivery from retired generations",
           "[livecapture][transport][contract][generation][statistics]" )
{
    DeterministicLiveSourceTransport transport;
    constexpr Generation retiredGeneration = 10u;
    constexpr Generation activeGeneration = 11u;

    transport.beginStatisticsGeneration( retiredGeneration );
    transport.recordDeliveredForTest( retiredGeneration, 3u );
    transport.beginStatisticsGeneration( activeGeneration );
    transport.recordDeliveredForTest( retiredGeneration, 7u );
    transport.recordDeliveredForTest( activeGeneration, 2u );

    const auto statistics = transport.statistics();
    REQUIRE( statistics.generation == activeGeneration );
    REQUIRE( statistics.receivedBytes == 2u );
    REQUIRE( statistics.receivedChunks == 1u );
    REQUIRE( statistics.deliveredBytes == 2u );
    REQUIRE( statistics.deliveredChunks == 1u );
}

TEST_CASE( "ProcessLiveSourceTransport clear returns before its process result",
           "[livecapture][transport][contract][clear][process]" )
{
    DeferredProcessLiveSourceTransport transport;
    SafeQSignalSpy clearSpy( &transport, &LiveSourceTransport::clearRemoteFinished );

    constexpr Generation generation = 12u;
    constexpr ClearRequestId requestId = 5001u;
    transport.clearRemoteAsync( generation, requestId );

    REQUIRE( clearSpy.count() == 0 );
    REQUIRE( clearSpy.safeWait( 3000 ) );
    REQUIRE( clearSpy.count() == 1 );
    REQUIRE( clearSpy.at( 0 ).at( 0 ).toULongLong() == generation );
    REQUIRE( clearSpy.at( 0 ).at( 1 ).toULongLong() == requestId );
    REQUIRE( clearSpy.at( 0 ).at( 2 ).toBool() );
    REQUIRE( clearSpy.at( 0 ).at( 3 ).toString().isEmpty() );
}

TEST_CASE( "Asynchronous clear failures remain request-local",
           "[livecapture][transport][contract][clear][process]" )
{
    FailingClearProcessLiveSourceTransport transport;
    SafeQSignalSpy clearSpy( &transport, &LiveSourceTransport::clearRemoteFinished );

    transport.clearRemoteAsync( 17u, 6001u );

    REQUIRE( clearSpy.safeWait( 3000 ) );
    REQUIRE_FALSE( clearSpy.at( 0 ).at( 2 ).toBool() );
    REQUIRE_FALSE( clearSpy.at( 0 ).at( 3 ).toString().isEmpty() );
    REQUIRE( transport.lastError().isEmpty() );
}

TEST_CASE( "AdbLogcatSource uses the same injected async start API initially and on reconnect",
           "[livecapture][transport][factory][source]" )
{
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::IosLogStream;
    sessionData.adbExecutable = QStringLiteral( "pymobiledevice3" );
    sessionData.deviceSerial = QStringLiteral( "ios-device" );
    sessionData.extraArgs = QStringLiteral( "--network" );
    sessionData.ansiOutputEnabled = true;

    AdbLogcatSource source( sessionData, {}, factory );
    source.connectSource();
    REQUIRE( factory.lastTransport != nullptr );
    REQUIRE( factory.lastTransport->startGenerations.size() == 1u );
    const auto initialGeneration = factory.lastTransport->startGenerations.front();

    source.reconnectSource();
    REQUIRE( factory.lastTransport->startGenerations.size() == 2u );
    REQUIRE( factory.lastTransport->startGenerations.back() > initialGeneration );
    REQUIRE( factory.requestedConfigs.size() == 1u );
    const auto& requestedConfig = factory.requestedConfigs.front();
    REQUIRE( requestedConfig.sourceType == LiveLogSourceType::IosLogStream );
    REQUIRE( requestedConfig.executable == QStringLiteral( "pymobiledevice3" ) );
    REQUIRE( requestedConfig.deviceId == QStringLiteral( "ios-device" ) );
    REQUIRE( requestedConfig.extraArgs == QStringLiteral( "--network" ) );
    REQUIRE( requestedConfig.ansiOutputEnabled );
}

TEST_CASE( "AdbLogcatSource coalesces connect requests while startup is pending",
           "[livecapture][transport][factory][source][start]" )
{
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSource source( {}, {}, factory );

    REQUIRE( source.connectSource() );
    REQUIRE( source.connectSource() );
    REQUIRE( factory.lastTransport != nullptr );
    REQUIRE( factory.lastTransport->startGenerations.size() == 1u );
}

TEST_CASE( "AdbLogcatSource stops terminal delivery after synchronous source destruction",
           "[livecapture][transport][factory][source][error][reentrant][destroy][ios]" )
{
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::IosLogStream;
    auto source = std::make_unique<AdbLogcatSource>( sessionData, nullptr, factory );
    QPointer<AdbLogcatSource> sourceGuard( source.get() );
    int sourceDiagnostics = 0;
    int controllerErrorStates = 0;
    int controllerFailures = 0;

    source->setControllerCallbacks(
        {},
        [ &controllerErrorStates ]( Generation, LiveSourceTransport::State state ) {
            if ( state == LiveSourceTransport::State::Error ) {
                ++controllerErrorStates;
            }
        },
        [ &controllerFailures ]( Generation, klogg::livecapture::LiveSourceError ) {
            ++controllerFailures;
        } );
    QObject::connect( source.get(), &AdbLogcatSource::errorOccurred,
                      [ &sourceDiagnostics ]( const QString& ) { ++sourceDiagnostics; } );
    QObject::connect( source.get(), &AdbLogcatSource::stateChanged, source.get(),
                      [ &source ]( AdbLogcatSource::State state ) {
                          if ( state == AdbLogcatSource::State::Error ) {
                              source.reset();
                          }
                      } );

    REQUIRE( source->connectSource() );
    REQUIRE( factory.lastTransport != nullptr );
    const auto generation = factory.lastTransport->startGenerations.back();
    const klogg::livecapture::LiveSourceError terminalError{
        klogg::livecapture::ErrorCategory::Backend,
        "ios-terminal-destruction-test",
        klogg::livecapture::ErrorScope::Stream,
        klogg::livecapture::RetryPolicy::Never,
        "The native iOS stream terminated.",
        "source deleted by state observer"
    };

    factory.lastTransport->publishTerminalError( generation, terminalError );

    CHECK( sourceGuard.isNull() );
    CHECK( sourceDiagnostics == 0 );
    CHECK( controllerErrorStates == 0 );
    CHECK( controllerFailures == 0 );
}

TEST_CASE( "AdbLogcatSource finalizes input before a reentrant terminal disconnect",
           "[livecapture][transport][factory][source][error][ordering][reentrant][ios]" )
{
    QTemporaryDir captureRoot;
    REQUIRE( captureRoot.isValid() );
    auto logData = std::make_shared<StreamingLogData>( makeCaptureId(), captureRoot.path() );
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::IosLogStream;
    AdbLogcatSource source( sessionData, logData, factory );
    int sourceDiagnostics = 0;
    int controllerErrorStates = 0;
    int controllerFailures = 0;

    source.setControllerCallbacks(
        {},
        [ &controllerErrorStates ]( Generation, LiveSourceTransport::State state ) {
            if ( state == LiveSourceTransport::State::Error ) {
                ++controllerErrorStates;
            }
        },
        [ &controllerFailures ]( Generation, klogg::livecapture::LiveSourceError ) {
            ++controllerFailures;
        } );
    QObject::connect( &source, &AdbLogcatSource::errorOccurred, &source,
                      [ & ]( const QString& ) {
                          ++sourceDiagnostics;
                          CHECK( logData->getNbLine().get() == 1u );
                          source.disconnectSource();
                      } );

    REQUIRE( source.connectSource() );
    REQUIRE( factory.lastTransport != nullptr );
    const auto generation = factory.lastTransport->startGenerations.back();
    factory.lastTransport->publishBytes( generation, QByteArrayLiteral( "partial terminal line" ) );
    REQUIRE( logData->getNbLine().get() == 0u );
    const klogg::livecapture::LiveSourceError terminalError{
        klogg::livecapture::ErrorCategory::Backend,
        "ios-terminal-reentrant-stop-test",
        klogg::livecapture::ErrorScope::Stream,
        klogg::livecapture::RetryPolicy::Never,
        "The native iOS stream terminated.",
        "source disconnected by diagnostic observer"
    };

    factory.lastTransport->publishTerminalError( generation, terminalError );

    CHECK( sourceDiagnostics == 1 );
    CHECK( controllerErrorStates == 0 );
    CHECK( controllerFailures == 0 );
    CHECK( source.state() == AdbLogcatSource::State::Disconnected );
    CHECK( factory.lastTransport->stopGenerations
           == std::vector<Generation>{ generation } );
}

TEST_CASE( "AdbLogcatSource serializes restart behind an in-flight remote clear",
           "[livecapture][transport][factory][source][clear]" )
{
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::AdbLogcat;
    AdbLogcatSource source( sessionData, {}, factory );

    REQUIRE( source.connectSource() );
    REQUIRE( factory.lastTransport != nullptr );
    const auto initialGeneration = factory.lastTransport->startGenerations.back();
    factory.lastTransport->publishState( initialGeneration, LiveSourceTransport::State::Connected );

    REQUIRE( source.clearAndRestart() );
    REQUIRE( source.clearAndRestart() );
    REQUIRE( factory.lastTransport->clearRequests.size() == 1u );
    const auto clearRequest = factory.lastTransport->clearRequests.front();

    REQUIRE( source.connectSource() );
    REQUIRE( factory.lastTransport->startGenerations.size() == 1u );

    factory.lastTransport->completeClear( clearRequest.generation, clearRequest.requestId, true );
    REQUIRE( factory.lastTransport->startGenerations.size() == 2u );
    REQUIRE( factory.lastTransport->startGenerations.back() > initialGeneration );
}

TEST_CASE( "AdbLogcatSource reports asynchronous clear failures on a dedicated signal",
           "[livecapture][transport][factory][source][clear]" )
{
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSource source( {}, {}, factory );
    SafeQSignalSpy clearFailedSpy( &source, &AdbLogcatSource::clearFailed );

    REQUIRE( source.connectSource() );
    REQUIRE( factory.lastTransport != nullptr );
    const auto generation = factory.lastTransport->startGenerations.back();
    factory.lastTransport->publishState( generation, LiveSourceTransport::State::Connected );
    REQUIRE( source.clearAndRestart() );
    REQUIRE( factory.lastTransport->clearRequests.size() == 1u );

    const auto request = factory.lastTransport->clearRequests.front();
    factory.lastTransport->completeClear( request.generation, request.requestId, false,
                                          QStringLiteral( "clear-failed" ) );

    REQUIRE( clearFailedSpy.count() == 1 );
    REQUIRE( clearFailedSpy.at( 0 ).at( 0 ).toString() == QStringLiteral( "clear-failed" ) );
    REQUIRE( source.state() == AdbLogcatSource::State::Error );
}

TEST_CASE( "AdbLogcatSource ignores callbacks from retired transport generations",
           "[livecapture][transport][factory][source][generation]" )
{
    QTemporaryDir captureRoot;
    REQUIRE( captureRoot.isValid() );
    auto logData = std::make_shared<StreamingLogData>( makeCaptureId(), captureRoot.path() );
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::AdbLogcat;

    AdbLogcatSource source( sessionData, logData, factory );
    std::vector<QString> sourceErrors;
    QObject::connect(
        &source, &AdbLogcatSource::errorOccurred,
        [ &sourceErrors ]( const QString& error ) { sourceErrors.push_back( error ); } );

    source.connectSource();
    REQUIRE( factory.lastTransport != nullptr );
    const auto retiredGeneration = factory.lastTransport->startGenerations.back();
    source.disconnectSource();
    source.connectSource();
    const auto currentGeneration = factory.lastTransport->startGenerations.back();
    REQUIRE( currentGeneration > retiredGeneration );
    const auto currentState = source.state();

    factory.lastTransport->publishState( retiredGeneration, LiveSourceTransport::State::Connected );
    factory.lastTransport->publishBytes( retiredGeneration, QByteArrayLiteral( "stale\n" ) );
    factory.lastTransport->publishError( retiredGeneration, QStringLiteral( "stale-error" ) );

    REQUIRE( source.state() == currentState );
    REQUIRE( sourceErrors.empty() );
    REQUIRE( logData->getNbLine().get() == 0u );

    factory.lastTransport->publishState( currentGeneration, LiveSourceTransport::State::Connected );
    factory.lastTransport->publishBytes( currentGeneration, QByteArrayLiteral( "current\n" ) );
    REQUIRE( source.state() == AdbLogcatSource::State::Connected );
    REQUIRE( logData->getNbLine().get() == 1u );
}

TEST_CASE( "AdbLogcatSource intentional stop emits no error even with synchronous callbacks",
           "[livecapture][transport][factory][source][stop]" )
{
    RecordingLiveSourceTransportFactory factory;
    AdbLogcatSessionData sessionData;
    AdbLogcatSource source( sessionData, {}, factory );
    std::vector<QString> sourceErrors;
    QObject::connect(
        &source, &AdbLogcatSource::errorOccurred,
        [ &sourceErrors ]( const QString& error ) { sourceErrors.push_back( error ); } );

    source.connectSource();
    REQUIRE( factory.lastTransport != nullptr );
    factory.lastTransport->emitRetiredCallbacksDuringStop = true;
    source.disconnectSource();
    source.disconnectSource();

    REQUIRE( factory.lastTransport->stopGenerations.size() == 1u );
    REQUIRE( sourceErrors.empty() );
    REQUIRE( source.state() == AdbLogcatSource::State::Disconnected );
}
