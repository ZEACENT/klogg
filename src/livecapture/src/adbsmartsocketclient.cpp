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

#include "adbsmartsocketclient.h"

#include <QAbstractSocket>
#include <QHash>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <utility>

namespace klogg::livecapture::adb {

qint64 AdbSmartSocketFactory::readSocket( QTcpSocket& socket, char* data, qint64 maxSize )
{
    return socket.read( data, maxSize );
}

qint64 AdbSmartSocketFactory::writeSocket( QTcpSocket& socket, const char* data, qint64 maxSize )
{
    return socket.write( data, maxSize );
}

namespace {

class DefaultSocketFactory final : public AdbSmartSocketFactory {
public:
    QTcpSocket* createSocket( QObject* parent ) override
    {
        // Ownership is transferred to Qt's parent-child object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new QTcpSocket( parent );
    }
};

class DefaultDeadlineScheduler final : public AdbSmartSocketDeadlineScheduler {
public:
    ~DefaultDeadlineScheduler() override
    {
        for ( auto* const timer : timers_ ) {
            timer->stop();
        }
    }

    DeadlineToken armDeadline( AdbSmartSocketDeadlineKind, int timeoutMs, QObject* context,
                               std::function<void()> callback ) override
    {
        const auto token = ++nextToken_;
        // Ownership is transferred to the deadline context's Qt object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto* const timer = new QTimer( context );
        timer->setSingleShot( true );
        timer->setTimerType( Qt::PreciseTimer );
        timers_.insert( token, timer );
        QObject::connect( timer, &QTimer::timeout, context,
                          [ this, token, callback = std::move( callback ) ]() mutable {
                              auto* const expired = timers_.take( token );
                              if ( expired != nullptr ) {
                                  expired->deleteLater();
                              }
                              callback();
                          } );
        timer->start( std::max( timeoutMs, 0 ) );
        return token;
    }

    void cancelDeadline( DeadlineToken token ) override
    {
        auto* const timer = timers_.take( token );
        if ( timer != nullptr ) {
            timer->stop();
            timer->deleteLater();
        }
    }

private:
    DeadlineToken nextToken_{ 0 };
    QHash<DeadlineToken, QTimer*> timers_;
};

ByteVector bytesFromString( const std::string& bytes )
{
    ByteVector result;
    result.reserve( bytes.size() );
    for ( const auto byte : bytes ) {
        result.push_back( static_cast<std::uint8_t>( static_cast<unsigned char>( byte ) ) );
    }
    return result;
}

ByteVector bytesFromByteArray( const QByteArray& bytes )
{
    ByteVector result;
    const auto size = static_cast<std::size_t>( bytes.size() );
    result.reserve( size );
    for ( std::size_t index = 0; index < size; ++index ) {
        result.push_back( static_cast<std::uint8_t>(
            static_cast<unsigned char>( bytes.at( static_cast<int>( index ) ) ) ) );
    }
    return result;
}

QByteArray byteArrayFromBytes( const ByteVector& bytes )
{
    QByteArray result;
    if ( bytes.empty() ) {
        return result;
    }

    if ( bytes.size() > static_cast<std::size_t>( std::numeric_limits<int>::max() ) ) {
        return result;
    }
    result.reserve( static_cast<int>( bytes.size() ) );
    for ( const auto byte : bytes ) {
        result.append( static_cast<char>( byte ) );
    }
    return result;
}

QString protocolDiagnostic( const ProtocolError& error )
{
    return QString::fromStdString( error.message );
}

void registerSignalMetaTypes()
{
    qRegisterMetaType<Generation>( "klogg::livecapture::Generation" );
    qRegisterMetaType<AdbSmartSocketClient::OperationId>( "AdbSmartSocketClient::OperationId" );
    qRegisterMetaType<AdbSmartSocketClient::OperationId>(
        "klogg::livecapture::adb::AdbSmartSocketClient::OperationId" );
    qRegisterMetaType<AdbSmartSocketErrorCode>( "AdbSmartSocketErrorCode" );
    qRegisterMetaType<AdbSmartSocketErrorCode>(
        "klogg::livecapture::adb::AdbSmartSocketErrorCode" );
}

} // namespace

class AdbSmartSocketClient::Impl final {
public:
    Impl( AdbSmartSocketClient& client, AdbSmartSocketClientConfig config )
        : client_( client )
        , config_( std::move( config ) )
        , ownedSocketFactory_( std::make_unique<DefaultSocketFactory>() )
        , ownedDeadlineScheduler_( std::make_unique<DefaultDeadlineScheduler>() )
        , socketFactory_( ownedSocketFactory_.get() )
        , deadlineScheduler_( ownedDeadlineScheduler_.get() )
        , statusDecoder_( config_.maxHostReplyBytes )
        , hostReplyDecoder_( config_.maxHostReplyBytes )
        , shellDecoder_( config_.maxShellFrameBytes )
    {
    }

