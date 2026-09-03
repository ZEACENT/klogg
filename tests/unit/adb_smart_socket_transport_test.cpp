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

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QPointer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "adblogcatsource.h"
#include "adbprotocol.h"
#include "adbserversupervisor.h"
#include "adbsmartsocketclient.h"
#include "adbsmartsockettransport.h"
#include "livedataqueue.h"
#include "livesourcetransport.h"

namespace {
using klogg::livecapture::Generation;
using klogg::livecapture::LiveDataQueueLimits;
using namespace klogg::livecapture::adb;

constexpr int EventPumpTimeoutMs = 1500;
constexpr Generation StreamGeneration = 101u;
constexpr auto StreamSerial = "SERIAL-42";
constexpr auto VersionRequest = "host:version";
constexpr auto HostFeaturesRequest = "host:host-features";
constexpr auto UnscopedFeaturesRequest = "host:features";
constexpr auto TransportRequest = "host:transport:SERIAL-42";
constexpr auto LogcatRequest
    = "shell,v2,raw:logcat -v threadtime -v year -v zone -v usec";
constexpr auto ClearRequest = "shell,v2,raw:logcat -c";

static_assert( std::is_base_of<LiveSourceTransport, AdbSmartSocketTransport>::value,
               "The native ADB transport must satisfy the shared live-source contract." );

bool pumpEventsUntil( const std::function<bool()>& predicate, int timeoutMs = EventPumpTimeoutMs )
{
    QElapsedTimer guard;
    guard.start();
    while ( !predicate() && guard.elapsed() < timeoutMs ) {
        QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );
    }
    return predicate();
}

void processDeferredDeletes()
{
    QCoreApplication::sendPostedEvents( nullptr, QEvent::DeferredDelete );
    QCoreApplication::processEvents( QEventLoop::AllEvents );
}

QByteArray hostReplyFrame( const QByteArray& payload )
{
    REQUIRE( payload.size() <= 0xffff );
    return QByteArray::number( payload.size(), 16 ).rightJustified( 4, '0' ).toUpper() + payload;
}

QByteArray shellV2Frame( std::uint8_t channel, const QByteArray& payload )
{
    const auto length = static_cast<std::uint32_t>( payload.size() );
    QByteArray frame;
    frame.reserve( payload.size() + 5 );
    frame.append( static_cast<char>( channel ) );
    frame.append( static_cast<char>( length & 0xffu ) );
    frame.append( static_cast<char>( ( length >> 8u ) & 0xffu ) );
    frame.append( static_cast<char>( ( length >> 16u ) & 0xffu ) );
    frame.append( static_cast<char>( ( length >> 24u ) & 0xffu ) );
    frame.append( payload );
    return frame;
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

class FakeAdbServer final : public QObject {
public:
    using RequestHandler = std::function<void( QTcpSocket&, int connectionIndex, int requestIndex,
                                               const QByteArray& request )>;

    explicit FakeAdbServer( RequestHandler handler, QObject* parent = nullptr )
        : QObject( parent )
        , handler_( std::move( handler ) )
    {
        REQUIRE( server_.listen( QHostAddress::LocalHost, 0 ) );
        QObject::connect( &server_, &QTcpServer::newConnection, this,
                          [ this ] { acceptConnections(); } );
    }

    quint16 port() const
    {
        return server_.serverPort();
    }

    int connectionCount() const
    {
        return static_cast<int>( sockets_.size() );
    }

    int requestCount() const
    {
        return static_cast<int>( requests_.size() );
    }

    const QVector<QByteArray>& requests() const
    {
        return requests_;
    }

    const QVector<int>& requestConnections() const
    {
        return requestConnections_;
    }

    QTcpSocket* socketAt( int index ) const
    {
        return sockets_.at( index ).data();
    }

    bool peerWasLoopbackAtAccept( int index ) const
    {
        return peerWasLoopbackAtAccept_.at( index );
    }

    static void send( QTcpSocket& socket, const QByteArray& bytes )
    {
        REQUIRE( socket.state() == QAbstractSocket::ConnectedState );
        REQUIRE( socket.write( bytes ) == bytes.size() );
        socket.flush();
    }

    static void sendThenClose( QTcpSocket& socket, const QByteArray& bytes = {} )
    {
        if ( !bytes.isEmpty() ) {
            send( socket, bytes );
        }
        socket.disconnectFromHost();
    }

private:
    struct ConnectionBuffer {
        QByteArray bytes;
        int requestIndex = 0;
    };

    void acceptConnections()
    {
        while ( server_.hasPendingConnections() ) {
            auto* const socket = server_.nextPendingConnection();
            REQUIRE( socket != nullptr );
            const auto connectionIndex = static_cast<int>( sockets_.size() );
            sockets_.append( socket );
            buffers_.append( ConnectionBuffer{} );
            peerWasLoopbackAtAccept_.append( socket->peerAddress().isLoopback() );
            QObject::connect( socket, &QTcpSocket::readyRead, this,
                              [ this, socket, connectionIndex ] {
                                  consumeRequests( *socket, connectionIndex );
                              } );
        }
    }

    void consumeRequests( QTcpSocket& socket, int connectionIndex )
    {
        auto& buffer = buffers_[ connectionIndex ];
        while ( socket.bytesAvailable() > 0 ) {
            buffer.bytes.append( socket.read( std::min<qint64>( socket.bytesAvailable(), 7 ) ) );
        }

        for ( ;; ) {
            if ( buffer.bytes.size() < 4 ) {
                return;
            }
            const auto length = parseRequestLength( buffer.bytes.left( 4 ) );
            REQUIRE( length.has_value() );
            if ( buffer.bytes.size() - 4 < *length ) {
                return;
            }

            const auto request = buffer.bytes.mid( 4, *length );
            buffer.bytes.remove( 0, 4 + *length );
            const auto requestIndex = buffer.requestIndex++;
            requests_.append( request );
            requestConnections_.append( connectionIndex );
            handler_( socket, connectionIndex, requestIndex, request );
        }
    }

private:
    QTcpServer server_;
    RequestHandler handler_;
    QVector<QPointer<QTcpSocket>> sockets_;
    QVector<ConnectionBuffer> buffers_;
    QVector<QByteArray> requests_;
    QVector<int> requestConnections_;
    QVector<bool> peerWasLoopbackAtAccept_;
};

class TrackingSocketFactory final : public AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership is transferred to the supplied Qt parent.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const socket = new QTcpSocket( parent );
        sockets_.append( socket );
        return socket;
    }

    int socketCount() const
    {
        return static_cast<int>( sockets_.size() );
    }

    int connectedSocketCount() const
    {
        return static_cast<int>( std::count_if(
            sockets_.begin(), sockets_.end(), []( const QPointer<QTcpSocket>& socket ) {
                return socket && socket->state() == QAbstractSocket::ConnectedState;
            } ) );
    }

    bool allSocketsRetired() const
    {
        return std::all_of(
            sockets_.begin(), sockets_.end(), []( const QPointer<QTcpSocket>& socket ) {
                return !socket || socket->state() == QAbstractSocket::UnconnectedState;
            } );
    }

