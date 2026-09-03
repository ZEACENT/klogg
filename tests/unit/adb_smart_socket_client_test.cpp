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
#include <QTcpServer>
#include <QTcpSocket>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adbprotocol.h"
#include "adbsmartsocketclient.h"

namespace {
using namespace klogg::livecapture;
using namespace klogg::livecapture::adb;

constexpr int EventPumpTimeoutMs = 1500;

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

    explicit FakeAdbServer( RequestHandler handler = {}, QObject* parent = nullptr )
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

    bool peerWasLoopbackAtAccept( int index ) const
    {
        return peerWasLoopbackAtAccept_.at( index );
    }

    QTcpSocket* socketAt( int index ) const
    {
        return sockets_.at( index );
    }

    static void send( QTcpSocket& socket, const QByteArray& bytes )
    {
        if ( !bytes.isEmpty() ) {
            REQUIRE( socket.write( bytes ) == bytes.size() );
            socket.flush();
        }
    }

    static void sendThenClose( QTcpSocket& socket, const QByteArray& bytes )
    {
        send( socket, bytes );
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
            peerWasLoopbackAtAccept_.append( socket->peerAddress().isLoopback() );
            buffers_.append( ConnectionBuffer{} );
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
            const auto chunkSize = std::min<qint64>( socket.bytesAvailable(), 7 );
            buffer.bytes.append( socket.read( chunkSize ) );
        }

        for ( ;; ) {
            if ( buffer.bytes.size() < 4 ) {
                return;
            }
            const auto length = parseRequestLength( buffer.bytes.left( 4 ) );
            const auto requestLength = length.value_or( -1 );
            REQUIRE( requestLength >= 0 );
            if ( buffer.bytes.size() - 4 < requestLength ) {
                return;
            }

            const auto request = buffer.bytes.mid( 4, requestLength );
            buffer.bytes.remove( 0, 4 + requestLength );
            const auto requestIndex = buffer.requestIndex++;
            requests_.append( request );
            requestConnections_.append( connectionIndex );
            if ( handler_ ) {
                handler_( socket, connectionIndex, requestIndex, request );
            }
        }
    }

private:
    QTcpServer server_;
    RequestHandler handler_;
    QVector<QTcpSocket*> sockets_;
    QVector<bool> peerWasLoopbackAtAccept_;
    QVector<ConnectionBuffer> buffers_;
    QVector<QByteArray> requests_;
    QVector<int> requestConnections_;
};

class InspectableTcpSocket final : public QTcpSocket {
public:
    enum class WriteMode : std::uint8_t { Normal, Partial, Stalled, AcceptedPending };

    InspectableTcpSocket( qint64 partialWriteLimit, WriteMode writeMode, QObject* parent )
        : QTcpSocket( parent )
        , partialWriteLimit_( partialWriteLimit )
        , writeMode_( writeMode )
    {
    }

    const QVector<qint64>& requestedReadSizes() const
    {
        return requestedReadSizes_;
    }

    const QVector<qint64>& requestedWriteSizes() const
    {
        return requestedWriteSizes_;
    }

    const QVector<qint64>& acceptedWriteSizes() const
    {
        return acceptedWriteSizes_;
    }

    qint64 bytesToWrite() const override
    {
        if ( writeMode_ == WriteMode::AcceptedPending && !acceptedWriteSizes_.empty() ) {
            return 1;
        }
        return QTcpSocket::bytesToWrite();
    }

protected:
    qint64 readData( char* data, qint64 maxSize ) override
    {
        requestedReadSizes_.append( maxSize );
        return QTcpSocket::readData( data, maxSize );
    }

    qint64 writeData( const char* data, qint64 maxSize ) override
    {
        requestedWriteSizes_.append( maxSize );
        if ( writeMode_ == WriteMode::Stalled ) {
            acceptedWriteSizes_.append( 0 );
            return 0;
        }
        if ( writeMode_ == WriteMode::AcceptedPending ) {
            acceptedWriteSizes_.append( maxSize );
            return maxSize;
        }

        const auto accepted
            = writeMode_ == WriteMode::Partial ? std::min( maxSize, partialWriteLimit_ ) : maxSize;
        const auto written = QTcpSocket::writeData( data, accepted );
        acceptedWriteSizes_.append( written );
        return written;
    }

private:
    qint64 partialWriteLimit_;
    WriteMode writeMode_;
    QVector<qint64> requestedReadSizes_;
    QVector<qint64> requestedWriteSizes_;
    QVector<qint64> acceptedWriteSizes_;
};

class InspectableSocketFactory final : public AdbSmartSocketFactory {
public:
    explicit InspectableSocketFactory( InspectableTcpSocket::WriteMode mode
                                       = InspectableTcpSocket::WriteMode::Normal,
                                       qint64 partialWriteLimit = 2 )
        : mode_( mode )
        , partialWriteLimit_( partialWriteLimit )
    {
    }

    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership is transferred to the supplied Qt parent.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const socket = new InspectableTcpSocket( partialWriteLimit_, mode_, parent );
        sockets_.append( socket );
        return socket;
    }

    qint64 readSocket( QTcpSocket& socket, char* data, qint64 maxSize ) override
    {
        requestedReadSizes_.append( maxSize );
        auto* const inspectable = dynamic_cast<InspectableTcpSocket*>( &socket );
        const auto previousProbeCount
            = inspectable != nullptr ? inspectable->requestedReadSizes().size() : 0;
        const auto bytesRead = AdbSmartSocketFactory::readSocket( socket, data, maxSize );
        if ( inspectable != nullptr ) {
            for ( auto index = previousProbeCount; index < inspectable->requestedReadSizes().size();
                  ++index ) {
                readDataProbeSizes_.append( inspectable->requestedReadSizes().at( index ) );
            }
        }
        return bytesRead;
    }

    qint64 writeSocket( QTcpSocket& socket, const char* data, qint64 maxSize ) override
    {
        requestedWriteSizes_.append( maxSize );
        const auto written = AdbSmartSocketFactory::writeSocket( socket, data, maxSize );
        acceptedWriteSizes_.append( written );
        return written;
    }

    const QVector<qint64>& requestedReadSizes() const
    {
        return requestedReadSizes_;
    }

    const QVector<qint64>& readDataProbeSizes() const
    {
        return readDataProbeSizes_;
    }

    const QVector<qint64>& requestedWriteSizes() const
    {
        return requestedWriteSizes_;
    }

    const QVector<qint64>& acceptedWriteSizes() const
    {
        return acceptedWriteSizes_;
    }

    int socketCount() const
    {
        return static_cast<int>( sockets_.size() );
    }

    int liveSocketCount() const
    {
        return static_cast<int>( std::count_if(
            sockets_.begin(), sockets_.end(),
            []( const QPointer<InspectableTcpSocket>& socket ) { return !socket.isNull(); } ) );
    }

    InspectableTcpSocket* socketAt( int index ) const
    {
        return sockets_.at( index ).data();
    }

private:
    InspectableTcpSocket::WriteMode mode_;
    qint64 partialWriteLimit_;
    QVector<QPointer<InspectableTcpSocket>> sockets_;
    QVector<qint64> requestedReadSizes_;
    QVector<qint64> readDataProbeSizes_;
    QVector<qint64> requestedWriteSizes_;
    QVector<qint64> acceptedWriteSizes_;
};

class ManualDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    struct Entry {
        DeadlineToken token{ 0 };
        AdbSmartSocketDeadlineKind kind{ AdbSmartSocketDeadlineKind::Connect };
        int timeoutMs{ 0 };
        QPointer<QObject> context;
        std::function<void()> callback;
        bool active{ true };
    };

    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind kind, int timeoutMs, QObject* context,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        entries_.push_back( Entry{ token, kind, timeoutMs, context, std::move( callback ), true } );
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
            return entry.active && entry.kind == kind && !entry.context.isNull();
        } );
    }

    std::size_t armCount( AdbSmartSocketDeadlineKind kind ) const
    {
        return static_cast<std::size_t>(
            std::count_if( entries_.cbegin(), entries_.cend(), [ kind ]( const Entry& entry ) {
                return entry.kind == kind;
            } ) );
    }

    DeadlineToken activeToken( AdbSmartSocketDeadlineKind kind ) const
    {
        const auto found
            = std::find_if( entries_.cbegin(), entries_.cend(), [ kind ]( const Entry& entry ) {
                  return entry.active && entry.kind == kind && !entry.context.isNull();
              } );
        REQUIRE( found != entries_.cend() );
        return found->token;
    }

    bool isActive( DeadlineToken token ) const
    {
        const auto found = std::find_if(
            entries_.cbegin(), entries_.cend(),
            [ token ]( const Entry& entry ) { return entry.token == token; } );
        REQUIRE( found != entries_.cend() );
        return found->active && !found->context.isNull();
    }

    int timeoutMs( DeadlineToken token ) const
    {
        const auto found = std::find_if(
            entries_.cbegin(), entries_.cend(),
            [ token ]( const Entry& entry ) { return entry.token == token; } );
        REQUIRE( found != entries_.cend() );
        return found->timeoutMs;
    }

    void fire( AdbSmartSocketDeadlineKind kind )
    {
        auto found
            = std::find_if( entries_.begin(), entries_.end(), [ kind ]( const Entry& entry ) {
                  return entry.active && entry.kind == kind && !entry.context.isNull();
              } );
        REQUIRE( found != entries_.end() );
        found->active = false;
        const auto callback = found->callback;
        callback();
    }

    void fire( DeadlineToken token )
    {
        auto found = std::find_if( entries_.begin(), entries_.end(),
                                  [ token ]( const Entry& entry ) {
                                      return entry.token == token && entry.active
                                             && !entry.context.isNull();
                                  } );
        REQUIRE( found != entries_.end() );
        found->active = false;
        const auto callback = found->callback;
        callback();
    }

    void fireLastEvenIfCancelled()
    {
        REQUIRE_FALSE( entries_.empty() );
        const auto callback = entries_.back().callback;
        callback();
    }

private:
    DeadlineToken nextToken_{ 0 };
    std::vector<Entry> entries_;
};

class ImmediateDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    explicit ImmediateDeadlineScheduler( AdbSmartSocketDeadlineKind immediateKind )
        : immediateKind_( immediateKind )
    {
    }

    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind kind, int, QObject*,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        if ( kind == immediateKind_ ) {
            immediateToken_ = token;
            callback();
        }
        return token;
    }

    void cancelDeadline( DeadlineToken token ) override
    {
        cancelledTokens_.push_back( token );
    }

    DeadlineToken immediateToken() const
    {
        return immediateToken_;
    }

    bool wasCancelled( DeadlineToken token ) const
    {
        return std::find( cancelledTokens_.begin(), cancelledTokens_.end(), token )
               != cancelledTokens_.end();
    }

private:
    AdbSmartSocketDeadlineKind immediateKind_;
    DeadlineToken nextToken_{ 0 };
    DeadlineToken immediateToken_{ 0 };
    std::vector<DeadlineToken> cancelledTokens_;
};

AdbSmartSocketClientConfig clientConfig( quint16 port )
{
    AdbSmartSocketClientConfig config;
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

struct ClientCallback {
    enum class Kind : std::uint8_t { Connected, HostReply, Stdout, Stderr, Exit, Error };

    Kind kind{ Kind::Connected };
    Generation generation{ 0 };
    AdbSmartSocketClient::OperationId operationId{ 0 };
    QByteArray bytes;
    int exitCode{ -1 };
    AdbSmartSocketErrorCode errorCode{ AdbSmartSocketErrorCode::Connection };
    QString diagnostic;
};

class ClientProbe final {
public:
    explicit ClientProbe( AdbSmartSocketClient& client )
    {
        QObject::connect(
            &client, &AdbSmartSocketClient::operationConnected,
            [ this ]( Generation generation, AdbSmartSocketClient::OperationId operationId ) {
                ClientCallback callback;
                callback.kind = ClientCallback::Kind::Connected;
                callback.generation = generation;
                callback.operationId = operationId;
                callbacks.push_back( std::move( callback ) );
            } );
        QObject::connect( &client, &AdbSmartSocketClient::hostReplyReceived,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    const QByteArray& reply ) {
                              ClientCallback callback;
                              callback.kind = ClientCallback::Kind::HostReply;
                              callback.generation = generation;
                              callback.operationId = operationId;
                              callback.bytes = reply;
                              callbacks.push_back( std::move( callback ) );
                          } );
        QObject::connect( &client, &AdbSmartSocketClient::shellStdoutReceived,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    const QByteArray& bytes ) {
                              ClientCallback callback;
                              callback.kind = ClientCallback::Kind::Stdout;
                              callback.generation = generation;
                              callback.operationId = operationId;
                              callback.bytes = bytes;
                              callbacks.push_back( std::move( callback ) );
                          } );
        QObject::connect( &client, &AdbSmartSocketClient::shellStderrReceived,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    const QByteArray& bytes ) {
                              ClientCallback callback;
                              callback.kind = ClientCallback::Kind::Stderr;
                              callback.generation = generation;
                              callback.operationId = operationId;
                              callback.bytes = bytes;
                              callbacks.push_back( std::move( callback ) );
                          } );
        QObject::connect( &client, &AdbSmartSocketClient::shellExited,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    std::uint8_t exitCode ) {
                              ClientCallback callback;
                              callback.kind = ClientCallback::Kind::Exit;
                              callback.generation = generation;
                              callback.operationId = operationId;
                              callback.exitCode = exitCode;
                              callbacks.push_back( std::move( callback ) );
                          } );
        QObject::connect( &client, &AdbSmartSocketClient::errorOccurred,
                          [ this ]( Generation generation,
                                    AdbSmartSocketClient::OperationId operationId,
                                    AdbSmartSocketErrorCode code, const QString& diagnostic ) {
                              ClientCallback callback;
                              callback.kind = ClientCallback::Kind::Error;
                              callback.generation = generation;
                              callback.operationId = operationId;
                              callback.errorCode = code;
                              callback.diagnostic = diagnostic;
                              callbacks.push_back( std::move( callback ) );
                          } );
    }

    int count( ClientCallback::Kind kind ) const
    {
        return static_cast<int>( std::count_if(
            callbacks.begin(), callbacks.end(),
            [ kind ]( const ClientCallback& callback ) { return callback.kind == kind; } ) );
    }

    const ClientCallback& first( ClientCallback::Kind kind ) const
    {
        const auto found = std::find_if(
            callbacks.begin(), callbacks.end(),
            [ kind ]( const ClientCallback& callback ) { return callback.kind == kind; } );
        REQUIRE( found != callbacks.end() );
        return *found;
    }

    bool allCallbacksMatch( Generation generation,
                            AdbSmartSocketClient::OperationId operationId ) const
    {
        return std::all_of( callbacks.begin(), callbacks.end(),
                            [ generation, operationId ]( const ClientCallback& callback ) {
                                return callback.generation == generation
                                       && callback.operationId == operationId;
                            } );
    }

    std::vector<ClientCallback> callbacks;
};