    Impl( AdbSmartSocketClient& client, AdbSmartSocketClientConfig config,
          AdbSmartSocketFactory& socketFactory, AdbSmartSocketDeadlineScheduler& deadlineScheduler )
        : client_( client )
        , config_( std::move( config ) )
        , socketFactory_( &socketFactory )
        , deadlineScheduler_( &deadlineScheduler )
        , statusDecoder_( config_.maxHostReplyBytes )
        , hostReplyDecoder_( config_.maxHostReplyBytes )
        , shellDecoder_( config_.maxShellFrameBytes )
    {
    }

    ~Impl()
    {
        invalidateOperation();
    }

    void requestHostService( Generation generation, OperationId operationId, HostService service )
    {
        const auto builtService = buildHostService( service );
        if ( builtService.error.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              protocolDiagnostic( *builtService.error ) );
            return;
        }
        if ( !builtService.value.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              QStringLiteral( "ADB host service builder returned no request." ) );
            return;
        }

        const auto encoded = encodeSmartSocketRequest( bytesFromString( *builtService.value ) );
        if ( encoded.error.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              protocolDiagnostic( *encoded.error ) );
            return;
        }
        if ( !encoded.value.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              QStringLiteral( "ADB request encoder returned no request." ) );
            return;
        }

        const auto kind = service == HostService::TrackDevicesLong ? OperationKind::StreamingHost
                                                                   : OperationKind::OneShotHost;
        if ( !beginOperation( generation, operationId, kind ) ) {
            return;
        }
        firstRequest_ = byteArrayFromBytes( *encoded.value );
        connectSocket();
    }

    void startShellService( Generation generation, OperationId operationId,
                            const TransportSelection& transport, const std::string& service )
    {
        if ( service.rfind( "shell,v2,", 0u ) != 0u || service.find( '\0' ) != std::string::npos ) {
            emitRequestError(
                generation, operationId, AdbSmartSocketErrorCode::Protocol,
                QStringLiteral(
                    "ADB shell operations require a shell-v2 service without NUL bytes." ) );
            return;
        }

        const auto transportService = buildTransportService( transport );
        if ( transportService.error.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              protocolDiagnostic( *transportService.error ) );
            return;
        }
        if ( !transportService.value.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              QStringLiteral( "ADB transport builder returned no request." ) );
            return;
        }

        const auto encodedTransport
            = encodeSmartSocketRequest( bytesFromString( *transportService.value ) );
        const auto encodedShell = encodeSmartSocketRequest( bytesFromString( service ) );
        if ( encodedTransport.error.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              protocolDiagnostic( *encodedTransport.error ) );
            return;
        }
        if ( encodedShell.error.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              protocolDiagnostic( *encodedShell.error ) );
            return;
        }
        if ( !encodedTransport.value.has_value() || !encodedShell.value.has_value() ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              QStringLiteral( "ADB request encoder returned no request." ) );
            return;
        }

        if ( !beginOperation( generation, operationId, OperationKind::Shell ) ) {
            return;
        }
        firstRequest_ = byteArrayFromBytes( *encodedTransport.value );
        shellRequest_ = byteArrayFromBytes( *encodedShell.value );
        connectSocket();
    }

    void cancelGeneration( Generation generation )
    {
        if ( phase_ == Phase::Idle || generation_ != generation ) {
            return;
        }
        invalidateOperation();
    }