private:
    QVector<QPointer<QTcpSocket>> sockets_;
};

class ManualDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    struct Entry {
        DeadlineToken token{ 0 };
        AdbSmartSocketDeadlineKind kind{ AdbSmartSocketDeadlineKind::Connect };
        QPointer<QObject> context;
        std::function<void()> callback;
        bool active{ true };
    };

    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind kind, int, QObject* context,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        entries_.push_back( Entry{ token, kind, context, std::move( callback ), true } );
        return token;
    }

    void cancelDeadline( DeadlineToken token ) override
    {
        for ( auto& entry : entries_ ) {
            if ( entry.token == token ) {
                entry.active = false;
            }
        }
    }

    bool hasActive( AdbSmartSocketDeadlineKind kind ) const
    {
        return std::any_of( entries_.begin(), entries_.end(), [ kind ]( const Entry& entry ) {
            return entry.active && entry.kind == kind && entry.context;
        } );
    }

    void fire( AdbSmartSocketDeadlineKind kind )
    {
        const auto found
            = std::find_if( entries_.begin(), entries_.end(), [ kind ]( const Entry& entry ) {
                  return entry.active && entry.kind == kind && entry.context;
              } );
        REQUIRE( found != entries_.end() );
        found->active = false;
        const auto callback = found->callback;
        callback();
    }

private:
    DeadlineToken nextToken_{ 0 };
    std::vector<Entry> entries_;
};

AdbSmartSocketClientConfig clientConfig( quint16 port )
{
    AdbSmartSocketClientConfig config;
    config.serverAddress = QHostAddress::LocalHost;
    config.serverPort = port;
    config.maxReadChunkBytes = 64;
    config.maxWriteChunkBytes = 64;
    config.maxHostReplyBytes = 0xffffu;
    config.maxShellFrameBytes = std::size_t{ 1024u } * 1024u;
    config.connectTimeoutMs = 3000;
    config.writeTimeoutMs = 3000;
    config.readTimeoutMs = 3000;
    return config;
}

AdbSmartSocketTransportConfig transportConfig( quint16 port,
                                               LiveDataQueueLimits queueLimits
                                               = LiveDataQueueLimits{ 64u * 1024u, 64u } )
{
    AdbSmartSocketTransportConfig config;
    config.clientConfig = clientConfig( port );
    config.deviceSerial = QString::fromLatin1( StreamSerial );
    config.logcatOptions = LogcatCommandOptions{};
    config.queueLimits = queueLimits;
    return config;
}

class SmartSocketTestTransportFactory final : public LiveSourceTransportFactory {
public:
    SmartSocketTestTransportFactory( AdbSmartSocketTransportConfig config,
                                     AdbSmartSocketFactory& socketFactory,
                                     AdbSmartSocketDeadlineScheduler& deadlineScheduler )
        : config_( std::move( config ) )
        , socketFactory_( socketFactory )
        , deadlineScheduler_( deadlineScheduler )
    {
    }

    std::unique_ptr<LiveSourceTransport> create( const LiveSourceTransportConfig& ) const override
    {
        return std::make_unique<AdbSmartSocketTransport>( config_, socketFactory_,
                                                          deadlineScheduler_ );
    }

private:
    AdbSmartSocketTransportConfig config_;
    AdbSmartSocketFactory& socketFactory_;
    AdbSmartSocketDeadlineScheduler& deadlineScheduler_;
};

struct ObservedState {
    Generation generation{ 0 };
    LiveSourceTransport::State state{ LiveSourceTransport::State::Disconnected };
};

struct ObservedBytes {
    Generation generation{ 0 };
    QByteArray bytes;
};

struct ObservedError {
    Generation generation{ 0 };
    QString error;
};

struct ObservedClear {
    Generation generation{ 0 };
    LiveSourceTransport::ClearRequestId requestId{ 0 };
    bool succeeded{ false };
    QString error;
};

class TransportProbe final {
public:
    explicit TransportProbe( LiveSourceTransport& transport )
    {
        QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                          [ this ]( Generation generation, LiveSourceTransport::State state ) {
                              states.push_back( { generation, state } );
                          } );
        QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                          [ this ]( Generation generation, const QByteArray& bytes ) {
                              received.push_back( { generation, bytes } );
                          } );
        QObject::connect( &transport, &LiveSourceTransport::errorOccurred,
                          [ this ]( Generation generation, const QString& error ) {
                              errors.push_back( { generation, error } );
                          } );
        QObject::connect( &transport, &LiveSourceTransport::clearRemoteFinished,
                          [ this ]( Generation generation,
                                    LiveSourceTransport::ClearRequestId requestId, bool succeeded,
                                    const QString& error ) {
                              clears.push_back( { generation, requestId, succeeded, error } );
                          } );
    }

    int stateCount( LiveSourceTransport::State state ) const
    {
        return static_cast<int>( std::count_if(
            states.begin(), states.end(),
            [ state ]( const ObservedState& observed ) { return observed.state == state; } ) );
    }

    QByteArray receivedBytes() const
    {
        QByteArray bytes;
        for ( const auto& observed : received ) {
            bytes.append( observed.bytes );
        }
        return bytes;
    }

    std::vector<ObservedState> states;
    std::vector<ObservedBytes> received;
    std::vector<ObservedError> errors;
    std::vector<ObservedClear> clears;
};