void requireHostUnexpectedEof( const QByteArray& responsePrefix )
{
    FakeAdbServer server( [ responsePrefix ]( QTcpSocket& socket, int, int, const QByteArray& ) {
        FakeAdbServer::sendThenClose( socket, responsePrefix );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    constexpr Generation generation = 501;
    constexpr AdbSmartSocketClient::OperationId operationId = 601;
    client.requestHostService( generation, operationId, HostService::ServerFeatures );

    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
    const auto& error = probe.first( ClientCallback::Kind::Error );
    CHECK( error.generation == generation );
    CHECK( error.operationId == operationId );
    CHECK( error.errorCode == AdbSmartSocketErrorCode::UnexpectedEof );
    CHECK_FALSE( error.diagnostic.isEmpty() );
    CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
}

void requireShellUnexpectedEof( const QByteArray& serviceResponsePrefix,
                                bool closeDuringTransportStatus )
{
    FakeAdbServer server( [ serviceResponsePrefix, closeDuringTransportStatus ](
                              QTcpSocket& socket, int, int requestIndex, const QByteArray& ) {
        if ( requestIndex == 0 ) {
            if ( closeDuringTransportStatus ) {
                FakeAdbServer::sendThenClose( socket, serviceResponsePrefix );
            }
            else {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            }
            return;
        }
        FakeAdbServer::sendThenClose( socket, serviceResponsePrefix );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    constexpr Generation generation = 701;
    constexpr AdbSmartSocketClient::OperationId operationId = 801;
    client.startShellService( generation, operationId,
                              TransportSelection{ TransportKind::Serial, "emulator-5554" },
                              std::string{ "shell,v2,raw:logcat -v color" } );

    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
    const auto& error = probe.first( ClientCallback::Kind::Error );
    CHECK( error.generation == generation );
    CHECK( error.operationId == operationId );
    CHECK( error.errorCode == AdbSmartSocketErrorCode::UnexpectedEof );
    CHECK_FALSE( error.diagnostic.isEmpty() );
    CHECK( probe.count( ClientCallback::Kind::Exit ) == 0 );
}

quint16 unusedLoopbackPort()
{
    QTcpServer reservation;
    REQUIRE( reservation.listen( QHostAddress::LocalHost, 0 ) );
    const auto port = reservation.serverPort();
    reservation.close();
    return port;
}

} // namespace

TEST_CASE( "ADB smart-socket host requests are loopback-only nonblocking bounded transactions",
           "[livecapture][adb][network][client]" )
{
    const QByteArray features = QByteArrayLiteral( "shell_v2,cmd,stat_v2,apex" );
    FakeAdbServer server( [ features ]( QTcpSocket& socket, int, int, const QByteArray& request ) {
        REQUIRE( request == QByteArrayLiteral( "host:host-features" ) );
        FakeAdbServer::sendThenClose( socket,
                                      QByteArrayLiteral( "OKAY" ) + hostReplyFrame( features ) );
    } );
    InspectableSocketFactory socketFactory( InspectableTcpSocket::WriteMode::Partial, 2 );
    ManualDeadlineScheduler deadlines;
    auto config = clientConfig( server.port() );
    config.maxReadChunkBytes = 3;
    config.maxWriteChunkBytes = 5;
    AdbSmartSocketClient client( config, socketFactory, deadlines );
    ClientProbe probe( client );

    constexpr Generation generation = 41;
    constexpr AdbSmartSocketClient::OperationId operationId = 7001;
    client.requestHostService( generation, operationId, HostService::ServerFeatures );

    // Starting the operation must only schedule connectToHost; it must not spin or block
    // until the local server accepts the connection.
    CHECK( server.connectionCount() == 0 );
    CHECK( probe.callbacks.empty() );

    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 1; } ) );
    REQUIRE( server.connectionCount() == 1 );
    REQUIRE( server.requestCount() == 1 );
    CHECK( server.requests().front() == QByteArrayLiteral( "host:host-features" ) );
    CHECK( server.peerWasLoopbackAtAccept( 0 ) );

    REQUIRE( probe.count( ClientCallback::Kind::Connected ) == 1 );
    REQUIRE( probe.count( ClientCallback::Kind::HostReply ) == 1 );
    CHECK( probe.first( ClientCallback::Kind::HostReply ).bytes == features );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
    CHECK( probe.allCallbacksMatch( generation, operationId ) );

    REQUIRE( socketFactory.socketCount() == 1 );
    REQUIRE_FALSE( socketFactory.requestedWriteSizes().empty() );
    CHECK( socketFactory.requestedWriteSizes().size() > 1 );
    CHECK( std::all_of( socketFactory.requestedWriteSizes().begin(),
                        socketFactory.requestedWriteSizes().end(),
                        []( qint64 size ) { return size > 0 && size <= 5; } ) );
    CHECK( std::all_of( socketFactory.acceptedWriteSizes().begin(),
                        socketFactory.acceptedWriteSizes().end(),
                        []( qint64 size ) { return size >= 0 && size <= 2; } ) );
    // Calling readAll() would request an implementation-selected large buffer. The
    // network adapter must instead repeatedly call read(maxReadChunkBytes). Qt 6.10
    // may additionally issue readData(nullptr, 0) probes inside QIODevice::read();
    // those framework-internal probes do not represent a client read request.
    REQUIRE_FALSE( socketFactory.requestedReadSizes().empty() );
    CHECK( std::all_of( socketFactory.requestedReadSizes().begin(),
                        socketFactory.requestedReadSizes().end(),
                        []( qint64 size ) { return size > 0 && size <= 3; } ) );
    CHECK( std::all_of( socketFactory.readDataProbeSizes().begin(),
                        socketFactory.readDataProbeSizes().end(),
                        []( qint64 size ) { return size == 0 || size <= 3; } ) );
}

TEST_CASE( "ADB smart-socket client sends exact features and devices-l host transactions",
           "[livecapture][adb][network][client][host]" )
{
    struct Scenario {
        HostService service;
        QByteArray request;
        QByteArray reply;
        AdbSmartSocketClient::OperationId operationId;
    };
    const std::vector<Scenario> scenarios{
        { HostService::ServerFeatures, QByteArrayLiteral( "host:host-features" ),
          QByteArrayLiteral( "shell_v2,cmd" ), 1001 },
        { HostService::DevicesLong, QByteArrayLiteral( "host:devices-l" ),
          QByteArrayLiteral( "emulator-5554\tdevice transport_id:1\n" ), 1002 },
    };

    for ( const auto& scenario : scenarios ) {
        DYNAMIC_SECTION( scenario.request.constData() )
        {
            FakeAdbServer server(
                [ &scenario ]( QTcpSocket& socket, int, int, const QByteArray& request ) {
                    REQUIRE( request == scenario.request );
                    FakeAdbServer::sendThenClose(
                        socket, QByteArrayLiteral( "OKAY" ) + hostReplyFrame( scenario.reply ) );
                } );
            InspectableSocketFactory socketFactory;
            ManualDeadlineScheduler deadlines;
            AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
            ClientProbe probe( client );

            constexpr Generation generation = 81;
            client.requestHostService( generation, scenario.operationId, scenario.service );

            REQUIRE( pumpEventsUntil(
                [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 1; } ) );
            CHECK( probe.first( ClientCallback::Kind::HostReply ).bytes == scenario.reply );
            CHECK( probe.allCallbacksMatch( generation, scenario.operationId ) );
            CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
        }
    }
}