private:
    enum class OperationKind : std::uint8_t { OneShotHost, StreamingHost, Shell };
    enum class Phase : std::uint8_t {
        Idle,
        Connecting,
        Writing,
        HostStatus,
        HostReply,
        TransportStatus,
        ShellStatus,
        ShellFrames
    };

    bool beginOperation( Generation generation, OperationId operationId, OperationKind kind )
    {
        if ( phase_ != Phase::Idle ) {
            emitRequestError(
                generation, operationId, AdbSmartSocketErrorCode::Protocol,
                QStringLiteral( "An ADB smart-socket operation is already active." ) );
            return false;
        }
        if ( config_.serverPort == 0u || config_.serverAddress.isNull()
             || !config_.serverAddress.isLoopback() || config_.maxReadChunkBytes <= 0
             || config_.maxWriteChunkBytes <= 0 || config_.maxShellFrameBytes == 0u
             || config_.maxShellFrameBytes
                    > static_cast<std::size_t>( std::numeric_limits<int>::max() )
             || config_.connectTimeoutMs < 0 || config_.writeTimeoutMs < 0
             || config_.readTimeoutMs < 0 ) {
            emitRequestError( generation, operationId, AdbSmartSocketErrorCode::Protocol,
                              QStringLiteral( "ADB smart-socket configuration requires a valid "
                                              "loopback endpoint and positive I/O bounds." ) );
            return false;
        }

        ++operationSerial_;
        generation_ = generation;
        operationId_ = operationId;
        operationKind_ = kind;
        phase_ = Phase::Connecting;
        nextReadPhase_ = Phase::Idle;
        firstRequest_.clear();
        shellRequest_.clear();
        writeBuffer_.clear();
        writeOffset_ = 0;
        statusDecoder_ = SmartSocketStatusDecoder( config_.maxHostReplyBytes );
        hostReplyDecoder_ = LengthPrefixedHostReplyDecoder( config_.maxHostReplyBytes );
        shellDecoder_ = ShellV2FrameDecoder( config_.maxShellFrameBytes );
        return true;
    }

    void connectSocket()
    {
        socket_ = socketFactory_->createSocket( &client_ );
        if ( socket_ == nullptr ) {
            fail( AdbSmartSocketErrorCode::Connection,
                  QStringLiteral( "Unable to create an ADB smart-socket connection." ) );
            return;
        }

        const auto serial = operationSerial_;
        auto* const operationSocket = socket_;
        QObject::connect( socket_, &QTcpSocket::connected, &client_,
                          [ this, serial, operationSocket ] {
                              if ( isCurrentSocket( serial, operationSocket ) ) {
                                  connected();
                              }
                          } );
        QObject::connect( socket_, &QTcpSocket::readyRead, &client_,
                          [ this, serial, operationSocket ] {
                              if ( isCurrentSocket( serial, operationSocket ) ) {
                                  consumeAvailableBytes();
                              }
                          } );
        QObject::connect( socket_, &QTcpSocket::bytesWritten, &client_,
                          [ this, serial, operationSocket ]( qint64 ) {
                              if ( isCurrentSocket( serial, operationSocket )
                                   && phase_ == Phase::Writing ) {
                                  writeNextChunk();
                              }
                          } );
        QObject::connect( socket_, &QTcpSocket::stateChanged, &client_,
                          [ this, serial, operationSocket ]( QAbstractSocket::SocketState state ) {
                              if ( state == QAbstractSocket::UnconnectedState
                                   && isCurrentSocket( serial, operationSocket ) ) {
                                  socketClosed();
                              }
                          } );

        armDeadline( AdbSmartSocketDeadlineKind::Connect, config_.connectTimeoutMs,
                     AdbSmartSocketErrorCode::ConnectTimeout,
                     QStringLiteral( "ADB smart-socket connect timed out." ) );
        if ( isCurrentSocket( serial, operationSocket ) && phase_ == Phase::Connecting ) {
            operationSocket->connectToHost( config_.serverAddress, config_.serverPort );
        }
    }

    bool isCurrentSocket( std::uint64_t serial, const QTcpSocket* socket ) const
    {
        return phase_ != Phase::Idle && serial == operationSerial_ && socket_ == socket;
    }

    void connected()
    {
        if ( phase_ != Phase::Connecting ) {
            return;
        }
        cancelDeadline();
        const auto serial = operationSerial_;
        Q_EMIT client_.operationConnected( generation_, operationId_ );
        if ( serial != operationSerial_ || phase_ != Phase::Connecting ) {
            return;
        }

        const auto readPhase
            = operationKind_ == OperationKind::Shell ? Phase::TransportStatus : Phase::HostStatus;
        startWrite( firstRequest_, readPhase );
    }

    void startWrite( QByteArray bytes, Phase nextReadPhase )
    {
        writeBuffer_ = std::move( bytes );
        writeOffset_ = 0;
        nextReadPhase_ = nextReadPhase;
        phase_ = Phase::Writing;
        armDeadline( AdbSmartSocketDeadlineKind::Write, config_.writeTimeoutMs,
                     AdbSmartSocketErrorCode::WriteTimeout,
                     QStringLiteral( "ADB smart-socket write timed out." ) );
        writeNextChunk();
    }

    void writeNextChunk()
    {
        if ( phase_ != Phase::Writing || socket_ == nullptr ) {
            return;
        }

        const auto totalSize = static_cast<qint64>( writeBuffer_.size() );
        if ( writeOffset_ >= totalSize ) {
            if ( socket_->bytesToWrite() == 0 ) {
                enterReadPhase();
            }
            return;
        }

        const auto remaining = totalSize - writeOffset_;
        const auto requested = std::min( remaining, config_.maxWriteChunkBytes );
        const auto offset = static_cast<std::ptrdiff_t>( writeOffset_ );
        const auto written
            = socketFactory_->writeSocket( *socket_, writeBuffer_.constData() + offset, requested );
        if ( written < 0 ) {
            fail( AdbSmartSocketErrorCode::Connection,
                  QStringLiteral( "ADB smart-socket write failed: %1" )
                      .arg( socket_->errorString() ) );
            return;
        }
        if ( written == 0 ) {
            return;
        }
        if ( written > requested ) {
            fail( AdbSmartSocketErrorCode::Protocol,
                  QStringLiteral( "ADB smart-socket accepted more bytes than requested." ) );
            return;
        }

        writeOffset_ += written;
        if ( writeOffset_ >= totalSize && socket_->bytesToWrite() == 0 ) {
            enterReadPhase();
        }
    }

    void enterReadPhase()
    {
        cancelDeadline();
        writeBuffer_.clear();
        writeOffset_ = 0;
        phase_ = nextReadPhase_;
        nextReadPhase_ = Phase::Idle;
        statusDecoder_.reset();
        armDeadline( AdbSmartSocketDeadlineKind::Read, config_.readTimeoutMs,
                     AdbSmartSocketErrorCode::ReadTimeout,
                     QStringLiteral( "ADB smart-socket read timed out." ) );
    }

    void consumeAvailableBytes()
    {
        if ( socket_ == nullptr ) {
            return;
        }

        if ( phase_ == Phase::Writing ) {
            if ( writeOffset_ < static_cast<qint64>( writeBuffer_.size() ) ) {
                fail( AdbSmartSocketErrorCode::Protocol,
                      QStringLiteral( "ADB smart-socket received data before the request was "
                                      "accepted for transmission." ) );
                return;
            }
            enterReadPhase();
            if ( socket_ == nullptr || phase_ == Phase::Idle ) {
                return;
            }
        }

        while ( phase_ != Phase::Idle && socket_->bytesAvailable() > 0 ) {
            const auto readSize
                = std::min( { socket_->bytesAvailable(), config_.maxReadChunkBytes,
                              static_cast<qint64>( std::numeric_limits<int>::max() ) } );
            QByteArray chunk;
            chunk.resize( static_cast<int>( readSize ) );
            const auto bytesRead = socketFactory_->readSocket( *socket_, chunk.data(), readSize );
            if ( bytesRead <= 0 ) {
                break;
            }
            chunk.resize( static_cast<int>( bytesRead ) );
            processInput( bytesFromByteArray( chunk ) );
        }
    }

    void processInput( ByteVector bytes )
    {
        while ( phase_ != Phase::Idle && !bytes.empty() ) {
            switch ( phase_ ) {
            case Phase::HostStatus:
            case Phase::TransportStatus:
            case Phase::ShellStatus: {
                auto result = statusDecoder_.feed( bytes );
                if ( result.error.has_value() ) {
                    fail( AdbSmartSocketErrorCode::Protocol, protocolDiagnostic( *result.error ) );
                    return;
                }
                if ( result.frames.empty() ) {
                    return;
                }

                const auto status = std::move( result.frames.front() );
                if ( status.kind == SmartSocketStatusKind::Fail ) {
                    const auto detail = QString::fromUtf8( byteArrayFromBytes( status.message ) );
                    fail( AdbSmartSocketErrorCode::RemoteFailure,
                          QStringLiteral( "ADB service failed: %1" ).arg( detail ) );
                    return;
                }

                bytes = std::move( result.unconsumedBytes );
                if ( phase_ == Phase::HostStatus ) {
                    statusDecoder_.reset();
                    phase_ = Phase::HostReply;
                    armDeadline( AdbSmartSocketDeadlineKind::Read, config_.readTimeoutMs,
                                 AdbSmartSocketErrorCode::ReadTimeout,
                                 QStringLiteral( "ADB smart-socket read timed out." ) );
                    continue;
                }
                if ( phase_ == Phase::TransportStatus ) {
                    if ( !bytes.empty() ) {
                        fail( AdbSmartSocketErrorCode::Protocol,
                              QStringLiteral( "ADB transport status contained an unsolicited "
                                              "trailing response." ) );
                        return;
                    }
                    statusDecoder_.reset();
                    startWrite( shellRequest_, Phase::ShellStatus );
                    return;
                }

                statusDecoder_.reset();
                cancelDeadline();
                phase_ = Phase::ShellFrames;
                {
                    const auto serial = operationSerial_;
                    Q_EMIT client_.shellServiceStarted( generation_, operationId_ );
                    if ( serial != operationSerial_ || phase_ != Phase::ShellFrames ) {
                        return;
                    }
                }
                continue;
            }
            case Phase::HostReply: {
                auto result = hostReplyDecoder_.feed( bytes );
                if ( result.error.has_value() ) {
                    fail( AdbSmartSocketErrorCode::Protocol, protocolDiagnostic( *result.error ) );
                    return;
                }
                if ( result.frames.empty() ) {
                    return;
                }

                const auto generation = generation_;
                const auto operationId = operationId_;
                if ( operationKind_ == OperationKind::StreamingHost ) {
                    cancelDeadline();
                    const auto serial = operationSerial_;
                    for ( const auto& frame : result.frames ) {
                        Q_EMIT client_.hostReplyReceived( generation, operationId,
                                                          byteArrayFromBytes( frame ) );
                        if ( serial != operationSerial_ || phase_ != Phase::HostReply ) {
                            return;
                        }
                    }
                    return;
                }

                if ( result.frames.size() != 1u || result.bufferedByteCount != 0u ) {
                    fail( AdbSmartSocketErrorCode::Protocol,
                          QStringLiteral( "ADB one-shot host service returned trailing data." ) );
                    return;
                }

                const auto reply = byteArrayFromBytes( result.frames.front() );
                finishSuccess();
                Q_EMIT client_.hostReplyReceived( generation, operationId, reply );
                return;
            }
            case Phase::ShellFrames: {
                auto result = shellDecoder_.feed( bytes );
                const auto serial = operationSerial_;
                for ( std::size_t index = 0; index < result.frames.size(); ++index ) {
                    const auto& frame = result.frames.at( index );
                    const auto payload = byteArrayFromBytes( frame.payload );
                    switch ( frame.channel ) {
                    case ShellV2Channel::Stdout:
                        Q_EMIT client_.shellStdoutReceived( generation_, operationId_, payload );
                        break;
                    case ShellV2Channel::Stderr:
                        Q_EMIT client_.shellStderrReceived( generation_, operationId_, payload );
                        break;
                    case ShellV2Channel::Exit: {
                        if ( result.error.has_value() || index + 1u != result.frames.size()
                             || result.bufferedByteCount != 0u ) {
                            fail(
                                AdbSmartSocketErrorCode::Protocol,
                                QStringLiteral( "ADB shell-v2 exit frame was followed by trailing "
                                                "protocol data." ) );
                            return;
                        }

                        const auto generation = generation_;
                        const auto operationId = operationId_;
                        const auto exitCode = frame.payload.front();
                        finishSuccess();
                        Q_EMIT client_.shellExited( generation, operationId, exitCode );
                        return;
                    }
                    case ShellV2Channel::Stdin:
                    case ShellV2Channel::CloseStdin:
                    case ShellV2Channel::WindowSizeChange:
                        fail(
                            AdbSmartSocketErrorCode::Protocol,
                            QStringLiteral( "ADB shell-v2 returned an invalid server channel." ) );
                        return;
                    }
                    if ( serial != operationSerial_ || phase_ != Phase::ShellFrames ) {
                        return;
                    }
                }

                if ( result.error.has_value() ) {
                    fail( AdbSmartSocketErrorCode::Protocol, protocolDiagnostic( *result.error ) );
                }
                return;
            }
            case Phase::Idle:
                return;
            case Phase::Connecting:
            case Phase::Writing:
                fail( AdbSmartSocketErrorCode::Protocol,
                      QStringLiteral( "ADB smart-socket received data outside a read phase." ) );
                return;
            }
        }
    }

    void socketClosed()
    {
        if ( phase_ == Phase::Idle ) {
            return;
        }

        consumeAvailableBytes();
        if ( phase_ == Phase::Idle ) {
            return;
        }

        if ( phase_ == Phase::Connecting ) {
            fail( AdbSmartSocketErrorCode::Connection,
                  QStringLiteral( "Unable to connect to the ADB smart-socket endpoint: %1" )
                      .arg( socket_ != nullptr ? socket_->errorString() : QString{} ) );
            return;
        }

        const auto socketError
            = socket_ != nullptr ? socket_->error() : QAbstractSocket::UnknownSocketError;
        if ( socketError != QAbstractSocket::UnknownSocketError
             && socketError != QAbstractSocket::RemoteHostClosedError ) {
            fail( AdbSmartSocketErrorCode::Connection,
                  QStringLiteral( "ADB smart-socket connection failed: %1" )
                      .arg( socket_ != nullptr ? socket_->errorString() : QString{} ) );
            return;
        }

        fail( AdbSmartSocketErrorCode::UnexpectedEof,
              QStringLiteral(
                  "ADB smart-socket closed before the current protocol frame completed." ) );
    }

    void armDeadline( AdbSmartSocketDeadlineKind kind, int timeoutMs,
                      AdbSmartSocketErrorCode errorCode, QString diagnostic )
    {
        cancelDeadline();
        const auto serial = operationSerial_;
        const auto deadlineSerial = ++deadlineSerial_;
        const auto token = deadlineScheduler_->armDeadline(
            kind, timeoutMs, &client_,
            [ this, serial, deadlineSerial, errorCode, diagnostic = std::move( diagnostic ) ] {
                if ( phase_ != Phase::Idle && serial == operationSerial_
                     && deadlineSerial == deadlineSerial_ ) {
                    deadlineToken_ = 0;
                    fail( errorCode, diagnostic );
                }
            } );
        if ( phase_ != Phase::Idle && serial == operationSerial_
             && deadlineSerial == deadlineSerial_ ) {
            deadlineToken_ = token;
        }
        else if ( token != 0u ) {
            deadlineScheduler_->cancelDeadline( token );
        }
    }

    void cancelDeadline()
    {
        ++deadlineSerial_;
        if ( deadlineToken_ != 0u ) {
            deadlineScheduler_->cancelDeadline( deadlineToken_ );
            deadlineToken_ = 0;
        }
    }

    void finishSuccess()
    {
        retireOperation( false );
    }

    void fail( AdbSmartSocketErrorCode code, const QString& diagnostic )
    {
        if ( phase_ == Phase::Idle ) {
            return;
        }
        const auto generation = generation_;
        const auto operationId = operationId_;
        invalidateOperation();
        Q_EMIT client_.errorOccurred( generation, operationId, code, diagnostic );
    }

    void emitRequestError( Generation generation, OperationId operationId,
                           AdbSmartSocketErrorCode code, const QString& diagnostic )
    {
        Q_EMIT client_.errorOccurred( generation, operationId, code, diagnostic );
    }

    void invalidateOperation()
    {
        retireOperation( true );
    }

    void retireOperation( bool abortSocket )
    {
        ++operationSerial_;
        phase_ = Phase::Idle;
        cancelDeadline();
        writeBuffer_.clear();
        firstRequest_.clear();
        shellRequest_.clear();
        statusDecoder_.reset();
        hostReplyDecoder_.reset();
        shellDecoder_.reset();

        if ( socket_ != nullptr ) {
            auto* const retiredSocket = socket_;
            socket_ = nullptr;
            QObject::disconnect( retiredSocket, nullptr, &client_, nullptr );
            if ( abortSocket ) {
                retiredSocket->abort();
            }
            else {
                retiredSocket->disconnectFromHost();
            }
            retiredSocket->deleteLater();
        }
    }