void sendFeaturesOkay( QTcpSocket& socket,
                       const QByteArray& features = QByteArrayLiteral( "cmd,shell_v2,stat_v2" ) )
{
    FakeAdbServer::sendThenClose( socket,
                                  QByteArrayLiteral( "OKAY" ) + hostReplyFrame( features ) );
}

void handleSuccessfulStartupRequest( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                     const QByteArray& request, bool sendServiceOkay = true )
{
    if ( requestIndex == 0 ) {
        REQUIRE( request == QByteArray( TransportRequest ) );
        FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
        return;
    }

    REQUIRE( requestIndex == 1 );
    if ( connectionIndex % 2 == 0 ) {
        REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
        sendFeaturesOkay( socket );
        return;
    }

    REQUIRE( request == QByteArray( LogcatRequest ) );
    if ( sendServiceOkay ) {
        FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
    }
}

void requireSingleTerminalError( const AdbSmartSocketTransport& transport,
                                 const TransportProbe& probe, Generation generation,
                                 const QStringList& expectedFragments )
{
    REQUIRE( probe.errors.size() == 1u );
    CHECK( probe.errors.front().generation == generation );
    CHECK_FALSE( probe.errors.front().error.isEmpty() );
    CHECK( transport.lastError() == probe.errors.front().error );
    CHECK( probe.stateCount( LiveSourceTransport::State::Error ) == 1 );
    for ( const auto& fragment : expectedFragments ) {
        CHECK( probe.errors.front().error.contains( fragment, Qt::CaseInsensitive ) );
    }
}

} // namespace

TEST_CASE( "ADB smart-socket server probe requests server-scoped features with multiple devices",
           "[livecapture][adb][server][probe][features]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                              const QByteArray& request ) {
        REQUIRE( requestIndex == 0 );
        if ( request == QByteArray( VersionRequest ) ) {
            REQUIRE( connectionIndex == 0 );
            FakeAdbServer::sendThenClose(
                socket,
                QByteArrayLiteral( "OKAY" ) + hostReplyFrame( QByteArrayLiteral( "0029" ) ) );
            return;
        }
        if ( request == QByteArray( HostFeaturesRequest ) ) {
            REQUIRE( connectionIndex == 1 );
            sendFeaturesOkay( socket );
            return;
        }

        REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
        FakeAdbServer::send(
            socket, QByteArrayLiteral( "FAIL" )
                        + hostReplyFrame( QByteArrayLiteral( "more than one device/emulator" ) ) );
    } );
    AdbSmartSocketServerProbe probe;
    std::optional<AdbServerProbeResult> result;
    AdbServerEndpoint endpoint;
    endpoint.port = server.port();

    probe.probe( endpoint, [ &result ]( AdbServerProbeResult completed ) {
        result = std::move( completed );
    } );

    REQUIRE( pumpEventsUntil( [ &result ] { return result.has_value(); } ) );
    REQUIRE( result.has_value() );
    CHECK( result->state == AdbServerProbeState::Ready );
    CHECK( result->protocolVersion == 0x29u );
    CHECK( result->features
           == std::vector<std::string>{ "cmd", "shell_v2", "stat_v2" } );
    CHECK( result->diagnostic.empty() );
    REQUIRE( server.connectionCount() == 2 );
    REQUIRE( server.requests().size() == 2 );
    CHECK( server.requests().at( 0 ) == QByteArray( VersionRequest ) );
    CHECK( server.requests().at( 1 ) == QByteArray( HostFeaturesRequest ) );
    CHECK( server.requestConnections().at( 0 ) == 0 );
    CHECK( server.requestConnections().at( 1 ) == 1 );
}

TEST_CASE( "ADB smart-socket transport starts nonblocking and reaches Connected only after shell "
           "service readiness",
           "[livecapture][adb][transport][startup]" )
{
    FakeAdbServer server(
        []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& request ) {
            REQUIRE( connectionIndex <= 1 );
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request, false );
        } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );

    REQUIRE( probe.states.size() == 1u );
    CHECK( probe.states.front().generation == StreamGeneration );
    CHECK( probe.states.front().state == LiveSourceTransport::State::Connecting );
    CHECK( server.connectionCount() == 0 );
    CHECK( probe.received.empty() );

    REQUIRE( pumpEventsUntil( [ &server ] { return server.requestCount() == 4; } ) );
    REQUIRE( server.connectionCount() == 2 );
    REQUIRE( server.requests().size() == 4 );
    CHECK( server.requests().at( 0 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 1 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 2 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 3 ) == QByteArray( LogcatRequest ) );
    CHECK( server.requestConnections().at( 0 ) == 0 );
    CHECK( server.requestConnections().at( 1 ) == 0 );
    CHECK( server.requestConnections().at( 2 ) == 1 );
    CHECK( server.requestConnections().at( 3 ) == 1 );
    CHECK( server.peerWasLoopbackAtAccept( 0 ) );
    CHECK( server.peerWasLoopbackAtAccept( 1 ) );
    CHECK( probe.stateCount( LiveSourceTransport::State::Connected ) == 0 );
    CHECK( probe.received.empty() );

    FakeAdbServer::send( *server.socketAt( 1 ), QByteArrayLiteral( "OKAY" ) );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

    // Service OKAY transitions the client into shell-frame reads. It is readiness,
    // not data: no empty or synthetic log chunk may be published.
    CHECK( probe.received.empty() );
    CHECK( transport.lastError().isEmpty() );
    CHECK( transport.findChildren<QProcess*>().empty() );
    CHECK( socketFactory.connectedSocketCount() == 1 );

    transport.stop( StreamGeneration );
}