TEST_CASE( "ADB smart-socket selected-device features use a transport-scoped host transaction",
           "[livecapture][adb][network][client][host][transport]" )
{
    constexpr Generation generation = 82;
    constexpr AdbSmartSocketClient::OperationId operationId = 1003;
    const TransportSelection transport{ TransportKind::Serial, "foo:bar" };
    const QByteArray features = QByteArrayLiteral( "shell_v2,cmd,stat_v2" );

    FakeAdbServer server( [ features ]( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                        const QByteArray& request ) {
        REQUIRE( connectionIndex == 0 );
        if ( requestIndex == 0 ) {
            REQUIRE( request == QByteArrayLiteral( "host:transport:foo:bar" ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }

        REQUIRE( requestIndex == 1 );
        REQUIRE( request == QByteArrayLiteral( "host:features" ) );
        FakeAdbServer::sendThenClose( socket,
                                      QByteArrayLiteral( "OKAY" ) + hostReplyFrame( features ) );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestTransportHostService( generation, operationId, transport,
                                        TransportHostService::Features );

    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 1; } ) );
    REQUIRE( server.connectionCount() == 1 );
    REQUIRE( socketFactory.socketCount() == 1 );
    REQUIRE( server.requestCount() == 2 );
    REQUIRE( server.requestConnections().size() == 2 );
    CHECK( server.requests().at( 0 ) == QByteArrayLiteral( "host:transport:foo:bar" ) );
    CHECK( server.requests().at( 1 ) == QByteArrayLiteral( "host:features" ) );
    CHECK( server.requestConnections().at( 0 ) == 0 );
    CHECK( server.requestConnections().at( 1 ) == 0 );
    CHECK( probe.first( ClientCallback::Kind::HostReply ).bytes == features );
    CHECK( probe.count( ClientCallback::Kind::HostReply ) == 1 );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
    CHECK( probe.allCallbacksMatch( generation, operationId ) );
}

TEST_CASE( "ADB smart-socket selected-device feature query preserves transport and feature FAIL diagnostics",
           "[livecapture][adb][network][client][host][transport][error]" )
{
    constexpr Generation generation = 83;
    const TransportSelection transport{ TransportKind::Serial, "emulator-5554" };

    SECTION( "transport FAIL" )
    {
        constexpr AdbSmartSocketClient::OperationId operationId = 1004;
        const QByteArray diagnostic = QByteArrayLiteral( "device 'emulator-5554' not found" );
        FakeAdbServer server(
            [ diagnostic ]( QTcpSocket& socket, int connectionIndex, int requestIndex,
                            const QByteArray& request ) {
                REQUIRE( connectionIndex == 0 );
                REQUIRE( requestIndex == 0 );
                REQUIRE( request == QByteArrayLiteral( "host:transport:emulator-5554" ) );
                FakeAdbServer::send(
                    socket, QByteArrayLiteral( "FAIL" ) + hostReplyFrame( diagnostic ) );
            } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.requestTransportHostService( generation, operationId, transport,
                                            TransportHostService::Features );

        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        REQUIRE( server.connectionCount() == 1 );
        REQUIRE( server.requestCount() == 1 );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::RemoteFailure );
        CHECK( error.diagnostic.contains( QString::fromUtf8( diagnostic ) ) );
        CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
        CHECK( probe.allCallbacksMatch( generation, operationId ) );
    }

    SECTION( "features FAIL" )
    {
        constexpr AdbSmartSocketClient::OperationId operationId = 1005;
        const QByteArray diagnostic = QByteArrayLiteral( "selected device rejected host:features" );
        FakeAdbServer server(
            [ diagnostic ]( QTcpSocket& socket, int connectionIndex, int requestIndex,
                            const QByteArray& request ) {
                REQUIRE( connectionIndex == 0 );
                if ( requestIndex == 0 ) {
                    REQUIRE( request == QByteArrayLiteral( "host:transport:emulator-5554" ) );
                    FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                    return;
                }

                REQUIRE( requestIndex == 1 );
                REQUIRE( request == QByteArrayLiteral( "host:features" ) );
                FakeAdbServer::send(
                    socket, QByteArrayLiteral( "FAIL" ) + hostReplyFrame( diagnostic ) );
            } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.requestTransportHostService( generation, operationId, transport,
                                            TransportHostService::Features );

        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        REQUIRE( server.connectionCount() == 1 );
        REQUIRE( server.requestCount() == 2 );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::RemoteFailure );
        CHECK( error.diagnostic.contains( QString::fromUtf8( diagnostic ) ) );
        CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
        CHECK( probe.allCallbacksMatch( generation, operationId ) );
    }
}

TEST_CASE( "ADB smart-socket selected-device feature replies reject malformed framing and trailing "
           "bytes",
           "[livecapture][adb][network][client][host][transport][protocol]" )
{
    struct Scenario {
        QByteArray response;
        AdbSmartSocketClient::OperationId operationId;
    };
    const std::vector<Scenario> scenarios{
        { QByteArrayLiteral( "OKAY00Z1" ), 1005 },
        { QByteArrayLiteral( "OKAY" ) + hostReplyFrame( QByteArrayLiteral( "one" ) )
              + hostReplyFrame( QByteArrayLiteral( "two" ) ),
          1006 },
        { QByteArrayLiteral( "OKAY" ) + hostReplyFrame( QByteArrayLiteral( "one" ) )
              + QByteArrayLiteral( "00" ),
          1007 },
    };

    for ( const auto& scenario : scenarios ) {
        DYNAMIC_SECTION( "response size " << scenario.response.size() )
        {
            FakeAdbServer server(
                [ &scenario ]( QTcpSocket& socket, int connectionIndex, int requestIndex,
                               const QByteArray& request ) {
                    REQUIRE( connectionIndex == 0 );
                    if ( requestIndex == 0 ) {
                        REQUIRE( request
                                 == QByteArrayLiteral( "host:transport:emulator-5554" ) );
                        FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                        return;
                    }
                    REQUIRE( requestIndex == 1 );
                    REQUIRE( request == QByteArrayLiteral( "host:features" ) );
                    FakeAdbServer::send( socket, scenario.response );
                } );
            InspectableSocketFactory socketFactory;
            ManualDeadlineScheduler deadlines;
            AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
            ClientProbe probe( client );

            constexpr Generation generation = 84;
            client.requestTransportHostService(
                generation, scenario.operationId,
                TransportSelection{ TransportKind::Serial, "emulator-5554" },
                TransportHostService::Features );

            REQUIRE( pumpEventsUntil(
                [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
            CHECK( server.connectionCount() == 1 );
            CHECK( server.requestCount() == 2 );
            CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
                   == AdbSmartSocketErrorCode::Protocol );
            CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
            CHECK( probe.allCallbacksMatch( generation, scenario.operationId ) );
        }
    }
}

TEST_CASE( "ADB smart-socket selected-device feature transaction preserves cancellation and "
           "timeout correlation",
           "[livecapture][adb][network][client][host][transport][cancel][deadline]" )
{
    const TransportSelection transport{ TransportKind::Serial, "emulator-5554" };

    SECTION( "cancellation while awaiting the one-shot host reply is silent" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex,
                                  const QByteArray& request ) {
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArrayLiteral( "host:transport:emulator-5554" ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArrayLiteral( "host:features" ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        constexpr Generation generation = 85;
        constexpr AdbSmartSocketClient::OperationId operationId = 1008;
        client.requestTransportHostService( generation, operationId, transport,
                                            TransportHostService::Features );
        REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
            return server.requestCount() == 2
                   && deadlines.hasActive( AdbSmartSocketDeadlineKind::Read );
        } ) );

        client.cancelGeneration( generation );
        deadlines.fireLastEvenIfCancelled();
        QCoreApplication::processEvents( QEventLoop::AllEvents );

        CHECK( server.connectionCount() == 1 );
        CHECK( server.requestCount() == 2 );
        CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
        CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
    }

    SECTION( "one-shot host reply read timeout keeps operation correlation" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex,
                                  const QByteArray& request ) {
            if ( requestIndex == 0 ) {
                REQUIRE( request == QByteArrayLiteral( "host:transport:emulator-5554" ) );
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }
            REQUIRE( requestIndex == 1 );
            REQUIRE( request == QByteArrayLiteral( "host:features" ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        constexpr Generation generation = 86;
        constexpr AdbSmartSocketClient::OperationId operationId = 1009;
        client.requestTransportHostService( generation, operationId, transport,
                                            TransportHostService::Features );
        REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
            return server.requestCount() == 2
                   && deadlines.hasActive( AdbSmartSocketDeadlineKind::Read );
        } ) );

        deadlines.fire( AdbSmartSocketDeadlineKind::Read );

        REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
        CHECK( server.connectionCount() == 1 );
        CHECK( server.requestCount() == 2 );
        CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
               == AdbSmartSocketErrorCode::ReadTimeout );
        CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
        CHECK( probe.allCallbacksMatch( generation, operationId ) );
    }
}

TEST_CASE( "ADB one-shot host reply rearms a full EOF deadline while the socket stays open",
           "[livecapture][adb][network][client][host][eof][deadline]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int, int, const QByteArray& request ) {
        REQUIRE( request == QByteArrayLiteral( "host:host-features" ) );
        FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    const auto config = clientConfig( server.port() );
    AdbSmartSocketClient client( config, socketFactory, deadlines );
    ClientProbe probe( client );

    constexpr Generation generation = 87;
    constexpr AdbSmartSocketClient::OperationId operationId = 1010;
    client.requestHostService( generation, operationId, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
        return server.requestCount() == 1
               && deadlines.armCount( AdbSmartSocketDeadlineKind::Read ) >= 2u
               && deadlines.hasActive( AdbSmartSocketDeadlineKind::Read );
    } ) );

    const auto payloadReadDeadline
        = deadlines.activeToken( AdbSmartSocketDeadlineKind::Read );
    REQUIRE( deadlines.timeoutMs( payloadReadDeadline ) == config.readTimeoutMs );
    const auto readsBeforeReply = socketFactory.requestedReadSizes().size();
    FakeAdbServer::send( *server.socketAt( 0 ),
                         hostReplyFrame( QByteArrayLiteral( "shell_v2,cmd" ) ) );
    REQUIRE( pumpEventsUntil( [ &socketFactory, readsBeforeReply ] {
        return socketFactory.requestedReadSizes().size() > readsBeforeReply;
    } ) );

    CHECK( server.socketAt( 0 )->state() == QAbstractSocket::ConnectedState );
    CHECK_FALSE( deadlines.isActive( payloadReadDeadline ) );
    CHECK( deadlines.armCount( AdbSmartSocketDeadlineKind::Read ) == 3u );
    REQUIRE( deadlines.hasActive( AdbSmartSocketDeadlineKind::Read ) );
    const auto eofDeadline = deadlines.activeToken( AdbSmartSocketDeadlineKind::Read );
    CHECK( eofDeadline != payloadReadDeadline );
    CHECK( deadlines.timeoutMs( eofDeadline ) == config.readTimeoutMs );
    CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );

    deadlines.fire( eofDeadline );

    REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
    const auto& error = probe.first( ClientCallback::Kind::Error );
    CHECK( error.errorCode == AdbSmartSocketErrorCode::ReadTimeout );
    CHECK( error.generation == generation );
    CHECK( error.operationId == operationId );
    CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
}

