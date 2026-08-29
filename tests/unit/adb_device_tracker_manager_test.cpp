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
#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "adbdeviceinfo.h"
#include "adbdevicetracker.h"
#include "adbinfrastructuremanager.h"
#include "adbserversupervisor.h"
#include "adbsmartsocketclient.h"
#include "adbtrackeddeviceprovider.h"
#include "containers.h"
#include "devicediscovery.h"
#include "livestate.h"

namespace {

using namespace std::chrono_literals;
using klogg::livecapture::ErrorCategory;
using klogg::livecapture::ErrorScope;
using klogg::livecapture::Generation;
using klogg::livecapture::InfrastructureOwnership;
using klogg::livecapture::InfrastructureStatus;
using klogg::livecapture::RetryPolicy;
using namespace klogg::livecapture::adb;
using DomainAdbDeviceInfo = klogg::livecapture::adb::AdbDeviceInfo;
using DomainAdbDeviceState = klogg::livecapture::adb::AdbDeviceState;

constexpr std::uint32_t SupportedProtocolVersion = 0x29u;
constexpr auto FirstServerIdentity = "adb-server:first";
constexpr auto ReplacementServerIdentity = "adb-server:replacement";

bool drainEventsUntil( const std::function<bool()>& predicate )
{
    for ( int iteration = 0; iteration < 10000 && !predicate(); ++iteration ) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents( QEventLoop::AllEvents );
    }
    return predicate();
}

std::optional<int> parseRequestLength( const QByteArray& header )
{
    if ( header.size() != 4 ) {
        return std::nullopt;
    }

    bool okay = false;
    const auto length = header.toInt( &okay, 16 );
    if ( !okay || length < 0 ) {
        return std::nullopt;
    }
    return length;
}

QByteArray hostFrame( const QByteArray& payload )
{
    REQUIRE( payload.size() <= 0xffff );
    return QByteArray::number( payload.size(), 16 ).rightJustified( 4, '0' ).toUpper() + payload;
}

class TrackDevicesServer final : public QObject {
public:
    explicit TrackDevicesServer( QObject* parent = nullptr )
        : QObject( parent )
    {
        REQUIRE( server_.listen( QHostAddress::LocalHost, 0 ) );
        QObject::connect( &server_, &QTcpServer::newConnection, this,
                          [ this ] { acceptConnections(); } );
    }

    quint16 port() const
    {
        return server_.serverPort();
    }

    klogg::ContainerIndex connectionCount() const
    {
        return sockets_.size();
    }

    klogg::ContainerIndex requestCount() const
    {
        return requests_.size();
    }

    const QByteArray& requestAt( klogg::ContainerIndex index ) const
    {
        return requests_.at( index );
    }

    void sendTrackAccepted( klogg::ContainerIndex connectionIndex, const QByteArray& firstSnapshot )
    {
        send( connectionIndex, QByteArrayLiteral( "OKAY" ) + hostFrame( firstSnapshot ) );
    }

    void sendSnapshot( klogg::ContainerIndex connectionIndex, const QByteArray& snapshot )
    {
        send( connectionIndex, hostFrame( snapshot ) );
    }

    void sendFailure( klogg::ContainerIndex connectionIndex, const QByteArray& diagnostic )
    {
        send( connectionIndex, QByteArrayLiteral( "FAIL" ) + hostFrame( diagnostic ) );
    }

    void closeConnection( klogg::ContainerIndex connectionIndex )
    {
        auto* const socket = sockets_.at( connectionIndex );
        REQUIRE( socket != nullptr );
        socket->disconnectFromHost();
    }

private:
    void acceptConnections()
    {
        while ( server_.hasPendingConnections() ) {
            auto* const socket = server_.nextPendingConnection();
            REQUIRE( socket != nullptr );
            const auto connectionIndex = sockets_.size();
            sockets_.append( socket );
            buffers_.append( QByteArray{} );
            QObject::connect(
                socket, &QTcpSocket::readyRead, this,
                [ this, socket, connectionIndex ] { consumeRequest( *socket, connectionIndex ); } );
        }
    }

    void consumeRequest( QTcpSocket& socket, klogg::ContainerIndex connectionIndex )
    {
        auto& bytes = buffers_[ connectionIndex ];
        bytes.append( socket.readAll() );
        if ( bytes.size() < 4 ) {
            return;
        }

        const auto length = parseRequestLength( bytes.left( 4 ) );
        REQUIRE( length.has_value() );
        if ( bytes.size() < 4 + *length ) {
            return;
        }

        requests_.append( bytes.mid( 4, *length ) );
        bytes.remove( 0, 4 + *length );
    }

    void send( klogg::ContainerIndex connectionIndex, const QByteArray& bytes )
    {
        auto* const socket = sockets_.at( connectionIndex );
        REQUIRE( socket != nullptr );
        REQUIRE( socket->write( bytes ) == bytes.size() );
        socket->flush();
    }

private:
    QTcpServer server_;
    QVector<QTcpSocket*> sockets_;
    QVector<QByteArray> buffers_;
    QVector<QByteArray> requests_;
};

class DefaultSocketFactory final : public AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership is transferred to the supplied Qt parent.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new QTcpSocket( parent );
    }
};

class PassiveDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind, int, QObject*,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        callbacks_.push_back( Entry{ token, std::move( callback ), true } );
        return token;
    }

    void cancelDeadline( DeadlineToken token ) override
    {
        for ( auto& entry : callbacks_ ) {
            if ( entry.token == token ) {
                entry.active = false;
            }
        }
    }