TEST_CASE( "ADB smart-socket stdout crosses the bounded queue byte-for-byte with boundary-local "
           "statistics",
           "[livecapture][adb][transport][stdout][queue][statistics]" )
{
    FakeAdbServer server(
        []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

    const auto first = QByteArray::fromHex( "1b5b33316d" ) + QByteArrayLiteral( "red\nsp" );
    const auto second = QByteArrayLiteral( "lit line\n" );
    const auto expected = first + second;
    FakeAdbServer::send( *server.socketAt( 1 ),
                         shellV2Frame( 1u, first ) + shellV2Frame( 1u, second ) );

    REQUIRE( pumpEventsUntil(
        [ &probe, &expected ] { return probe.receivedBytes().size() == expected.size(); } ) );
    CHECK( probe.receivedBytes() == expected );
    REQUIRE_FALSE( probe.received.empty() );
    CHECK( std::all_of(
        probe.received.begin(), probe.received.end(),
        []( const ObservedBytes& bytes ) { return bytes.generation == StreamGeneration; } ) );

    const auto statistics = transport.statistics();
    CHECK( statistics.generation == StreamGeneration );
    CHECK( statistics.receivedBytes == static_cast<std::size_t>( expected.size() ) );
    CHECK( statistics.receivedChunks == 2u );
    CHECK( statistics.queuedBytes == 0u );
    CHECK( statistics.queuedChunks == 0u );
    CHECK( statistics.deliveredBytes == static_cast<std::size_t>( expected.size() ) );
    CHECK( statistics.deliveredChunks == 2u );
    CHECK( statistics.backpressuredBytes == 0u );
    CHECK( statistics.backpressuredChunks == 0u );
    CHECK( statistics.highWaterQueuedBytes == static_cast<std::size_t>( expected.size() ) );
    CHECK( statistics.highWaterQueuedChunks == 2u );

    transport.stop( StreamGeneration );
}

TEST_CASE( "ADB smart-socket stderr remains diagnostic while shell exit fails the stream once",
           "[livecapture][adb][transport][stderr][exit][error]" )
{
    struct FailureCase {
        QByteArray diagnostic;
        QStringList expectedFragments;
    };
    const std::array cases{
        FailureCase{ QByteArrayLiteral( "logcat: permission denied\n" ),
                     { QStringLiteral( "permission denied" ), QStringLiteral( "17" ) } },
        FailureCase{ QByteArrayLiteral( "Invalid parameter year to -v\n" ),
                     { QStringLiteral( "Android 7.0" ),
                       QStringLiteral( "source-device wall time" ),
                       QStringLiteral( "Invalid parameter year to -v" ),
                       QStringLiteral( "17" ) } },
    };

    for ( const auto& value : cases ) {
        INFO( value.diagnostic.constData() );
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

        const auto stdoutBytes = QByteArrayLiteral( "captured stdout\n" );
        FakeAdbServer::send( *server.socketAt( 1 ),
                             shellV2Frame( 2u, value.diagnostic )
                                 + shellV2Frame( 1u, stdoutBytes ) );
        REQUIRE( pumpEventsUntil(
            [ &probe, &stdoutBytes ] { return probe.receivedBytes() == stdoutBytes; } ) );
        CHECK_FALSE( probe.receivedBytes().contains( value.diagnostic ) );
        CHECK( transport.lastError().isEmpty() );

        FakeAdbServer::send( *server.socketAt( 1 ),
                             shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 17 ) ) ) );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );

        requireSingleTerminalError( transport, probe, StreamGeneration, value.expectedFragments );
    }
}

TEST_CASE( "ADB smart-socket FAIL EOF and timeout failures map to one terminal transport error",
           "[livecapture][adb][transport][error]" )
{
    SECTION( "selected-device transport FAIL" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            REQUIRE( connectionIndex == 0 );
            REQUIRE( requestIndex == 0 );
            REQUIRE( request == QByteArray( TransportRequest ) );
            FakeAdbServer::send(
                socket, QByteArrayLiteral( "FAIL" )
                            + hostReplyFrame( QByteArrayLiteral(
                                "ADB server rejected host:transport:SERIAL-42" ) ) );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );
        requireSingleTerminalError(
            transport, probe, StreamGeneration,
            { QStringLiteral( "selected ADB device" ), QStringLiteral( "features" ),
              QStringLiteral( "rejected" ) } );
        CHECK( server.connectionCount() == 1 );
        CHECK( server.requestCount() == 1 );
    }

    SECTION( "selected-device features FAIL" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            REQUIRE( connectionIndex == 0 );
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }

            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
            FakeAdbServer::send(
                socket, QByteArrayLiteral( "FAIL" )
                            + hostReplyFrame( QByteArrayLiteral(
                                "ADB server rejected host:features for selected transport" ) ) );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );
        requireSingleTerminalError(
            transport, probe, StreamGeneration,
            { QStringLiteral( "selected ADB device" ), QStringLiteral( "features" ),
              QStringLiteral( "rejected" ) } );
        CHECK( server.connectionCount() == 1 );
        CHECK( server.requestCount() == 2 );
    }

    SECTION( "device logcat service FAIL" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            if ( connectionIndex == 0 ) {
                handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
                return;
            }

            REQUIRE( connectionIndex == 1 );
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }

            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArray( LogcatRequest ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "FAIL" )
                                             + hostReplyFrame( QByteArrayLiteral(
                                                 "device unauthorized for logcat" ) ) );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );
        requireSingleTerminalError(
            transport, probe, StreamGeneration,
            { QStringLiteral( "logcat" ), QStringLiteral( "unauthorized" ) } );
        CHECK( probe.stateCount( LiveSourceTransport::State::Connected ) == 0 );
    }

    SECTION( "EOF after Connected" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] {
            return probe.stateCount( LiveSourceTransport::State::Connected ) == 1;
        } ) );
        FakeAdbServer::sendThenClose( *server.socketAt( 1 ) );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );
        requireSingleTerminalError( transport, probe, StreamGeneration,
                                    { QStringLiteral( "EOF" ) } );
    }

    SECTION( "features read timeout" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            REQUIRE( connectionIndex == 0 );
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ & ] {
            return server.requestCount() == 2
                   && deadlines.hasActive( AdbSmartSocketDeadlineKind::Read );
        } ) );
        deadlines.fire( AdbSmartSocketDeadlineKind::Read );

        REQUIRE( probe.errors.size() == 1u );
        requireSingleTerminalError(
            transport, probe, StreamGeneration,
            { QStringLiteral( "selected ADB device" ), QStringLiteral( "features" ),
              QStringLiteral( "timed out" ) } );
    }
}