TEST_CASE( "ADB smart-socket shell operation sequences transport and service on one socket",
           "[livecapture][adb][network][client][shell-v2]" )
{
    const QByteArray service = QByteArrayLiteral( "shell,v2,raw:logcat -v color" );
    FakeAdbServer server( [ service ]( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                       const QByteArray& request ) {
        REQUIRE( connectionIndex == 0 );
        if ( requestIndex == 0 ) {
            REQUIRE( request == QByteArrayLiteral( "host:transport:emulator-5554" ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            return;
        }

        REQUIRE( requestIndex == 1 );
        REQUIRE( request == service );
        const auto coalesced = QByteArrayLiteral( "OKAY" )
                               + shellV2Frame( 1u, QByteArrayLiteral( "stdout line\n" ) )
                               + shellV2Frame( 2u, QByteArrayLiteral( "stderr line\n" ) )
                               + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 23 ) ) );
        FakeAdbServer::send( socket, coalesced );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    auto config = clientConfig( server.port() );
    config.maxReadChunkBytes = 4;
    AdbSmartSocketClient client( config, socketFactory, deadlines );
    ClientProbe probe( client );

    constexpr Generation generation = 91;
    constexpr AdbSmartSocketClient::OperationId operationId = 2001;
    client.startShellService( generation, operationId,
                              TransportSelection{ TransportKind::Serial, "emulator-5554" },
                              service.toStdString() );

    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Exit ) == 1; } ) );
    REQUIRE( server.connectionCount() == 1 );
    REQUIRE( server.requestCount() == 2 );
    REQUIRE( server.requestConnections().size() == 2 );
    CHECK( server.requestConnections().at( 0 ) == 0 );
    CHECK( server.requestConnections().at( 1 ) == 0 );
    CHECK( probe.first( ClientCallback::Kind::Stdout ).bytes
           == QByteArrayLiteral( "stdout line\n" ) );
    CHECK( probe.first( ClientCallback::Kind::Stderr ).bytes
           == QByteArrayLiteral( "stderr line\n" ) );
    CHECK( probe.first( ClientCallback::Kind::Exit ).exitCode == 23 );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
    CHECK( probe.allCallbacksMatch( generation, operationId ) );
}

TEST_CASE( "ADB smart-socket preserves valid shell output but rejects bytes after exit",
           "[livecapture][adb][network][client][shell-v2][ordering]" )
{
    const QByteArray service = QByteArrayLiteral( "shell,v2,raw:logcat -v color" );

    SECTION( "complete frame after exit" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex, const QByteArray& ) {
            if ( requestIndex == 0 ) {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }

            FakeAdbServer::send( socket,
                                 QByteArrayLiteral( "OKAY" )
                                     + shellV2Frame( 1u, QByteArrayLiteral( "before-exit\n" ) )
                                     + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) )
                                     + shellV2Frame( 1u, QByteArrayLiteral( "after-exit\n" ) ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.startShellService( 92u, 2002u,
                                  TransportSelection{ TransportKind::Serial, "emulator-5554" },
                                  service.toStdString() );

        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        CHECK( probe.count( ClientCallback::Kind::Stdout ) == 1 );
        CHECK( probe.first( ClientCallback::Kind::Stdout ).bytes
               == QByteArrayLiteral( "before-exit\n" ) );
        CHECK( probe.count( ClientCallback::Kind::Exit ) == 0 );
        CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
               == AdbSmartSocketErrorCode::Protocol );
        CHECK( probe.first( ClientCallback::Kind::Error )
                   .diagnostic.contains( QStringLiteral( "exit" ), Qt::CaseInsensitive ) );
    }

    SECTION( "partial frame bytes after exit" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex, const QByteArray& ) {
            if ( requestIndex == 0 ) {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }

            FakeAdbServer::send( socket,
                                 QByteArrayLiteral( "OKAY" )
                                     + shellV2Frame( 1u, QByteArrayLiteral( "before-partial\n" ) )
                                     + shellV2Frame( 3u, QByteArray( 1, static_cast<char>( 0 ) ) )
                                     + QByteArray::fromHex( "0102" ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.startShellService( 94u, 2004u,
                                  TransportSelection{ TransportKind::Serial, "emulator-5554" },
                                  service.toStdString() );

        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        CHECK( probe.count( ClientCallback::Kind::Stdout ) == 1 );
        CHECK( probe.first( ClientCallback::Kind::Stdout ).bytes
               == QByteArrayLiteral( "before-partial\n" ) );
        CHECK( probe.count( ClientCallback::Kind::Exit ) == 0 );
        CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
               == AdbSmartSocketErrorCode::Protocol );
    }

    SECTION( "malformed frame after valid stdout" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex, const QByteArray& ) {
            if ( requestIndex == 0 ) {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                return;
            }

            FakeAdbServer::send( socket,
                                 QByteArrayLiteral( "OKAY" )
                                     + shellV2Frame( 1u, QByteArrayLiteral( "valid-prefix\n" ) )
                                     + QByteArray::fromHex( "7f00000000" ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.startShellService( 93u, 2003u,
                                  TransportSelection{ TransportKind::Serial, "emulator-5554" },
                                  service.toStdString() );

        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        CHECK( probe.count( ClientCallback::Kind::Stdout ) == 1 );
        CHECK( probe.first( ClientCallback::Kind::Stdout ).bytes
               == QByteArrayLiteral( "valid-prefix\n" ) );
        CHECK( probe.count( ClientCallback::Kind::Exit ) == 0 );
        CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
               == AdbSmartSocketErrorCode::Protocol );
    }
}

TEST_CASE( "ADB smart-socket FAIL replies preserve remote diagnostics",
           "[livecapture][adb][network][client][error]" )
{
    const QByteArray diagnostic = QByteArrayLiteral( "device unauthorized; accept RSA key" );

    SECTION( "host service" )
    {
        FakeAdbServer server( [ diagnostic ]( QTcpSocket& socket, int, int, const QByteArray& ) {
            FakeAdbServer::send( socket,
                                 QByteArrayLiteral( "FAIL" ) + hostReplyFrame( diagnostic ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.requestHostService( 101, 3001, HostService::ServerFeatures );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::RemoteFailure );
        CHECK( error.diagnostic.contains( QString::fromUtf8( diagnostic ) ) );
        CHECK( probe.allCallbacksMatch( 101, 3001 ) );
    }

    SECTION( "shell service after successful transport selection" )
    {
        FakeAdbServer server( [ diagnostic ]( QTcpSocket& socket, int, int requestIndex,
                                              const QByteArray& ) {
            if ( requestIndex == 0 ) {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            }
            else {
                FakeAdbServer::send( socket,
                                     QByteArrayLiteral( "FAIL" ) + hostReplyFrame( diagnostic ) );
            }
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.startShellService( 102, 3002, TransportSelection{ TransportKind::Any, {} },
                                  std::string{ "shell,v2,raw:logcat -v color" } );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::RemoteFailure );
        CHECK( error.diagnostic.contains( QString::fromUtf8( diagnostic ) ) );
        CHECK( probe.allCallbacksMatch( 102, 3002 ) );
    }
}

TEST_CASE( "ADB smart-socket host EOF is diagnosed at every status length and payload boundary",
           "[livecapture][adb][network][client][eof]" )
{
    const auto okayReply
        = QByteArrayLiteral( "OKAY" ) + hostReplyFrame( QByteArrayLiteral( "reply" ) );
    for ( int cut = 0; cut < okayReply.size(); ++cut ) {
        DYNAMIC_SECTION( "OKAY host reply cut after " << cut << " bytes" )
        {
            requireHostUnexpectedEof( okayReply.left( cut ) );
        }
    }

    const auto failReply
        = QByteArrayLiteral( "FAIL" ) + hostReplyFrame( QByteArrayLiteral( "error" ) );
    for ( int cut = 0; cut < failReply.size(); ++cut ) {
        DYNAMIC_SECTION( "FAIL diagnostic cut after " << cut << " bytes" )
        {
            requireHostUnexpectedEof( failReply.left( cut ) );
        }
    }
}

TEST_CASE( "ADB smart-socket shell EOF is diagnosed at transport status service status and frame "
           "boundaries",
           "[livecapture][adb][network][client][shell-v2][eof]" )
{
    const auto okay = QByteArrayLiteral( "OKAY" );
    for ( int cut = 0; cut < okay.size(); ++cut ) {
        DYNAMIC_SECTION( "transport status cut after " << cut << " bytes" )
        {
            requireShellUnexpectedEof( okay.left( cut ), true );
        }
    }

    const auto serviceAndFrame = okay + shellV2Frame( 1u, QByteArrayLiteral( "stdout" ) );
    for ( int cut = 0; cut < serviceAndFrame.size(); ++cut ) {
        DYNAMIC_SECTION( "shell service/frame cut after " << cut << " bytes" )
        {
            requireShellUnexpectedEof( serviceAndFrame.left( cut ), false );
        }
    }
}

TEST_CASE( "ADB smart-socket deadlines are deterministic for connect write and read phases",
           "[livecapture][adb][network][client][deadline]" )
{
    SECTION( "connect deadline" )
    {
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( unusedLoopbackPort() ), socketFactory,
                                     deadlines );
        ClientProbe probe( client );

        client.requestHostService( 201, 4001, HostService::ServerFeatures );
        REQUIRE( deadlines.hasActive( AdbSmartSocketDeadlineKind::Connect ) );
        deadlines.fire( AdbSmartSocketDeadlineKind::Connect );

        REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::ConnectTimeout );
        CHECK( error.diagnostic.contains( QStringLiteral( "connect" ), Qt::CaseInsensitive ) );
        CHECK( probe.allCallbacksMatch( 201, 4001 ) );
    }

    SECTION( "write deadline" )
    {
        FakeAdbServer server;
        InspectableSocketFactory socketFactory( InspectableTcpSocket::WriteMode::Stalled );
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.requestHostService( 202, 4002, HostService::ServerFeatures );
        REQUIRE( pumpEventsUntil(
            [ &deadlines ] { return deadlines.hasActive( AdbSmartSocketDeadlineKind::Write ); } ) );
        deadlines.fire( AdbSmartSocketDeadlineKind::Write );

        REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::WriteTimeout );
        CHECK( error.diagnostic.contains( QStringLiteral( "write" ), Qt::CaseInsensitive ) );
        CHECK( probe.allCallbacksMatch( 202, 4002 ) );
    }

    SECTION( "read deadline" )
    {
        FakeAdbServer server( []( QTcpSocket&, int, int, const QByteArray& ) {
            // Deliberately retain the accepted socket without replying.
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );

        client.requestHostService( 203, 4003, HostService::DevicesLong );
        REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
            return server.requestCount() == 1
                   && deadlines.hasActive( AdbSmartSocketDeadlineKind::Read );
        } ) );
        deadlines.fire( AdbSmartSocketDeadlineKind::Read );

        REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::ReadTimeout );
        CHECK( error.diagnostic.contains( QStringLiteral( "read" ), Qt::CaseInsensitive ) );
        CHECK( probe.allCallbacksMatch( 203, 4003 ) );
    }
}