private:
    struct Entry {
        DeadlineToken token{ 0 };
        std::function<void()> callback;
        bool active{ true };
    };

    DeadlineToken nextToken_{ 0 };
    std::vector<Entry> callbacks_;
};

class ManualProbe final : public AdbServerProbe {
public:
    struct Request {
        AdbServerToken token{ 0 };
        Callback callback;
        bool active{ true };
    };

    AdbServerToken probe( const AdbServerEndpoint& endpoint, Callback callback ) override
    {
        CHECK( endpoint.address == QHostAddress::LocalHost );
        CHECK( endpoint.port == 5037 );
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, std::move( callback ), true } );
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

    void completeReady( std::size_t index, std::string identity = FirstServerIdentity )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( AdbServerProbeResult{ AdbServerProbeState::Ready,
                                        SupportedProtocolVersion,
                                        { "shell_v2", "cmd" },
                                        std::move( identity ),
                                        {} } );
    }

    void completeAbsent( std::size_t index, std::string diagnostic = "server unavailable" )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( AdbServerProbeResult{
            AdbServerProbeState::Absent, 0u, {}, {}, std::move( diagnostic ) } );
    }

    void completeFailed( std::size_t index, std::string diagnostic = "probe failed" )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( AdbServerProbeResult{
            AdbServerProbeState::Failed, 0u, {}, {}, std::move( diagnostic ) } );
    }

    std::vector<Request> requests;

private:
    AdbServerToken nextToken_{ 0 };
};

class ManualLauncher final : public AdbServerLauncher {
public:
    struct Request {
        AdbServerToken token{ 0 };
        AdbServerLaunchRequest request;
        Callback callback;
    };

    AdbServerToken launch( const AdbServerLaunchRequest& request, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, request, std::move( callback ) } );
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

    std::vector<Request> requests;
    std::vector<AdbServerToken> cleanupTokens;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class ManualStartupLock final : public AdbServerStartupLock {
public:
    struct Request {
        AdbServerToken token{ 0 };
        Callback callback;
        bool active{ true };
    };

    AdbServerToken acquire( const QString&, Callback callback ) override
    {
        const auto token = ++nextToken_;
        requests.push_back( Request{ token, std::move( callback ), true } );
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

    void completeAcquired( std::size_t index )
    {
        REQUIRE( index < requests.size() );
        REQUIRE( requests.at( index ).active );
        requests.at( index ).active = false;
        const auto callback = requests.at( index ).callback;
        callback( AdbServerStartupLockResult{ AdbServerStartupLockState::Acquired, {} } );
    }

    std::vector<Request> requests;
    std::vector<AdbServerToken> releasedTokens;

private:
    AdbServerToken nextToken_{ 0 };
};

class FakeKeyStore final : public AdbKeyStore {
public:
    AdbServerKeyInspection inspectStandardKey() override
    {
        ++inspectionCount;
        return inspection;
    }

    AdbServerKeyGenerationResult generateStandardKey() override
    {
        ++generationCount;
        return generationResult;
    }

    AdbServerKeyInspection inspection{ AdbServerStandardKeyState::Present, {} };
    AdbServerKeyGenerationResult generationResult{ true, {} };
    int inspectionCount{ 0 };
    int generationCount{ 0 };
};

class ManualServerScheduler final : public AdbServerScheduler {
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

    void fire( AdbServerScheduleKind kind )
    {
        auto found
            = std::find_if( entries.rbegin(), entries.rend(), [ kind ]( const Entry& entry ) {
                  return entry.active && entry.kind == kind;
              } );
        REQUIRE( found != entries.rend() );
        found->active = false;
        const auto callback = found->callback;
        callback();
    }

    void fireEvenIfCancelled( AdbServerToken token )
    {
        const auto found
            = std::find_if( entries.begin(), entries.end(),
                            [ token ]( const Entry& entry ) { return entry.token == token; } );
        REQUIRE( found != entries.end() );
        const auto callback = found->callback;
        callback();
    }

    std::vector<Entry> entries;

private:
    AdbServerToken nextToken_{ 0 };
};

AdbSmartSocketClientConfig clientConfig( quint16 port )
{
    AdbSmartSocketClientConfig config;
    config.serverAddress = QHostAddress::LocalHost;
    config.serverPort = port;
    config.maxReadChunkBytes = 64;
    config.maxWriteChunkBytes = 64;
    config.maxHostReplyBytes = 0xffffu;
    config.connectTimeoutMs = 3000;
    config.writeTimeoutMs = 3000;
    config.readTimeoutMs = 3000;
    return config;
}

AdbInfrastructureManagerConfig managerConfig()
{
    AdbInfrastructureManagerConfig config;
    config.server.endpoint = AdbServerEndpoint{ QHostAddress::LocalHost, 5037 };
    config.server.packagedServerPath = QStringLiteral( "/opt/klogg/libexec/adb" );
    config.server.lockPath = QStringLiteral( "/test-user/klogg/adb-server.lock" );
    config.server.minimumProtocolVersion = SupportedProtocolVersion;
    config.server.requiredFeatures = { "shell_v2" };
    config.server.healthProbeInterval = 1s;
    config.server.reconnectBackoff = { 10ms, 20ms, 40ms };
    config.tracker.reconnectBackoff = { 5ms, 10ms, 20ms };
    return config;
}

struct ClientOperation {
    Generation generation{ 0 };
    AdbSmartSocketClient::OperationId operationId{ 0 };
};

class ClientOperationRecorder final {
public:
    explicit ClientOperationRecorder( AdbSmartSocketClient& client )
    {
        QObject::connect(
            &client, &AdbSmartSocketClient::operationConnected,
            [ this ]( Generation generation, AdbSmartSocketClient::OperationId operationId ) {
                operations.push_back( ClientOperation{ generation, operationId } );
            } );
        QObject::connect( &client, &AdbSmartSocketClient::hostReplyReceived,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    const QByteArray& payload ) {
                              replies.push_back( ClientOperation{ generation, operationId } );
                              replyPayloads.push_back( payload );
                          } );
    }

    std::vector<ClientOperation> operations;
    std::vector<ClientOperation> replies;
    std::vector<QByteArray> replyPayloads;
};