TEST_CASE( "ADB smart-socket fails safely when the server is replaced after feature negotiation",
           "[livecapture][adb][transport][features][replacement]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                              const QByteArray& request ) {
        if ( connectionIndex == 0 ) {
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
            sendFeaturesOkay( socket );
            return;
        }

        REQUIRE( connectionIndex == 1 );
        REQUIRE( requestIndex == 0 );
        REQUIRE( request == QByteArray( TransportRequest ) );
        FakeAdbServer::send( socket,
                             QByteArrayLiteral( "FAIL" )
                                 + hostReplyFrame( QByteArrayLiteral(
                                     "replacement ADB server rejected the selected transport" ) ) );
    } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );

    requireSingleTerminalError(
        transport, probe, StreamGeneration,
        { QStringLiteral( "replacement" ), QStringLiteral( "selected transport" ) } );
    CHECK( probe.stateCount( LiveSourceTransport::State::Connected ) == 0 );
    CHECK( probe.received.empty() );
    CHECK( server.connectionCount() == 2 );
    CHECK( server.requestCount() == 3 );
    processDeferredDeletes();
    CHECK( socketFactory.allSocketsRetired() );
}

TEST_CASE( "ADB smart-socket transport rejects selected devices without shell_v2 actionably and "
           "without modifying the environment",
           "[livecapture][adb][transport][features][compatibility]" )
{
    const auto originalPath = qgetenv( "PATH" );
    const auto originalServerSocket = qgetenv( "ADB_SERVER_SOCKET" );
    FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                              const QByteArray& request ) {
        REQUIRE( connectionIndex == 0 );
        if ( requestIndex == 0 ) {
            REQUIRE( request == QByteArray( TransportRequest ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }
        REQUIRE( requestIndex == 1 );
        REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
        sendFeaturesOkay( socket, QByteArrayLiteral( "cmd,shell_v2x,stat_v2,apex" ) );
    } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );

    requireSingleTerminalError(
        transport, probe, StreamGeneration,
        { QStringLiteral( "shell_v2" ), QStringLiteral( "selected ADB device" ) } );
    CHECK( server.connectionCount() == 1 );
    CHECK( server.requestCount() == 2 );
    CHECK( transport.findChildren<QProcess*>().empty() );
    CHECK( qgetenv( "PATH" ) == originalPath );
    CHECK( qgetenv( "ADB_SERVER_SOCKET" ) == originalServerSocket );
}