TEST_CASE( "ADB smart-socket cancels a deadline token returned after a synchronous expiry",
           "[livecapture][adb][network][client][deadline][reentrant]" )
{
    FakeAdbServer server;
    InspectableSocketFactory socketFactory;
    ImmediateDeadlineScheduler deadlines( AdbSmartSocketDeadlineKind::Read );
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 251, 4501, HostService::ServerFeatures );

    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
    const auto token = deadlines.immediateToken();
    REQUIRE( token != 0u );
    CHECK( deadlines.wasCancelled( token ) );
    const auto& error = probe.first( ClientCallback::Kind::Error );
    CHECK( error.errorCode == AdbSmartSocketErrorCode::ReadTimeout );
    CHECK( probe.allCallbacksMatch( 251, 4501 ) );
}

TEST_CASE( "ADB smart-socket cancel invalidates generation and closes without an error callback",
           "[livecapture][adb][network][client][cancel]" )
{
    FakeAdbServer server( []( QTcpSocket&, int, int, const QByteArray& ) {
        // Hold the request in the read phase until cancellation.
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    constexpr Generation generation = 301;
    client.requestHostService( generation, 5001, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
        return server.requestCount() == 1
               && deadlines.hasActive( AdbSmartSocketDeadlineKind::Read );
    } ) );
    REQUIRE( socketFactory.socketCount() == 1 );
    QPointer<InspectableTcpSocket> cancelledSocket( socketFactory.socketAt( 0 ) );

    client.cancelGeneration( generation );

    CHECK( cancelledSocket->state() == QAbstractSocket::UnconnectedState );
    // A deadline already queued by the event dispatcher must still be harmless after
    // generation invalidation, even if the scheduler invokes its stale closure.
    deadlines.fireLastEvenIfCancelled();
    QCoreApplication::processEvents( QEventLoop::AllEvents );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
    CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
    processDeferredDeletes();
    CHECK( cancelledSocket.isNull() );
}

TEST_CASE( "ADB smart-socket reconnect reconstructs sockets and resets partial decoders",
           "[livecapture][adb][network][client][reconnect]" )
{
    const QByteArray secondReply = QByteArrayLiteral( "shell_v2,cmd" );
    FakeAdbServer server( [ secondReply ]( QTcpSocket& socket, int connectionIndex, int,
                                           const QByteArray& ) {
        if ( connectionIndex == 0 ) {
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKA" ) );
            return;
        }
        FakeAdbServer::sendThenClose(
            socket, QByteArrayLiteral( "OKAY" ) + hostReplyFrame( secondReply ) );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 401, 6001, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil( [ &server ] { return server.requestCount() == 1; } ) );
    client.cancelGeneration( 401 );

    client.requestHostService( 402, 6002, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 1; } ) );

    REQUIRE( server.connectionCount() == 2 );
    REQUIRE( socketFactory.socketCount() == 2 );
    REQUIRE( probe.count( ClientCallback::Kind::HostReply ) == 1 );
    const auto& reply = probe.first( ClientCallback::Kind::HostReply );
    CHECK( reply.generation == 402 );
    CHECK( reply.operationId == 6002 );
    CHECK( reply.bytes == secondReply );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
}

TEST_CASE(
    "ADB smart-socket client rejects oversized host and shell-v2 frames before payload buffering",
    "[livecapture][adb][network][client][bounds]" )
{
    SECTION( "host frame" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int, const QByteArray& ) {
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY0009" ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        auto config = clientConfig( server.port() );
        config.maxHostReplyBytes = 8;
        AdbSmartSocketClient client( config, socketFactory, deadlines );
        ClientProbe probe( client );

        client.requestHostService( 601, 7001, HostService::ServerFeatures );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::Protocol );
        CHECK( error.diagnostic.contains( QStringLiteral( "exceed" ), Qt::CaseInsensitive ) );
        CHECK( probe.allCallbacksMatch( 601, 7001 ) );
    }

    SECTION( "shell-v2 frame" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex, const QByteArray& ) {
            if ( requestIndex == 0 ) {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
            }
            else {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" )
                                                 + QByteArray::fromHex( "0105000000" ) );
            }
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        auto config = clientConfig( server.port() );
        config.maxShellFrameBytes = 4;
        AdbSmartSocketClient client( config, socketFactory, deadlines );
        ClientProbe probe( client );

        client.startShellService( 602, 7002, TransportSelection{ TransportKind::Usb, {} },
                                  std::string{ "shell,v2,raw:logcat -v color" } );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
        const auto& error = probe.first( ClientCallback::Kind::Error );
        CHECK( error.errorCode == AdbSmartSocketErrorCode::Protocol );
        CHECK( error.diagnostic.contains( QStringLiteral( "exceed" ), Qt::CaseInsensitive ) );
        CHECK( probe.allCallbacksMatch( 602, 7002 ) );
    }
}