class ManagerSnapshotRecorder final {
public:
    explicit ManagerSnapshotRecorder( AdbInfrastructureManager& manager )
    {
        QObject::connect( &manager, &AdbInfrastructureManager::snapshotChanged,
                          [ this ]( const AdbInfrastructureSnapshot& snapshot ) {
                              snapshots.push_back( snapshot );
                          } );
        QObject::connect( &manager, &AdbInfrastructureManager::keyConsentRequired,
                          [ this ]( Generation generation, std::uint64_t epoch ) {
                              consentRequests.push_back( std::make_pair( generation, epoch ) );
                          } );
    }

    std::size_t deviceSnapshotCount() const
    {
        return static_cast<std::size_t>( std::count_if(
            snapshots.begin(), snapshots.end(), []( const AdbInfrastructureSnapshot& snapshot ) {
                return snapshot.devices.requestGeneration != 0u;
            } ) );
    }

    std::vector<AdbInfrastructureSnapshot> snapshots;
    std::vector<std::pair<Generation, std::uint64_t>> consentRequests;
};

struct ManagerHarness {
    TrackDevicesServer server;
    DefaultSocketFactory socketFactory;
    PassiveDeadlineScheduler deadlines;
    AdbSmartSocketClient client;
    ClientOperationRecorder clientOperations;
    ManualProbe probe;
    ManualLauncher launcher;
    ManualStartupLock startupLock;
    FakeKeyStore keyStore;
    ManualServerScheduler supervisorScheduler;
    ManualServerScheduler trackerScheduler;
    AdbInfrastructureManager manager;
    ManagerSnapshotRecorder snapshots;

    ManagerHarness()
        : client( clientConfig( server.port() ), socketFactory, deadlines )
        , clientOperations( client )
        , manager( managerConfig(),
                   AdbInfrastructureManagerDependencies{ probe, launcher, startupLock, keyStore,
                                                         supervisorScheduler, client,
                                                         trackerScheduler } )
        , snapshots( manager )
    {
    }

    AdbInfrastructureLease acquireAndReachReady()
    {
        auto lease = manager.acquireLease();
        REQUIRE( manager.activeLeaseCount() == 1u );
        REQUIRE( probe.requests.size() == 1u );
        CHECK( server.connectionCount() == 0 );
        CHECK( server.requestCount() == 0 );

        probe.completeReady( 0 );
        REQUIRE( manager.snapshot().infrastructure.status == InfrastructureStatus::Ready );
        REQUIRE( drainEventsUntil( [ this ] { return server.requestCount() == 1; } ) );
        CHECK( server.requestAt( 0 ) == QByteArrayLiteral( "host:track-devices-l" ) );
        REQUIRE(
            drainEventsUntil( [ this ] { return clientOperations.operations.size() == 1u; } ) );
        return lease;
    }
};

const DomainAdbDeviceInfo& deviceWithSerial( const std::vector<DomainAdbDeviceInfo>& devices,
                                             std::string_view serial )
{
    const auto found
        = std::find_if( devices.begin(), devices.end(),
                        [ &serial ]( const auto& device ) { return device.serial == serial; } );
    REQUIRE( found != devices.end() );
    return *found;
}

void requireKnownDevices( const AdbInfrastructureManager& manager,
                          const std::vector<std::string>& serials )
{
    const auto snapshot = manager.snapshot();
    const auto& devices = snapshot.devices.devices;
    REQUIRE( devices.size() == serials.size() );
    for ( std::size_t index = 0; index < serials.size(); ++index ) {
        CHECK( devices.at( index ).serial == serials.at( index ) );
    }
}

QByteArray firstDeviceSnapshot()
{
    return QByteArrayLiteral(
        "online-1\tdevice product:foo model:Pixel_9 device:tokay transport_id:1\n"
        "locked-2\tunauthorized usb:1-2 transport_id:2\n"
        "sleeping-3\toffline transport_id:3\n" );
}

} // namespace