TEST_CASE( "ADB smart-socket stop cancels stream clients and queued data before stale callbacks "
           "escape",
           "[livecapture][adb][transport][stop][generation][queue]" )
{
    FakeAdbServer server(
        []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

    const auto clients = transport.findChildren<AdbSmartSocketClient*>();
    REQUIRE_FALSE( clients.empty() );
    for ( auto* client : clients ) {
        QObject::connect( client, &AdbSmartSocketClient::shellStdoutReceived, &transport,
                          [ &transport ]( Generation generation, AdbSmartSocketClient::OperationId,
                                          const QByteArray& ) {
                              if ( generation == StreamGeneration ) {
                                  transport.stop( generation );
                              }
                          } );
    }

    FakeAdbServer::send( *server.socketAt( 1 ),
                         shellV2Frame( 1u, QByteArrayLiteral( "queued-before-stop\n" ) )
                             + shellV2Frame( 1u, QByteArrayLiteral( "stale-after-stop\n" ) ) );
    REQUIRE( pumpEventsUntil( [ &probe ] {
        return probe.stateCount( LiveSourceTransport::State::Disconnected ) == 1;
    } ) );
    QCoreApplication::processEvents( QEventLoop::AllEvents );
    processDeferredDeletes();

    CHECK( probe.received.empty() );
    CHECK( probe.errors.empty() );
    CHECK( probe.clears.empty() );
    CHECK( probe.stateCount( LiveSourceTransport::State::Disconnected ) == 1 );
    CHECK( socketFactory.allSocketsRetired() );
    CHECK( transport.statistics().queuedBytes == 0u );
    CHECK( transport.statistics().queuedChunks == 0u );

    transport.stop( StreamGeneration );
    CHECK( probe.stateCount( LiveSourceTransport::State::Disconnected ) == 1 );
}

TEST_CASE( "ADB smart-socket reconnect uses new clients and fresh shell decoders",
           "[livecapture][adb][transport][reconnect][generation]" )
{
    FakeAdbServer server(
        []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    constexpr Generation replacementGeneration = StreamGeneration + 1u;
    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

    const auto partialOldFrame = shellV2Frame( 1u, QByteArrayLiteral( "old-partial" ) ).left( 4 );
    FakeAdbServer::send( *server.socketAt( 1 ), partialOldFrame );
    QCoreApplication::processEvents( QEventLoop::AllEvents );
    CHECK( probe.received.empty() );

    transport.stop( StreamGeneration );
    transport.start( replacementGeneration );
    REQUIRE( pumpEventsUntil( [ & ] {
        return server.connectionCount() == 4
               && probe.stateCount( LiveSourceTransport::State::Connected ) == 2;
    } ) );

    const auto fresh = QByteArrayLiteral( "fresh-generation\n" );
    FakeAdbServer::send( *server.socketAt( 3 ), shellV2Frame( 1u, fresh ) );
    REQUIRE( pumpEventsUntil( [ &probe, &fresh ] { return probe.receivedBytes() == fresh; } ) );
    REQUIRE( probe.received.size() == 1u );
    CHECK( probe.received.front().generation == replacementGeneration );
    CHECK( probe.received.front().bytes == fresh );
    CHECK( transport.statistics().generation == replacementGeneration );

    transport.stop( replacementGeneration );
}

TEST_CASE( "ADB smart-socket clear negotiates independently without an active stream",
           "[livecapture][adb][transport][clear][features]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                              const QByteArray& request ) {
        if ( connectionIndex == 0 ) {
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
            sendFeaturesOkay( socket );
            return;
        }

        REQUIRE( connectionIndex == 1 );
        if ( requestIndex == 0 ) {
            REQUIRE( request == QByteArray( TransportRequest ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }
        REQUIRE( requestIndex == 1 );
        REQUIRE( request == QByteArray( ClearRequest ) );
        FakeAdbServer::send( socket,
                             QByteArrayLiteral( "OKAY" )
                                 + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) ) );
    } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    constexpr Generation clearGeneration = StreamGeneration + 10u;
    constexpr LiveSourceTransport::ClearRequestId requestId = 7010u;
    transport.clearRemoteAsync( clearGeneration, requestId );

    CHECK( probe.clears.empty() );
    REQUIRE( pumpEventsUntil( [ &probe ] { return probe.clears.size() == 1u; } ) );
    REQUIRE( probe.clears.size() == 1u );
    CHECK( probe.clears.front().generation == clearGeneration );
    CHECK( probe.clears.front().requestId == requestId );
    CHECK( probe.clears.front().succeeded );
    CHECK( probe.clears.front().error.isEmpty() );
    CHECK( probe.states.empty() );
    CHECK( probe.errors.empty() );
    CHECK( transport.lastError().isEmpty() );
    REQUIRE( server.requests().size() == 4 );
    CHECK( server.requests().at( 0 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 1 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 2 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 3 ) == QByteArray( ClearRequest ) );
    CHECK( server.requestConnections().at( 0 ) == 0 );
    CHECK( server.requestConnections().at( 1 ) == 0 );
    CHECK( server.requestConnections().at( 2 ) == 1 );
    CHECK( server.requestConnections().at( 3 ) == 1 );
}

TEST_CASE( "ADB smart-socket clear uses an independent correlated operation without pausing stream",
           "[livecapture][adb][transport][clear]" )
{
    FakeAdbServer server(
        []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& request ) {
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }

            REQUIRE( requestIndex == 1 );
            if ( connectionIndex % 2 == 0 ) {
                REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
                sendFeaturesOkay( socket );
            }
            else if ( connectionIndex == 1 ) {
                REQUIRE( request == QByteArray( LogcatRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            }
            else {
                REQUIRE( connectionIndex == 3 );
                REQUIRE( request == QByteArray( ClearRequest ) );
                FakeAdbServer::send(
                    socket, QByteArrayLiteral( "OKAY" )
                                + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) ) );
            }
        } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );
    REQUIRE( socketFactory.connectedSocketCount() == 1 );

    constexpr LiveSourceTransport::ClearRequestId requestId = 7001u;
    transport.clearRemoteAsync( StreamGeneration, requestId );
    CHECK( probe.clears.empty() );
    REQUIRE( pumpEventsUntil( [ &probe ] { return probe.clears.size() == 1u; } ) );

    REQUIRE( probe.clears.size() == 1u );
    CHECK( probe.clears.front().generation == StreamGeneration );
    CHECK( probe.clears.front().requestId == requestId );
    CHECK( probe.clears.front().succeeded );
    CHECK( probe.clears.front().error.isEmpty() );
    CHECK( probe.stateCount( LiveSourceTransport::State::Connected ) == 1 );
    CHECK( probe.stateCount( LiveSourceTransport::State::Error ) == 0 );
    CHECK( probe.errors.empty() );
    CHECK( transport.lastError().isEmpty() );
    REQUIRE( server.requests().size() == 8 );
    CHECK( server.requests().at( 0 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 1 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 2 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 3 ) == QByteArray( LogcatRequest ) );
    CHECK( server.requests().at( 4 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 5 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 6 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 7 ) == QByteArray( ClearRequest ) );
    CHECK( server.requestConnections()
           == QVector<int>{ 0, 0, 1, 1, 2, 2, 3, 3 } );

    const auto afterClear = QByteArrayLiteral( "stream-still-running\n" );
    FakeAdbServer::send( *server.socketAt( 1 ), shellV2Frame( 1u, afterClear ) );
    REQUIRE( pumpEventsUntil(
        [ &probe, &afterClear ] { return probe.receivedBytes() == afterClear; } ) );

    transport.stop( StreamGeneration );
}