TEST_CASE( "ADB track-devices host requests remain open for repeated snapshots",
           "[livecapture][adb][network][client][host][streaming]" )
{
    const auto first = QByteArrayLiteral( "emulator-5554\tdevice\n" );
    const auto second = QByteArrayLiteral( "emulator-5554\toffline\n" );
    const auto third = QByteArrayLiteral( "emulator-5554\tdevice product:sdk\n" );
    FakeAdbServer server(
        [ &first, &second ]( QTcpSocket& socket, int, int, const QByteArray& request ) {
            REQUIRE( request == QByteArrayLiteral( "host:track-devices-l" ) );
            FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) + hostReplyFrame( first )
                                             + hostReplyFrame( second ) );
        } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 701, 8001, HostService::TrackDevicesLong );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 2; } ) );
    REQUIRE( server.connectionCount() == 1 );
    FakeAdbServer::send( *server.socketAt( 0 ), hostReplyFrame( third ) );
    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 3; } ) );

    CHECK( probe.callbacks.at( 1 ).bytes == first );
    CHECK( probe.callbacks.at( 2 ).bytes == second );
    CHECK( probe.callbacks.at( 3 ).bytes == third );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
    CHECK( probe.allCallbacksMatch( 701, 8001 ) );

    client.cancelGeneration( 701 );
    processDeferredDeletes();
    CHECK( socketFactory.liveSocketCount() == 0 );
}

TEST_CASE( "ADB smart-socket terminal callbacks may synchronously start successor operations",
           "[livecapture][adb][network][client][reentrant]" )
{
    SECTION( "host reply starts another host request" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int,
                                  const QByteArray& ) {
            const auto reply = connectionIndex == 0 ? QByteArrayLiteral( "first" )
                                                    : QByteArrayLiteral( "second" );
            FakeAdbServer::sendThenClose( socket,
                                          QByteArrayLiteral( "OKAY" ) + hostReplyFrame( reply ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );
        QObject::connect( &client, &AdbSmartSocketClient::hostReplyReceived, &client,
                          [ &client ]( Generation, AdbSmartSocketClient::OperationId operationId,
                                       const QByteArray& ) {
                              if ( operationId == 8101 ) {
                                  client.requestHostService( 711, 8102, HostService::DevicesLong );
                              }
                          } );

        client.requestHostService( 711, 8101, HostService::ServerFeatures );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 2; } ) );
        CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
        CHECK( server.connectionCount() == 2 );
    }

    SECTION( "selected-device host reply starts another host request" )
    {
        FakeAdbServer server( []( QTcpSocket& socket, int connectionIndex, int requestIndex,
                                  const QByteArray& request ) {
            if ( connectionIndex == 0 ) {
                if ( requestIndex == 0 ) {
                    REQUIRE( request
                             == QByteArrayLiteral( "host:transport:emulator-5554" ) );
                    FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                    return;
                }

                REQUIRE( requestIndex == 1 );
                REQUIRE( request == QByteArrayLiteral( "host:features" ) );
                FakeAdbServer::sendThenClose(
                    socket, QByteArrayLiteral( "OKAY" )
                                + hostReplyFrame( QByteArrayLiteral( "selected" ) ) );
                return;
            }

            REQUIRE( connectionIndex == 1 );
            REQUIRE( requestIndex == 0 );
            REQUIRE( request == QByteArrayLiteral( "host:version" ) );
            FakeAdbServer::sendThenClose(
                socket, QByteArrayLiteral( "OKAY" )
                            + hostReplyFrame( QByteArrayLiteral( "0029" ) ) );
        } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );
        QObject::connect( &client, &AdbSmartSocketClient::hostReplyReceived, &client,
                          [ &client ]( Generation, AdbSmartSocketClient::OperationId operationId,
                                       const QByteArray& ) {
                              if ( operationId == 8151 ) {
                                  client.requestHostService( 711, 8152, HostService::Version );
                              }
                          } );

        client.requestTransportHostService(
            711, 8151, TransportSelection{ TransportKind::Serial, "emulator-5554" },
            TransportHostService::Features );
        REQUIRE( pumpEventsUntil(
            [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 2; } ) );
        CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
        CHECK( server.connectionCount() == 2 );
        CHECK( server.requestCount() == 3 );
    }

    SECTION( "shell exit starts a host request" )
    {
        FakeAdbServer server(
            []( QTcpSocket& socket, int connectionIndex, int requestIndex, const QByteArray& ) {
                if ( connectionIndex == 0 && requestIndex == 0 ) {
                    FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) );
                }
                else if ( connectionIndex == 0 ) {
                    FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" )
                                                     + shellV2Frame( 3u, QByteArray( 1, '\0' ) ) );
                }
                else {
                    FakeAdbServer::sendThenClose(
                        socket, QByteArrayLiteral( "OKAY" )
                                    + hostReplyFrame( QByteArrayLiteral( "features" ) ) );
                }
            } );
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
        ClientProbe probe( client );
        QObject::connect(
            &client, &AdbSmartSocketClient::shellExited, &client,
            [ &client ]( Generation, AdbSmartSocketClient::OperationId, std::uint8_t ) {
                client.requestHostService( 712, 8202, HostService::ServerFeatures );
            } );

        client.startShellService( 712, 8201, TransportSelection{ TransportKind::Any, {} },
                                  std::string{ "shell,v2,raw:true" } );
        REQUIRE( pumpEventsUntil( [ &probe ] {
            return probe.count( ClientCallback::Kind::Exit ) == 1
                   && probe.count( ClientCallback::Kind::HostReply ) == 1;
        } ) );
        CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
        CHECK( server.connectionCount() == 2 );
    }
}

TEST_CASE( "ADB smart-socket retires completed sockets without child accumulation",
           "[livecapture][adb][network][client][ownership]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int, int, const QByteArray& ) {
        FakeAdbServer::sendThenClose( socket, QByteArrayLiteral( "OKAY0000" ) );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    for ( AdbSmartSocketClient::OperationId operationId = 8301; operationId < 8311;
          ++operationId ) {
        client.requestHostService( 713, operationId, HostService::ServerFeatures );
        REQUIRE( pumpEventsUntil( [ &probe, operationId ] {
            return probe.count( ClientCallback::Kind::HostReply )
                   == static_cast<int>( operationId - 8300 );
        } ) );
        processDeferredDeletes();
        CHECK( socketFactory.liveSocketCount() == 0 );
        CHECK( client.findChildren<QTcpSocket*>().empty() );
    }
}

TEST_CASE( "ADB smart-socket write deadline covers accepted bytes until socket drain",
           "[livecapture][adb][network][client][deadline][write]" )
{
    FakeAdbServer server;
    InspectableSocketFactory socketFactory( InspectableTcpSocket::WriteMode::AcceptedPending );
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 714, 8401, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil(
        [ &deadlines ] { return deadlines.hasActive( AdbSmartSocketDeadlineKind::Write ); } ) );
    CHECK_FALSE( deadlines.hasActive( AdbSmartSocketDeadlineKind::Read ) );
    deadlines.fire( AdbSmartSocketDeadlineKind::Write );

    REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
    CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
           == AdbSmartSocketErrorCode::WriteTimeout );
}

TEST_CASE( "ADB smart-socket response after full write acceptance advances despite pending drain",
           "[livecapture][adb][network][client][write][reentrant]" )
{
    FakeAdbServer server;
    InspectableSocketFactory socketFactory( InspectableTcpSocket::WriteMode::AcceptedPending );
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 715, 8501, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
        return server.connectionCount() == 1
               && deadlines.hasActive( AdbSmartSocketDeadlineKind::Write );
    } ) );
    FakeAdbServer::sendThenClose(
        *server.socketAt( 0 ),
        QByteArrayLiteral( "OKAY" ) + hostReplyFrame( QByteArrayLiteral( "shell_v2" ) ) );

    REQUIRE( pumpEventsUntil(
        [ &probe ] { return probe.count( ClientCallback::Kind::HostReply ) == 1; } ) );
    CHECK( probe.count( ClientCallback::Kind::Error ) == 0 );
}

TEST_CASE( "ADB smart-socket rejects unsolicited input before request acceptance",
           "[livecapture][adb][network][client][write][bounds]" )
{
    FakeAdbServer server;
    InspectableSocketFactory socketFactory( InspectableTcpSocket::WriteMode::Stalled );
    ManualDeadlineScheduler deadlines;
    auto config = clientConfig( server.port() );
    config.maxReadChunkBytes = 16;
    config.maxHostReplyBytes = 8;
    AdbSmartSocketClient client( config, socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 716, 8601, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil( [ &server, &deadlines ] {
        return server.connectionCount() == 1
               && deadlines.hasActive( AdbSmartSocketDeadlineKind::Write );
    } ) );
    FakeAdbServer::send( *server.socketAt( 0 ), QByteArray( 4096, 'X' ) );

    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
    CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
           == AdbSmartSocketErrorCode::Protocol );
}