TEST_CASE( "shared ADB tracker starts only after infrastructure readiness and publishes typed "
           "track-devices snapshots",
           "[livecapture][adb][tracker][manager][snapshot]" )
{
    ManagerHarness harness;
    auto lease = harness.manager.acquireLease();

    REQUIRE( harness.probe.requests.size() == 1u );
    const auto managerGeneration = harness.manager.snapshot().generation;
    CHECK( managerGeneration > 0u );
    CHECK( harness.manager.snapshot().infrastructure.status == InfrastructureStatus::Connecting );
    CHECK( harness.server.connectionCount() == 0 );
    CHECK( harness.server.requestCount() == 0 );

    harness.probe.completeReady( 0 );
    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 1; } ) );
    REQUIRE( harness.server.requestAt( 0 ) == QByteArrayLiteral( "host:track-devices-l" ) );
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );

    const auto snapshot = harness.manager.snapshot();
    CHECK( snapshot.generation == managerGeneration );
    CHECK( snapshot.infrastructureEpoch > 0u );
    CHECK( snapshot.devices.generation == managerGeneration );
    CHECK( snapshot.devices.infrastructureEpoch == snapshot.infrastructureEpoch );
    CHECK( snapshot.devices.requestGeneration > 0u );
    CHECK( snapshot.infrastructure.status == InfrastructureStatus::Ready );
    CHECK_FALSE( snapshot.error.has_value() );

    const auto& online = deviceWithSerial( snapshot.devices.devices, "online-1" );
    CHECK( online.state == DomainAdbDeviceState::Online );
    CHECK( online.stateText == "device" );
    CHECK( online.product == "foo" );
    CHECK( online.model == "Pixel 9" );
    CHECK( online.device == "tokay" );
    REQUIRE( online.transportId.has_value() );
    CHECK( *online.transportId == 1u );

    const auto& unauthorized = deviceWithSerial( snapshot.devices.devices, "locked-2" );
    CHECK( unauthorized.state == DomainAdbDeviceState::Unauthorized );
    CHECK( unauthorized.stateText == "unauthorized" );
    const auto& offline = deviceWithSerial( snapshot.devices.devices, "sleeping-3" );
    CHECK( offline.state == DomainAdbDeviceState::Offline );
    CHECK( offline.stateText == "offline" );
}

TEST_CASE( "unchanged ADB snapshots coalesce while detach and attach transitions publish",
           "[livecapture][adb][tracker][coalesce][attach]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    const auto initial = firstDeviceSnapshot();

    harness.server.sendTrackAccepted( 0, initial );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    const auto eventCountAfterInitial = harness.snapshots.snapshots.size();
    const auto replyCountAfterInitial = harness.clientOperations.replies.size();
    const auto requestGeneration = harness.manager.snapshot().devices.requestGeneration;

    harness.server.sendSnapshot( 0, initial );
    REQUIRE( drainEventsUntil( [ &harness, replyCountAfterInitial ] {
        return harness.clientOperations.replies.size() == replyCountAfterInitial + 1u;
    } ) );
    CHECK( harness.snapshots.snapshots.size() == eventCountAfterInitial );
    CHECK( harness.manager.snapshot().devices.requestGeneration == requestGeneration );

    const auto detached = QByteArrayLiteral( "online-1\tdevice model:Pixel_9 transport_id:1\n"
                                             "sleeping-3\toffline transport_id:3\n" );
    harness.server.sendSnapshot( 0, detached );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 2u; } ) );
    CHECK( harness.snapshots.snapshots.size() == eventCountAfterInitial + 1u );
    requireKnownDevices( harness.manager, { "online-1", "sleeping-3" } );

    harness.server.sendSnapshot( 0, initial );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    CHECK( harness.snapshots.snapshots.size() == eventCountAfterInitial + 2u );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );
    CHECK( harness.manager.snapshot().devices.requestGeneration == requestGeneration );
}

TEST_CASE( "latest tracked snapshot feeds discovery coordinator and selects an online device by "
           "default",
           "[livecapture][adb][tracker][device-discovery][selection]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted(
        0, QByteArrayLiteral( "offline-first\toffline transport_id:1\n"
                              "locked-second\tunauthorized transport_id:2\n"
                              "online-third\tdevice model:Pixel_8 transport_id:3\n" ) );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );

    const auto selected = harness.manager.defaultOnlineDevice();
    REQUIRE( selected.has_value() );
    CHECK( selected->serial == "online-third" );

    DeviceDiscoveryCoordinator<::AdbDeviceInfo> coordinator;
    const auto refreshGeneration = coordinator.beginRefresh();
    REQUIRE( coordinator.accept(
        mapTrackedAdbInfrastructureSnapshot( refreshGeneration, harness.manager.snapshot() ) ) );
    REQUIRE( coordinator.currentDevices().size() == 3 );
    CHECK( coordinator.currentDevices().at( 0 ).serial == QStringLiteral( "offline-first" ) );
    CHECK( coordinator.currentDevices().at( 0 ).state == ::AdbDeviceState::Offline );
    CHECK( coordinator.currentDevices().at( 2 ).serial == QStringLiteral( "online-third" ) );
    CHECK( coordinator.currentDevices().at( 2 ).state == ::AdbDeviceState::Online );
    REQUIRE_FALSE( coordinator.currentError().has_value() );

    harness.server.sendSnapshot(
        0, QByteArrayLiteral( "offline-only\toffline\nlocked-only\tunauthorized\n" ) );
    REQUIRE( drainEventsUntil( [ &harness ] {
        return harness.manager.snapshot().devices.devices.size() == 2u
               && harness.manager.snapshot().devices.devices.front().serial == "offline-only";
    } ) );
    CHECK_FALSE( harness.manager.defaultOnlineDevice().has_value() );
}