TEST_CASE( "ADB smart-socket clear failure remains request-local and leaves stream usable",
           "[livecapture][adb][transport][clear][error]" )
{
    const auto clearDiagnostic = QByteArrayLiteral( "logcat clear denied" );
    FakeAdbServer server( [ clearDiagnostic ]( QTcpSocket& socket, int connectionIndex,
                                               int requestIndex, const QByteArray& request ) {
        if ( requestIndex == 0 ) {
            REQUIRE( request == QByteArray( TransportRequest ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }

        REQUIRE( requestIndex == 1 );
        if ( connectionIndex % 2 == 0 ) {
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
            sendFeaturesOkay( socket );
        }
        else if ( connectionIndex == 1 ) {
            REQUIRE( request == QByteArray( LogcatRequest ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
        }
        else {
            REQUIRE( connectionIndex == 3 );
            REQUIRE( request == QByteArray( ClearRequest ) );
            FakeAdbServer::send( socket,
                                 QByteArrayLiteral( "FAIL" ) + hostReplyFrame( clearDiagnostic ) );
        }
    } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

    constexpr LiveSourceTransport::ClearRequestId requestId = 7002u;
    transport.clearRemoteAsync( StreamGeneration, requestId );
    REQUIRE( pumpEventsUntil( [ &probe ] { return probe.clears.size() == 1u; } ) );

    CHECK( probe.clears.front().generation == StreamGeneration );
    CHECK( probe.clears.front().requestId == requestId );
    CHECK_FALSE( probe.clears.front().succeeded );
    CHECK( probe.clears.front().error.contains( QString::fromUtf8( clearDiagnostic ) ) );
    CHECK( probe.errors.empty() );
    CHECK( probe.stateCount( LiveSourceTransport::State::Error ) == 0 );
    CHECK( probe.stateCount( LiveSourceTransport::State::Connected ) == 1 );
    CHECK( transport.lastError().isEmpty() );

    const auto afterFailure = QByteArrayLiteral( "stream-after-clear-failure\n" );
    FakeAdbServer::send( *server.socketAt( 1 ), shellV2Frame( 1u, afterFailure ) );
    REQUIRE( pumpEventsUntil(
        [ &probe, &afterFailure ] { return probe.receivedBytes() == afterFailure; } ) );

    transport.stop( StreamGeneration );
}

TEST_CASE( "ADB smart-socket delivers queued stdout before a coalesced terminal condition",
           "[livecapture][adb][transport][stdout][queue][ordering]" )
{
    SECTION( "shell exit" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );
        QStringList eventOrder;
        QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                          [ &eventOrder ]( Generation, const QByteArray& ) {
                              eventOrder.append( QStringLiteral( "bytes" ) );
                          } );
        QObject::connect( &transport, &LiveSourceTransport::errorOccurred,
                          [ &eventOrder ]( Generation, const QString& ) {
                              eventOrder.append( QStringLiteral( "error" ) );
                          } );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] {
            return probe.stateCount( LiveSourceTransport::State::Connected ) == 1;
        } ) );

        const auto finalStdout = QByteArrayLiteral( "final stdout before exit\n" );
        FakeAdbServer::send( *server.socketAt( 1 ),
                             shellV2Frame( 1u, finalStdout )
                                 + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 9 ) ) ) );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );

        CHECK( probe.receivedBytes() == finalStdout );
        CHECK( eventOrder == QStringList{ QStringLiteral( "bytes" ), QStringLiteral( "error" ) } );
        const auto statistics = transport.statistics();
        CHECK( statistics.queuedBytes == 0u );
        CHECK( statistics.queuedChunks == 0u );
        CHECK( statistics.deliveredBytes == static_cast<std::size_t>( finalStdout.size() ) );
        CHECK( statistics.deliveredChunks == 1u );
    }

    SECTION( "unexpected EOF" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );
        QStringList eventOrder;
        QObject::connect( &transport, &LiveSourceTransport::bytesReceived,
                          [ &eventOrder ]( Generation, const QByteArray& ) {
                              eventOrder.append( QStringLiteral( "bytes" ) );
                          } );
        QObject::connect( &transport, &LiveSourceTransport::errorOccurred,
                          [ &eventOrder ]( Generation, const QString& ) {
                              eventOrder.append( QStringLiteral( "error" ) );
                          } );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] {
            return probe.stateCount( LiveSourceTransport::State::Connected ) == 1;
        } ) );

        const auto finalStdout = QByteArrayLiteral( "final stdout before EOF\n" );
        FakeAdbServer::sendThenClose( *server.socketAt( 1 ), shellV2Frame( 1u, finalStdout ) );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );

        CHECK( probe.receivedBytes() == finalStdout );
        CHECK( eventOrder == QStringList{ QStringLiteral( "bytes" ), QStringLiteral( "error" ) } );
        const auto statistics = transport.statistics();
        CHECK( statistics.queuedBytes == 0u );
        CHECK( statistics.queuedChunks == 0u );
        CHECK( statistics.deliveredBytes == static_cast<std::size_t>( finalStdout.size() ) );
        CHECK( statistics.deliveredChunks == 1u );
    }
}

TEST_CASE( "AdbLogcatSource stop-then-clear workflow restarts through the smart-socket adapter",
           "[livecapture][adb][transport][source][clear][restart]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                              const QByteArray& request ) {
        if ( requestIndex == 0 ) {
            REQUIRE( request == QByteArray( TransportRequest ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }

        REQUIRE( requestIndex == 1 );
        if ( connectionIndex % 2 == 0 ) {
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
            sendFeaturesOkay( socket );
            return;
        }
        if ( request == QByteArray( LogcatRequest ) ) {
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }
        REQUIRE( request == QByteArray( ClearRequest ) );
        FakeAdbServer::send( socket,
                             QByteArrayLiteral( "OKAY" )
                                 + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) ) );
    } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    SmartSocketTestTransportFactory factory( transportConfig( server.port() ), socketFactory,
                                             deadlines );
    AdbLogcatSessionData sessionData;
    sessionData.sourceType = LiveLogSourceType::AdbLogcat;
    sessionData.deviceSerial = QString::fromLatin1( StreamSerial );
    AdbLogcatSource source( sessionData, {}, factory );

    REQUIRE( source.connectSource() );
    REQUIRE( pumpEventsUntil(
        [ &source ] { return source.state() == AdbLogcatSource::State::Connected; } ) );
    REQUIRE( source.clearAndRestart() );
    REQUIRE( pumpEventsUntil( [ & ] {
        return source.state() == AdbLogcatSource::State::Connected && server.requestCount() == 12;
    } ) );

    REQUIRE( server.requests().size() == 12 );
    CHECK( server.requests().at( 0 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 1 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 2 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 3 ) == QByteArray( LogcatRequest ) );
    CHECK( server.requests().at( 4 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 5 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 6 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 7 ) == QByteArray( ClearRequest ) );
    CHECK( server.requests().at( 8 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 9 ) == QByteArray( UnscopedFeaturesRequest ) );
    CHECK( server.requests().at( 10 ) == QByteArray( TransportRequest ) );
    CHECK( server.requests().at( 11 ) == QByteArray( LogcatRequest ) );
    CHECK( server.requestConnections()
           == QVector<int>{ 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5 } );
    CHECK( source.lastError().isEmpty() );

    source.disconnectSource();
}