TEST_CASE( "ADB smart-socket rejects transport status trailing bytes",
           "[livecapture][adb][network][client][shell-v2][protocol]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int, int requestIndex, const QByteArray& ) {
        REQUIRE( requestIndex == 0 );
        FakeAdbServer::send( socket, QByteArrayLiteral( "OKAYOKAY" ) );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.startShellService( 717, 8701, TransportSelection{ TransportKind::Any, {} },
                              std::string{ "shell,v2,raw:true" } );
    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
    CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
           == AdbSmartSocketErrorCode::Protocol );
    CHECK( server.requestCount() == 1 );
}

TEST_CASE( "ADB smart-socket rejects invalid configuration and non-v2 shell services",
           "[livecapture][adb][network][client][validation]" )
{
    SECTION( "zero shell frame cap" )
    {
        InspectableSocketFactory socketFactory;
        ManualDeadlineScheduler deadlines;
        auto config = clientConfig( 5037 );
        config.maxShellFrameBytes = 0;
        AdbSmartSocketClient client( config, socketFactory, deadlines );
        ClientProbe probe( client );
        client.requestHostService( 718, 8801, HostService::ServerFeatures );
        REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
        CHECK( socketFactory.socketCount() == 0 );
    }

    SECTION( "invalid selected-device feature inputs are rejected before connecting" )
    {
        struct Scenario {
            const char* name;
            TransportSelection transport;
            TransportHostService service;
            AdbSmartSocketClient::OperationId operationId;
        };
        const std::vector<Scenario> scenarios{
            { "invalid service", { TransportKind::Serial, "emulator-5554" },
              static_cast<TransportHostService>( 0xffu ), 8802 },
            { "invalid transport kind",
              { static_cast<TransportKind>( 0xffu ), "emulator-5554" },
              TransportHostService::Features, 8803 },
            { "missing serial", { TransportKind::Serial, {} },
              TransportHostService::Features, 8804 },
            { "NUL serial",
              { TransportKind::Serial, std::string( "serial\0hidden", 13u ) },
              TransportHostService::Features, 8805 },
        };

        for ( const auto& scenario : scenarios ) {
            DYNAMIC_SECTION( scenario.name )
            {
                InspectableSocketFactory socketFactory;
                ManualDeadlineScheduler deadlines;
                AdbSmartSocketClient client( clientConfig( 5037 ), socketFactory, deadlines );
                ClientProbe probe( client );

                client.requestTransportHostService( 718, scenario.operationId,
                                                            scenario.transport, scenario.service );

                REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
                CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
                       == AdbSmartSocketErrorCode::Protocol );
                CHECK( probe.allCallbacksMatch( 718, scenario.operationId ) );
                CHECK( socketFactory.socketCount() == 0 );
            }
        }
    }

    for ( const auto& service : std::vector<std::string>{
              {}, "shell:echo bad", "host:version", std::string{ "shell,v2,raw:a\0b", 16 } } ) {
        DYNAMIC_SECTION( "service size " << service.size() )
        {
            InspectableSocketFactory socketFactory;
            ManualDeadlineScheduler deadlines;
            AdbSmartSocketClient client( clientConfig( 5037 ), socketFactory, deadlines );
            ClientProbe probe( client );
            client.startShellService( 718, 8802, TransportSelection{ TransportKind::Any, {} },
                                      service );
            REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
            CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
                   == AdbSmartSocketErrorCode::Protocol );
            CHECK( socketFactory.socketCount() == 0 );
        }
    }
}

TEST_CASE( "ADB smart-socket queued callbacks preserve correlation and custom error types",
           "[livecapture][adb][network][client][queued]" )
{
    FakeAdbServer server( []( QTcpSocket& socket, int, int, const QByteArray& ) {
        FakeAdbServer::send( socket, QByteArrayLiteral( "FAIL0004nope" ) );
    } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
    QObject receiver;
    int queuedErrors = 0;
    QObject::connect(
        &client, &AdbSmartSocketClient::errorOccurred, &receiver,
        [ &queuedErrors ]( Generation generation, AdbSmartSocketClient::OperationId operationId,
                           AdbSmartSocketErrorCode code, const QString& diagnostic ) {
            CHECK( generation == 719 );
            CHECK( operationId == 8901 );
            CHECK( code == AdbSmartSocketErrorCode::RemoteFailure );
            CHECK( diagnostic.contains( QStringLiteral( "nope" ) ) );
            ++queuedErrors;
        },
        Qt::QueuedConnection );

    client.requestHostService( 719, 8901, HostService::ServerFeatures );
    REQUIRE( pumpEventsUntil( [ &queuedErrors ] { return queuedErrors == 1; } ) );
}

TEST_CASE( "ADB smart-socket rejects extra or partial trailing one-shot host frames",
           "[livecapture][adb][network][client][host][protocol]" )
{
    const std::vector<QByteArray> trailing{
        hostReplyFrame( QByteArrayLiteral( "one" ) ) + hostReplyFrame( QByteArrayLiteral( "two" ) ),
        hostReplyFrame( QByteArrayLiteral( "one" ) ) + QByteArrayLiteral( "00" ),
    };
    for ( const auto& response : trailing ) {
        DYNAMIC_SECTION( "trailing bytes " << response.size() )
        {
            FakeAdbServer server( [ response ]( QTcpSocket& socket, int, int, const QByteArray& ) {
                FakeAdbServer::send( socket, QByteArrayLiteral( "OKAY" ) + response );
            } );
            InspectableSocketFactory socketFactory;
            ManualDeadlineScheduler deadlines;
            AdbSmartSocketClient client( clientConfig( server.port() ), socketFactory, deadlines );
            ClientProbe probe( client );
            client.requestHostService( 720, 9001, HostService::ServerFeatures );

            REQUIRE( pumpEventsUntil(
                [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
            CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
                   == AdbSmartSocketErrorCode::Protocol );
            CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
        }
    }
}

TEST_CASE( "ADB smart-socket rejects a trailing one-shot host frame split at the read boundary",
           "[livecapture][adb][network][client][host][protocol]" )
{
    const auto firstReply = hostReplyFrame( QByteArrayLiteral( "one" ) );
    const auto trailingReply = hostReplyFrame( QByteArrayLiteral( "two" ) );
    FakeAdbServer server(
        [ firstReply, trailingReply ]( QTcpSocket& socket, int, int, const QByteArray& ) {
            FakeAdbServer::sendThenClose( socket, QByteArrayLiteral( "OKAY" ) + firstReply
                                                     + trailingReply );
        } );
    InspectableSocketFactory socketFactory;
    ManualDeadlineScheduler deadlines;
    auto config = clientConfig( server.port() );
    config.maxReadChunkBytes = QByteArrayLiteral( "OKAY" ).size() + firstReply.size();
    AdbSmartSocketClient client( config, socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 721, 9101, HostService::ServerFeatures );

    REQUIRE(
        pumpEventsUntil( [ &probe ] { return probe.count( ClientCallback::Kind::Error ) == 1; } ) );
    CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
           == AdbSmartSocketErrorCode::Protocol );
    CHECK( probe.count( ClientCallback::Kind::HostReply ) == 0 );
}

TEST_CASE( "ADB smart-socket survives synchronous connect deadline expiry",
           "[livecapture][adb][network][client][deadline][reentrant]" )
{
    InspectableSocketFactory socketFactory;
    ImmediateDeadlineScheduler deadlines( AdbSmartSocketDeadlineKind::Connect );
    AdbSmartSocketClient client( clientConfig( unusedLoopbackPort() ), socketFactory, deadlines );
    ClientProbe probe( client );

    client.requestHostService( 721, 9101, HostService::ServerFeatures );

    REQUIRE( probe.count( ClientCallback::Kind::Error ) == 1 );
    CHECK( probe.first( ClientCallback::Kind::Error ).errorCode
           == AdbSmartSocketErrorCode::ConnectTimeout );
    CHECK( deadlines.wasCancelled( deadlines.immediateToken() ) );
}