TEST_CASE( "track FAIL and EOF use bounded injected reconnect without withdrawing known devices",
           "[livecapture][adb][tracker][reconnect][eof][fail]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    const auto firstOperation = harness.clientOperations.operations.front();

    harness.server.closeConnection( 0 );
    REQUIRE( drainEventsUntil( [ &harness ] {
        return harness.trackerScheduler.activeCount( AdbServerScheduleKind::ReconnectBackoff )
               == 1u;
    } ) );
    CHECK( harness.trackerScheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff ) == 5ms );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );

    harness.trackerScheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 2; } ) );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.clientOperations.operations.size() == 2u; } ) );
    const auto secondOperation = harness.clientOperations.operations.back();
    CHECK( secondOperation.generation != firstOperation.generation );

    Q_EMIT harness.client.hostReplyReceived(
        firstOperation.generation, firstOperation.operationId,
        QByteArrayLiteral( "stale-device\tdevice transport_id:99\n" ) );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );

    harness.server.sendFailure( 1, QByteArrayLiteral( "track service replaced" ) );
    REQUIRE( drainEventsUntil( [ &harness ] {
        return harness.trackerScheduler.activeCount( AdbServerScheduleKind::ReconnectBackoff )
               == 1u;
    } ) );
    CHECK( harness.trackerScheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff ) == 10ms );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );
    REQUIRE( harness.manager.snapshot().error.has_value() );
    CHECK( harness.manager.snapshot().error->code == "adb-track-remote-failure" );
    CHECK( harness.manager.snapshot().error->scope == ErrorScope::Service );
    CHECK( harness.manager.snapshot().error->retryPolicy == RetryPolicy::Backoff );
    CHECK( harness.manager.snapshot().error->nativeDetail.find( "track service replaced" )
           != std::string::npos );
    CHECK( mapTrackedAdbInfrastructureSnapshot( 80u, harness.manager.snapshot() ).devices.empty() );
    const auto errorSnapshotCount = harness.snapshots.snapshots.size();
    harness.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    REQUIRE( harness.probe.requests.size() == 2u );
    harness.probe.completeReady( 1, FirstServerIdentity );
    REQUIRE( harness.manager.snapshot().error.has_value() );
    CHECK( harness.manager.snapshot().error->code == "adb-track-remote-failure" );
    CHECK( harness.snapshots.snapshots.size() == errorSnapshotCount );

    harness.trackerScheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 3; } ) );
    harness.server.sendTrackAccepted(
        2, QByteArrayLiteral( "replacement-device\tdevice model:Pixel_10 transport_id:7\n" ) );
    REQUIRE( drainEventsUntil( [ &harness ] {
        const auto snapshot = harness.manager.snapshot();
        const auto& devices = snapshot.devices.devices;
        return devices.size() == 1u && devices.front().serial == "replacement-device";
    } ) );
}

TEST_CASE( "malformed authoritative track snapshots retain known devices and reconnect",
           "[livecapture][adb][tracker][parse][protocol]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );

    harness.server.sendSnapshot(
        0, QByteArrayLiteral( "online-1\tdevice transport_id:1\nmissing-tab-separator\n" ) );
    REQUIRE( drainEventsUntil( [ &harness ] {
        return harness.trackerScheduler.activeCount( AdbServerScheduleKind::ReconnectBackoff )
               == 1u;
    } ) );

    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );
    REQUIRE( harness.manager.snapshot().error.has_value() );
    CHECK( harness.manager.snapshot().error->code == "adb-track-protocol" );
    CHECK( harness.manager.snapshot().error->scope == ErrorScope::Service );
    CHECK( harness.manager.snapshot().error->retryPolicy == RetryPolicy::Backoff );
    CHECK( harness.manager.snapshot().error->nativeDetail.find( "line 2" ) != std::string::npos );
}

TEST_CASE( "server replacement advances infrastructure epoch and ignores stale tracker callbacks",
           "[livecapture][adb][manager][epoch][stale]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    const auto firstEpoch = harness.manager.snapshot().infrastructureEpoch;
    const auto firstOperation = harness.clientOperations.operations.front();

    harness.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    REQUIRE( harness.probe.requests.size() == 2u );
    harness.probe.completeReady( 1, ReplacementServerIdentity );

    REQUIRE( harness.manager.snapshot().infrastructureEpoch == firstEpoch + 1u );
    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 2; } ) );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.clientOperations.operations.size() == 2u; } ) );
    const auto replacementOperation = harness.clientOperations.operations.back();
    CHECK( replacementOperation.generation != firstOperation.generation );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );

    Q_EMIT harness.client.hostReplyReceived(
        firstOperation.generation, firstOperation.operationId,
        QByteArrayLiteral( "stale-after-replacement\tdevice transport_id:88\n" ) );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );

    harness.server.sendTrackAccepted(
        1, QByteArrayLiteral( "current-after-replacement\tdevice transport_id:9\n" ) );
    REQUIRE( drainEventsUntil( [ &harness ] {
        const auto snapshot = harness.manager.snapshot();
        const auto& devices = snapshot.devices.devices;
        return devices.size() == 1u && devices.front().serial == "current-after-replacement";
    } ) );
    CHECK( harness.manager.snapshot().devices.infrastructureEpoch == firstEpoch + 1u );
}