private:
    AdbSmartSocketClient& client_;
    AdbSmartSocketClientConfig config_;
    std::unique_ptr<DefaultSocketFactory> ownedSocketFactory_;
    std::unique_ptr<DefaultDeadlineScheduler> ownedDeadlineScheduler_;
    AdbSmartSocketFactory* socketFactory_{ nullptr };
    AdbSmartSocketDeadlineScheduler* deadlineScheduler_{ nullptr };

    QTcpSocket* socket_{ nullptr };
    Generation generation_{ 0 };
    OperationId operationId_{ 0 };
    OperationKind operationKind_{ OperationKind::OneShotHost };
    Phase phase_{ Phase::Idle };
    Phase nextReadPhase_{ Phase::Idle };
    std::uint64_t operationSerial_{ 0 };
    std::uint64_t deadlineSerial_{ 0 };
    DeadlineToken deadlineToken_{ 0 };

    QByteArray firstRequest_;
    QByteArray shellRequest_;
    QByteArray writeBuffer_;
    qint64 writeOffset_{ 0 };

    SmartSocketStatusDecoder statusDecoder_;
    LengthPrefixedHostReplyDecoder hostReplyDecoder_;
    ShellV2FrameDecoder shellDecoder_;
};

AdbSmartSocketClient::AdbSmartSocketClient( AdbSmartSocketClientConfig config, QObject* parent )
    : QObject( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ) ) )
{
    registerSignalMetaTypes();
}

AdbSmartSocketClient::AdbSmartSocketClient( AdbSmartSocketClientConfig config,
                                            AdbSmartSocketFactory& socketFactory,
                                            AdbSmartSocketDeadlineScheduler& deadlineScheduler,
                                            QObject* parent )
    : QObject( parent )
    , impl_(
          std::make_unique<Impl>( *this, std::move( config ), socketFactory, deadlineScheduler ) )
{
    registerSignalMetaTypes();
}

AdbSmartSocketClient::~AdbSmartSocketClient() = default;

void AdbSmartSocketClient::requestHostService( Generation generation, OperationId operationId,
                                               HostService service )
{
    impl_->requestHostService( generation, operationId, service );
}

void AdbSmartSocketClient::startShellService( Generation generation, OperationId operationId,
                                              const TransportSelection& transport,
                                              std::string service )
{
    impl_->startShellService( generation, operationId, transport, service );
}

void AdbSmartSocketClient::cancelGeneration( Generation generation )
{
    impl_->cancelGeneration( generation );
}

} // namespace klogg::livecapture::adb