TEST_CASE( "ADB smart-socket clear is independent from stream generation lifecycle",
           "[livecapture][adb][transport][clear][generation][isolation]" )
{
    SECTION( "a detached clear starts after the stream has stopped" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            if ( connectionIndex < 2 ) {
                handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
                return;
            }
            if ( connectionIndex == 2 ) {
                if ( requestIndex == 0 ) {
                    REQUIRE( request == QByteArray( TransportRequest ) );
                    FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                    return;
                }
                REQUIRE( requestIndex == 1 );
                REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
                sendFeaturesOkay( socket );
                return;
            }
            REQUIRE( connectionIndex == 3 );
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( request == QByteArray( ClearRequest ) );
            REQUIRE( requestIndex == 1 );
            FakeAdbServer::send(
                socket, QByteArrayLiteral( "OKAY" )
                            + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) ) );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] {
            return probe.stateCount( LiveSourceTransport::State::Connected ) == 1;
        } ) );
        transport.stop( StreamGeneration );

        constexpr Generation clearGeneration = StreamGeneration + 10u;
        constexpr LiveSourceTransport::ClearRequestId requestId = 7201u;
        transport.clearRemoteAsync( clearGeneration, requestId );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.clears.size() == 1u; } ) );

        REQUIRE( probe.clears.size() == 1u );
        CHECK( probe.clears.front().generation == clearGeneration );
        CHECK( probe.clears.front().requestId == requestId );
        CHECK( probe.clears.front().succeeded );
        CHECK( server.requestCount() == 8 );
        CHECK( probe.errors.empty() );
    }

    SECTION( "an in-flight clear survives stopping its stream generation" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            if ( request == QByteArray( ClearRequest ) ) {
                REQUIRE( connectionIndex == 3 );
                REQUIRE( requestIndex == 1 );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
        TrackingSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory,
                                           deadlines );
        TransportProbe probe( transport );

        transport.start( StreamGeneration );
        REQUIRE( pumpEventsUntil( [ &probe ] {
            return probe.stateCount( LiveSourceTransport::State::Connected ) == 1;
        } ) );
        constexpr LiveSourceTransport::ClearRequestId requestId = 7202u;
        transport.clearRemoteAsync( StreamGeneration, requestId );
        REQUIRE( pumpEventsUntil( [ &server ] { return server.requestCount() == 8; } ) );

        transport.stop( StreamGeneration );
        REQUIRE( socketFactory.connectedSocketCount() == 1 );
        FakeAdbServer::send( *server.socketAt( 3 ),
                             shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) ) );
        REQUIRE( pumpEventsUntil( [ &probe ] { return probe.clears.size() == 1u; } ) );

        CHECK( probe.clears.front().generation == StreamGeneration );
        CHECK( probe.clears.front().requestId == requestId );
        CHECK( probe.clears.front().succeeded );
        CHECK( probe.errors.empty() );
    }
}

TEST_CASE( "ADB smart-socket preserves a failed generation diagnostic across reentrant replacement",
           "[livecapture][adb][transport][error][reentrant][generation]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                              const QByteArray& request ) {
        if ( connectionIndex == 0 ) {
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArray( TransportRequest ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArray( UnscopedFeaturesRequest ) );
            FakeAdbServer::send(
                socket, QByteArrayLiteral( "FAIL" )
                            + hostReplyFrame( QByteArrayLiteral( "first generation rejected" ) ) );
            return;
        }
        handleSuccessfulStartupRequest( socket, connectionIndex - 1, requestIndex, request );
    } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport( transportConfig( server.port() ), socketFactory, deadlines );
    TransportProbe probe( transport );
    constexpr Generation replacementGeneration = StreamGeneration + 1u;

    QObject::connect( &transport, &LiveSourceTransport::stateChanged,
                      [ &transport ]( Generation generation, LiveSourceTransport::State state ) {
                          if ( generation == StreamGeneration
                               && state == LiveSourceTransport::State::Error ) {
                              transport.start( replacementGeneration );
                          }
                      } );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil( [ &probe ] {
        return std::any_of( probe.states.begin(), probe.states.end(),
                            []( const ObservedState& state ) {
                                return state.generation == replacementGeneration
                                       && state.state == LiveSourceTransport::State::Connected;
                            } );
    } ) );

    REQUIRE( probe.errors.size() == 1u );
    CHECK( probe.errors.front().generation == StreamGeneration );
    CHECK( probe.errors.front().error.contains( QStringLiteral( "first generation rejected" ) ) );
    CHECK( transport.lastError().isEmpty() );

    transport.stop( replacementGeneration );
}

TEST_CASE(
    "ADB smart-socket queue backpressure is an explicit terminal failure with exact counters",
    "[livecapture][adb][transport][queue][backpressure][statistics]" )
{
    FakeAdbServer server(
        []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& request ) {
            handleSuccessfulStartupRequest( socket, connectionIndex, requestIndex, request );
        } );
    TrackingSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketTransport transport(
        transportConfig( server.port(), LiveDataQueueLimits{ 4u, 1u } ), socketFactory, deadlines );
    TransportProbe probe( transport );

    transport.start( StreamGeneration );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.stateCount( LiveSourceTransport::State::Connected ) == 1; } ) );

    FakeAdbServer::send( *server.socketAt( 1 ),
                         shellV2Frame( 1u, QByteArrayLiteral( "abcd" ) )
                             + shellV2Frame( 1u, QByteArrayLiteral( "x" ) ) );
    REQUIRE( pumpEventsUntil( [ &probe ] { return probe.errors.size() == 1u; } ) );

    requireSingleTerminalError( transport, probe, StreamGeneration,
                                { QStringLiteral( "queue" ), QStringLiteral( "backpressure" ) } );
    const auto statistics = transport.statistics();
    CHECK( statistics.generation == StreamGeneration );
    CHECK( statistics.receivedBytes == 5u );
    CHECK( statistics.receivedChunks == 2u );
    CHECK( statistics.backpressuredBytes == 1u );
    CHECK( statistics.backpressuredChunks == 1u );
    CHECK( statistics.highWaterQueuedBytes == 4u );
    CHECK( statistics.highWaterQueuedChunks == 1u );
    CHECK( statistics.receivedBytes
           == statistics.deliveredBytes + statistics.queuedBytes + statistics.backpressuredBytes );
    CHECK( statistics.receivedChunks
           == statistics.deliveredChunks + statistics.queuedChunks
                  + statistics.backpressuredChunks );
}