TEST_CASE( "supervisor loss cancels tracking and publishes a structured infrastructure error",
           "[livecapture][adb][manager][infrastructure][error]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    const auto operationCount = harness.clientOperations.operations.size();

    harness.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.completeAbsent( 1, "shared server disappeared" );

    REQUIRE( harness.manager.snapshot().infrastructure.status
             == InfrastructureStatus::Unavailable );
    REQUIRE( harness.manager.snapshot().error.has_value() );
    CHECK( harness.manager.snapshot().error->category == ErrorCategory::Infrastructure );
    CHECK( harness.manager.snapshot().error->scope == ErrorScope::Infrastructure );
    CHECK( harness.manager.snapshot().error->retryPolicy == RetryPolicy::WaitForInfrastructure );
    CHECK_FALSE( harness.manager.snapshot().error->code.empty() );
    CHECK( harness.manager.snapshot().error->nativeDetail.find( "disappeared" )
           != std::string::npos );
    CHECK( harness.trackerScheduler.activeCount( AdbServerScheduleKind::ReconnectBackoff ) == 0u );

    DeviceDiscoveryCoordinator<::AdbDeviceInfo> coordinator;
    const auto refreshGeneration = coordinator.beginRefresh();
    REQUIRE( coordinator.accept(
        mapTrackedAdbInfrastructureSnapshot( refreshGeneration, harness.manager.snapshot() ) ) );
    CHECK( coordinator.currentDevices().empty() );
    REQUIRE( coordinator.currentError().has_value() );
    CHECK( coordinator.currentError()->scope == ErrorScope::Infrastructure );
    CHECK( coordinator.currentError()->retryPolicy == RetryPolicy::WaitForInfrastructure );

    Q_EMIT harness.client.hostReplyReceived(
        harness.clientOperations.operations.front().generation,
        harness.clientOperations.operations.front().operationId,
        QByteArrayLiteral( "stale-after-loss\tdevice transport_id:5\n" ) );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );
    CHECK( harness.clientOperations.operations.size() == operationCount );
}

TEST_CASE( "one application manager shares one supervisor and tracker across consumer leases",
           "[livecapture][adb][manager][shared][lease]" )
{
    ManagerHarness harness;
    auto dialogLease = harness.manager.acquireLease();
    const auto sharedGeneration = harness.manager.snapshot().generation;
    REQUIRE( sharedGeneration > 0u );
    auto transportLease = harness.manager.acquireLease();
    CHECK( harness.manager.activeLeaseCount() == 2u );
    CHECK( harness.manager.snapshot().generation == sharedGeneration );
    REQUIRE( harness.probe.requests.size() == 1u );

    harness.probe.completeReady( 0 );
    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 1; } ) );
    CHECK( harness.server.connectionCount() == 1 );
    CHECK( harness.server.requestAt( 0 ) == QByteArrayLiteral( "host:track-devices-l" ) );

    dialogLease.reset();
    CHECK( harness.manager.activeLeaseCount() == 1u );
    CHECK( harness.probe.requests.size() == 1u );
    CHECK( harness.server.requestCount() == 1 );
    CHECK( harness.launcher.cleanupTokens.empty() );
    CHECK( harness.manager.snapshot().infrastructure.status == InfrastructureStatus::Ready );

    transportLease.reset();
    CHECK( harness.manager.activeLeaseCount() == 0u );
    CHECK( harness.probe.requests.size() == 1u );
    CHECK( harness.launcher.cleanupTokens.empty() );
    CHECK( harness.manager.snapshot().infrastructure.status == InfrastructureStatus::Ready );

    auto laterDialogLease = harness.manager.acquireLease();
    CHECK( harness.manager.activeLeaseCount() == 1u );
    CHECK( harness.manager.snapshot().generation == sharedGeneration );
    CHECK( harness.probe.requests.size() == 1u );
    CHECK( harness.server.requestCount() == 1 );
}

TEST_CASE( "manager forwards explicit ADB key consent without process or PATH discovery",
           "[livecapture][adb][manager][keys][consent]" )
{
    ManagerHarness harness;
    harness.keyStore.inspection.state = AdbServerStandardKeyState::Absent;
    auto lease = harness.manager.acquireLease();

    harness.probe.completeAbsent( 0 );
    REQUIRE( harness.startupLock.requests.size() == 1u );
    harness.startupLock.completeAcquired( 0 );
    REQUIRE( harness.probe.requests.size() == 2u );
    harness.probe.completeAbsent( 1 );

    REQUIRE( harness.snapshots.consentRequests.size() == 1u );
    CHECK( harness.snapshots.consentRequests.front().first
           == harness.manager.snapshot().generation );
    CHECK( harness.snapshots.consentRequests.front().second
           == harness.manager.snapshot().infrastructureEpoch );
    CHECK( harness.keyStore.generationCount == 0 );
    CHECK( harness.launcher.requests.empty() );

    harness.manager.grantKeyGenerationConsent( true );

    CHECK( harness.keyStore.generationCount == 1 );
    REQUIRE( harness.launcher.requests.size() == 1u );
    CHECK( harness.launcher.requests.front().request.executable
           == QStringLiteral( "/opt/klogg/libexec/adb" ) );
    CHECK_FALSE( harness.launcher.requests.front().request.allowPathLookup );
    CHECK( harness.server.connectionCount() == 0 );
    CHECK( harness.server.requestCount() == 0 );
}

TEST_CASE( "idle lease reacquisition retries a transient terminal supervisor failure once",
           "[livecapture][adb][manager][lease][retry]" )
{
    ManagerHarness harness;
    auto firstLease = harness.manager.acquireLease();
    const auto firstGeneration = harness.manager.snapshot().generation;
    REQUIRE( harness.probe.requests.size() == 1u );

    harness.probe.completeFailed( 0, "temporary probe failure" );
    REQUIRE( harness.manager.snapshot().infrastructure.status
             == InfrastructureStatus::Unavailable );
    REQUIRE( harness.manager.snapshot().error.has_value() );
    CHECK( harness.manager.snapshot().error->retryPolicy == RetryPolicy::WaitForInfrastructure );

    firstLease.reset();
    REQUIRE( harness.manager.activeLeaseCount() == 0u );
    auto replacementLease = harness.manager.acquireLease();

    CHECK( harness.manager.activeLeaseCount() == 1u );
    CHECK( harness.manager.snapshot().generation == firstGeneration + 1u );
    REQUIRE( harness.probe.requests.size() == 2u );
    CHECK( harness.probe.requests.back().active );
}

TEST_CASE( "tracker reconnect backoff caps and cancelled timers cannot restart stale epochs",
           "[livecapture][adb][tracker][reconnect][backoff][stale]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );

    const std::vector<std::chrono::milliseconds> expectedDelays{ 5ms, 10ms, 20ms, 20ms };
    for ( std::size_t attempt = 0; attempt < expectedDelays.size(); ++attempt ) {
        harness.server.closeConnection( static_cast<klogg::ContainerIndex>( attempt ) );
        REQUIRE( drainEventsUntil( [ &harness ] {
            return harness.trackerScheduler.activeCount( AdbServerScheduleKind::ReconnectBackoff )
                   == 1u;
        } ) );
        CHECK( harness.trackerScheduler.lastDelay( AdbServerScheduleKind::ReconnectBackoff )
               == expectedDelays.at( attempt ) );
        if ( attempt + 1u < expectedDelays.size() ) {
            harness.trackerScheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
            REQUIRE( drainEventsUntil( [ &harness, attempt ] {
                return harness.server.requestCount()
                       == static_cast<klogg::ContainerIndex>( attempt + 2u );
            } ) );
            harness.server.sendTrackAccepted( static_cast<klogg::ContainerIndex>( attempt + 1u ),
                                              firstDeviceSnapshot() );
            REQUIRE( drainEventsUntil( [ &harness, attempt ] {
                return harness.clientOperations.replies.size() == attempt + 2u;
            } ) );
        }
    }

    const auto staleToken = harness.trackerScheduler.entries.back().token;
    harness.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.completeReady( 1, ReplacementServerIdentity );
    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 5; } ) );
    const auto requestCount = harness.server.requestCount();

    harness.trackerScheduler.fireEvenIfCancelled( staleToken );
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents( QEventLoop::AllEvents );
    CHECK( harness.server.requestCount() == requestCount );
}

TEST_CASE(
    "manager excludes retained devices from default selection until the current epoch reports",
    "[livecapture][adb][manager][selection][stale]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    REQUIRE( harness.manager.defaultOnlineDevice().has_value() );

    harness.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    harness.probe.completeAbsent( 1, "shared server disappeared" );
    REQUIRE( harness.manager.snapshot().infrastructure.status
             == InfrastructureStatus::Unavailable );
    CHECK_FALSE( harness.manager.defaultOnlineDevice().has_value() );
    requireKnownDevices( harness.manager, { "online-1", "locked-2", "sleeping-3" } );
    CHECK( mapTrackedAdbInfrastructureSnapshot( 81u, harness.manager.snapshot() ).devices.empty() );

    harness.supervisorScheduler.fire( AdbServerScheduleKind::ReconnectBackoff );
    REQUIRE( harness.probe.requests.size() == 3u );
    harness.probe.completeReady( 2, ReplacementServerIdentity );
    REQUIRE( harness.manager.snapshot().infrastructure.status == InfrastructureStatus::Ready );
    CHECK_FALSE( harness.manager.defaultOnlineDevice().has_value() );
    CHECK( mapTrackedAdbInfrastructureSnapshot( 82u, harness.manager.snapshot() ).devices.empty() );

    REQUIRE( drainEventsUntil( [ &harness ] { return harness.server.requestCount() == 2; } ) );
    harness.server.sendTrackAccepted(
        1, QByteArrayLiteral( "replacement-online\tdevice transport_id:12\n" ) );
    REQUIRE( drainEventsUntil( [ &harness ] {
        const auto selected = harness.manager.defaultOnlineDevice();
        return selected.has_value() && selected->serial == "replacement-online";
    } ) );
    const auto currentDiscovery
        = mapTrackedAdbInfrastructureSnapshot( 83u, harness.manager.snapshot() );
    REQUIRE( currentDiscovery.devices.size() == 1 );
    CHECK( currentDiscovery.devices.front().serial == QStringLiteral( "replacement-online" ) );
}

TEST_CASE( "unchanged health confirmation coalesces at the aggregate manager boundary",
           "[livecapture][adb][manager][coalesce][health]" )
{
    ManagerHarness harness;
    auto lease = harness.acquireAndReachReady();
    harness.server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE( drainEventsUntil(
        [ &harness ] { return harness.manager.snapshot().devices.devices.size() == 3u; } ) );
    const auto snapshotCount = harness.snapshots.snapshots.size();
    const auto epoch = harness.manager.snapshot().infrastructureEpoch;

    harness.supervisorScheduler.fire( AdbServerScheduleKind::HealthProbe );
    REQUIRE( harness.probe.requests.size() == 2u );
    harness.probe.completeReady( 1, FirstServerIdentity );

    CHECK( harness.manager.snapshot().infrastructureEpoch == epoch );
    CHECK( harness.snapshots.snapshots.size() == snapshotCount );
    CHECK( harness.server.requestCount() == 1 );
}

TEST_CASE( "tracker and manager snapshots cross queued Qt connections",
           "[livecapture][adb][tracker][manager][queued]" )
{
    TrackDevicesServer server;
    DefaultSocketFactory socketFactory;
    PassiveDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ManualServerScheduler trackerScheduler;
    AdbDeviceTracker tracker( AdbDeviceTrackerConfig{ { 5ms } }, client, trackerScheduler );
    QObject trackerReceiver;
    int queuedTrackerSnapshots = 0;
    QObject::connect(
        &tracker, &AdbDeviceTracker::snapshotChanged, &trackerReceiver,
        [ &queuedTrackerSnapshots ]( const AdbTrackedDeviceSnapshot& snapshot ) {
            CHECK( snapshot.generation == 41u );
            CHECK( snapshot.infrastructureEpoch == 7u );
            ++queuedTrackerSnapshots;
        },
        Qt::QueuedConnection );

    tracker.start( 41u, 7u );
    REQUIRE( drainEventsUntil( [ &server ] { return server.requestCount() == 1; } ) );
    server.sendTrackAccepted( 0, firstDeviceSnapshot() );
    REQUIRE(
        drainEventsUntil( [ &queuedTrackerSnapshots ] { return queuedTrackerSnapshots == 1; } ) );

    ManagerHarness harness;
    QObject managerReceiver;
    int queuedManagerSnapshots = 0;
    QObject::connect(
        &harness.manager, &AdbInfrastructureManager::snapshotChanged, &managerReceiver,
        [ &queuedManagerSnapshots ]( const AdbInfrastructureSnapshot& snapshot ) {
            CHECK( snapshot.generation > 0u );
            ++queuedManagerSnapshots;
        },
        Qt::QueuedConnection );
    auto lease = harness.manager.acquireLease();
    REQUIRE(
        drainEventsUntil( [ &queuedManagerSnapshots ] { return queuedManagerSnapshots > 0; } ) );
}

TEST_CASE( "manager leases may outlive their non-owning manager",
           "[livecapture][adb][manager][lease][lifetime]" )
{
    TrackDevicesServer server;
    DefaultSocketFactory socketFactory;
    PassiveDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ManualProbe probe;
    ManualLauncher launcher;
    ManualStartupLock startupLock;
    FakeKeyStore keyStore;
    ManualServerScheduler supervisorScheduler;
    ManualServerScheduler trackerScheduler;
    AdbInfrastructureLease survivingLease;
    {
        AdbInfrastructureManager manager(
            managerConfig(),
            AdbInfrastructureManagerDependencies{ probe, launcher, startupLock, keyStore,
                                                  supervisorScheduler, client, trackerScheduler } );
        survivingLease = manager.acquireLease();
        REQUIRE( survivingLease );
        CHECK( manager.activeLeaseCount() == 1u );
    }

    survivingLease.reset();
    CHECK_FALSE( survivingLease );
}

TEST_CASE( "manager projects recoverable supervisor errors to infrastructure waiting policy",
           "[livecapture][adb][manager][error][device-discovery]" )
{
    ManagerHarness harness;
    auto lease = harness.manager.acquireLease();
    harness.probe.completeFailed( 0, "temporary probe failure" );

    REQUIRE( harness.manager.snapshot().error.has_value() );
    CHECK( harness.manager.snapshot().error->category == ErrorCategory::Infrastructure );
    CHECK( harness.manager.snapshot().error->scope == ErrorScope::Infrastructure );
    CHECK( harness.manager.snapshot().error->retryPolicy == RetryPolicy::WaitForInfrastructure );

    const auto result = mapTrackedAdbInfrastructureSnapshot( 77u, harness.manager.snapshot() );
    REQUIRE( result.error.has_value() );
    CHECK( result.error->category == ErrorCategory::Infrastructure );
    CHECK( result.error->scope == ErrorScope::Infrastructure );
    CHECK( result.error->retryPolicy == RetryPolicy::WaitForInfrastructure );
}

TEST_CASE( "non-retryable manager configuration errors remain non-retryable in discovery",
           "[livecapture][adb][manager][error][configuration]" )
{
    TrackDevicesServer server;
    DefaultSocketFactory socketFactory;
    PassiveDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ManualProbe probe;
    ManualLauncher launcher;
    ManualStartupLock startupLock;
    FakeKeyStore keyStore;
    ManualServerScheduler supervisorScheduler;
    ManualServerScheduler trackerScheduler;
    auto config = managerConfig();
    config.server.packagedServerPath = QStringLiteral( "relative/adb" );
    AdbInfrastructureManager manager(
        std::move( config ),
        AdbInfrastructureManagerDependencies{ probe, launcher, startupLock, keyStore,
                                              supervisorScheduler, client, trackerScheduler } );

    auto lease = manager.acquireLease();
    REQUIRE( manager.snapshot().error.has_value() );
    CHECK( manager.snapshot().error->category == ErrorCategory::Configuration );
    CHECK( manager.snapshot().error->retryPolicy == RetryPolicy::Never );

    const auto result = mapTrackedAdbInfrastructureSnapshot( 78u, manager.snapshot() );
    REQUIRE( result.error.has_value() );
    CHECK( result.error->category == ErrorCategory::Configuration );
    CHECK( result.error->retryPolicy == RetryPolicy::Never );
}
