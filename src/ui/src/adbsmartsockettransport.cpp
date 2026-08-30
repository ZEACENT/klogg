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

#include "adbsmartsockettransport.h"

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace klogg::livecapture::adb {
namespace {

constexpr std::size_t MaxDiagnosticBytes = std::size_t{ 64u } * 1024u;

std::vector<std::uint8_t> byteVector( const QByteArray& bytes )
{
    std::vector<std::uint8_t> result;
    const auto size = static_cast<std::size_t>( bytes.size() );
    result.reserve( size );
    for ( int index = 0; index < bytes.size(); ++index ) {
        result.push_back(
            static_cast<std::uint8_t>( static_cast<unsigned char>( bytes.at( index ) ) ) );
    }
    return result;
}

QByteArray byteArray( const std::vector<std::uint8_t>& bytes )
{
    if ( bytes.empty() ) {
        return {};
    }
    if ( bytes.size() > static_cast<std::size_t>( std::numeric_limits<int>::max() ) ) {
        return {};
    }

    return { reinterpret_cast<const char*>( bytes.data() ), static_cast<int>( bytes.size() ) };
}

std::string utf8String( const QString& text )
{
    const auto utf8 = text.toUtf8();
    return { utf8.constData(), static_cast<std::size_t>( utf8.size() ) };
}

void appendDiagnostic( QByteArray& diagnostic, const QByteArray& bytes )
{
    if ( bytes.isEmpty() || diagnostic.size() >= static_cast<int>( MaxDiagnosticBytes ) ) {
        return;
    }

    const auto remaining
        = static_cast<int>( MaxDiagnosticBytes ) - static_cast<int>( diagnostic.size() );
    diagnostic.append( bytes.left( remaining ) );
}

QString trimmedDiagnostic( const QByteArray& diagnostic )
{
    return QString::fromUtf8( diagnostic ).trimmed();
}

bool supportsShellV2( const QByteArray& features )
{
    const auto advertised = features.split( ',' );
    return std::any_of( advertised.begin(), advertised.end(), []( const QByteArray& feature ) {
        return feature.trimmed() == QByteArrayLiteral( "shell_v2" );
    } );
}

QString contextualError( const QString& operation, AdbSmartSocketErrorCode code,
                         const QString& diagnostic )
{
    switch ( code ) {
    case AdbSmartSocketErrorCode::UnexpectedEof:
        return QObject::tr( "%1 ended with unexpected EOF: %2" ).arg( operation, diagnostic );
    case AdbSmartSocketErrorCode::ConnectTimeout:
    case AdbSmartSocketErrorCode::WriteTimeout:
    case AdbSmartSocketErrorCode::ReadTimeout:
        return QObject::tr( "%1 timed out: %2" ).arg( operation, diagnostic );
    case AdbSmartSocketErrorCode::Connection:
    case AdbSmartSocketErrorCode::Protocol:
    case AdbSmartSocketErrorCode::RemoteFailure:
        return QObject::tr( "%1 failed: %2" ).arg( operation, diagnostic );
    }

    return QObject::tr( "%1 failed: %2" ).arg( operation, diagnostic );
}

} // namespace

class AdbSmartSocketTransport::Impl final {
public:
    Impl( AdbSmartSocketTransport& transport, AdbSmartSocketTransportConfig config,
          AdbSmartSocketFactory* socketFactory, AdbSmartSocketDeadlineScheduler* deadlineScheduler )
        : transport_( transport )
        , config_( std::move( config ) )
        , socketFactory_( socketFactory )
        , deadlineScheduler_( deadlineScheduler )
        , queue_( config_.queueLimits, 0u, [ this ] { scheduleQueuePump(); } )
    {
    }

    ~Impl()
    {
        queue_.close();
        cancelAllClients();
    }

    void start( Generation generation )
    {
        if ( activeGeneration_ == generation
             && ( state_ == LiveSourceTransport::State::Connecting
                  || state_ == LiveSourceTransport::State::Connected ) ) {
            return;
        }

        if ( activeGeneration_.has_value() ) {
            cancelStreamClients( *activeGeneration_ );
        }

        ++queueEpoch_;
        queuePumpScheduled_ = false;
        queue_.reset( generation );
        activeGeneration_ = generation;
        terminal_ = false;
        lastError_.clear();
        streamStderr_.clear();
        setState( generation, LiveSourceTransport::State::Connecting );

        // stateChanged is synchronous; a controller may stop or replace this run.
        if ( activeGeneration_ != generation || terminal_
             || state_ != LiveSourceTransport::State::Connecting ) {
            return;
        }

        const auto operationId = nextOperationId();
        featureClient_ = createClient();
        auto* const client = featureClient_.data();
        connectFeatureClient( client, generation, operationId );
        client->requestHostService( generation, operationId, HostService::Features );
    }

    void stop( Generation generation )
    {
        if ( activeGeneration_ != generation ) {
            return;
        }

        // Invalidate first so synchronous cancellation callbacks are stale.
        activeGeneration_.reset();
        terminal_ = false;
        ++queueEpoch_;
        queuePumpScheduled_ = false;
        queue_.reset( generation );
        cancelStreamClients( generation );
        setState( generation, LiveSourceTransport::State::Disconnected );
    }

    void clearRemoteAsync( Generation generation, LiveSourceTransport::ClearRequestId requestId )
    {
        const auto operationId = nextOperationId();
        auto* const client = createClient();
        ClearOperation operation;
        operation.client = client;
        operation.generation = generation;
        operation.requestId = requestId;
        clearOperations_.emplace( operationId, std::move( operation ) );
        connectClearClient( client, generation, operationId );
        client->requestHostService( generation, operationId, HostService::Features );
    }

    QString lastError() const
    {
        return lastError_;
    }

    LiveDataStatistics statistics() const
    {
        return queue_.statistics();
    }

private:
    enum class ClearPhase : std::uint8_t { Features, Shell };

    struct ClearOperation {
        QPointer<AdbSmartSocketClient> client;
        Generation generation{ 0 };
        LiveSourceTransport::ClearRequestId requestId{ 0 };
        ClearPhase phase{ ClearPhase::Features };
        QByteArray stderrBytes;
    };

    AdbSmartSocketClient* createClient()
    {
        if ( socketFactory_ != nullptr && deadlineScheduler_ != nullptr ) {
            // Ownership is transferred to the transport's Qt object tree.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
            return new AdbSmartSocketClient( config_.clientConfig, *socketFactory_,
                                             *deadlineScheduler_, &transport_ );
        }

        // Ownership is transferred to the transport's Qt object tree.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new AdbSmartSocketClient( config_.clientConfig, &transport_ );
    }

    AdbSmartSocketClient::OperationId nextOperationId()
    {
        ++nextOperationId_;
        if ( nextOperationId_ == 0u ) {
            ++nextOperationId_;
        }
        return nextOperationId_;
    }

    TransportSelection transportSelection() const
    {
        return TransportSelection{ TransportKind::Serial, utf8String( config_.deviceSerial ) };
    }

    bool isActive( Generation generation ) const
    {
        return activeGeneration_ == generation && !terminal_;
    }

    void connectFeatureClient( AdbSmartSocketClient* client, Generation generation,
                               AdbSmartSocketClient::OperationId operationId )
    {
        QObject::connect(
            client, &AdbSmartSocketClient::hostReplyReceived, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       const QByteArray& features ) {
                if ( callbackGeneration != generation || callbackId != operationId
                     || featureClient_ != client || !isActive( generation ) ) {
                    return;
                }

                retireFeatureClient( generation );
                if ( !supportsShellV2( features ) ) {
                    failStream(
                        generation,
                        QObject::tr( "ADB server does not advertise required shell_v2 support; "
                                     "use a compatible ADB server." ) );
                    return;
                }
                startLogcat( generation );
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::errorOccurred, &transport_,
            [ this, client, generation, operationId ](
                Generation callbackGeneration, AdbSmartSocketClient::OperationId callbackId,
                AdbSmartSocketErrorCode code, const QString& diagnostic ) {
                if ( callbackGeneration != generation || callbackId != operationId
                     || featureClient_ != client || !isActive( generation ) ) {
                    return;
                }
                failStream( generation,
                            contextualError( QObject::tr( "ADB server features negotiation" ), code,
                                             diagnostic ) );
            } );
    }

    void startLogcat( Generation generation )
    {
        const auto service = buildLogcatService( config_.logcatOptions );
        if ( service.error.has_value() ) {
            failStream( generation, QString::fromStdString( service.error->message ) );
            return;
        }
        if ( !service.value.has_value() ) {
            failStream( generation,
                        QObject::tr( "ADB logcat service builder returned no request." ) );
            return;
        }

        const auto operationId = nextOperationId();
        streamClient_ = createClient();
        auto* const client = streamClient_.data();
        connectStreamClient( client, generation, operationId );
        client->startShellService( generation, operationId, transportSelection(), *service.value );
    }

    void connectStreamClient( AdbSmartSocketClient* client, Generation generation,
                              AdbSmartSocketClient::OperationId operationId )
    {
        QObject::connect( client, &AdbSmartSocketClient::shellServiceStarted, &transport_,
                          [ this, client, generation,
                            operationId ]( Generation callbackGeneration,
                                           AdbSmartSocketClient::OperationId callbackId ) {
                              if ( callbackGeneration != generation || callbackId != operationId
                                   || streamClient_ != client || !isActive( generation ) ) {
                                  return;
                              }
                              setState( generation, LiveSourceTransport::State::Connected );
                          } );
        QObject::connect(
            client, &AdbSmartSocketClient::shellStdoutReceived, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       const QByteArray& bytes ) {
                if ( callbackGeneration != generation || callbackId != operationId
                     || streamClient_ != client || !isActive( generation ) || bytes.isEmpty() ) {
                    return;
                }
                enqueueStreamBytes( generation, bytes );
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::shellStderrReceived, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       const QByteArray& bytes ) {
                if ( callbackGeneration == generation && callbackId == operationId
                     && streamClient_ == client && isActive( generation ) ) {
                    appendDiagnostic( streamStderr_, bytes );
                }
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::shellExited, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       std::uint8_t exitCode ) {
                if ( callbackGeneration != generation || callbackId != operationId
                     || streamClient_ != client || !isActive( generation ) ) {
                    return;
                }

                const auto diagnostic = trimmedDiagnostic( streamStderr_ );
                auto error = QObject::tr( "ADB logcat exited with code %1." ).arg( exitCode );
                if ( !diagnostic.isEmpty() ) {
                    error.append( QStringLiteral( " " ) );
                    error.append( diagnostic );
                }
                failStream( generation, error );
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::errorOccurred, &transport_,
            [ this, client, generation, operationId ](
                Generation callbackGeneration, AdbSmartSocketClient::OperationId callbackId,
                AdbSmartSocketErrorCode code, const QString& diagnostic ) {
                if ( callbackGeneration != generation || callbackId != operationId
                     || streamClient_ != client || !isActive( generation ) ) {
                    return;
                }

                auto error
                    = contextualError( QObject::tr( "ADB logcat stream" ), code, diagnostic );
                const auto stderrDiagnostic = trimmedDiagnostic( streamStderr_ );
                if ( !stderrDiagnostic.isEmpty() ) {
                    error.append( QStringLiteral( " " ) );
                    error.append( stderrDiagnostic );
                }
                failStream( generation, error );
            } );
    }

    void connectClearClient( AdbSmartSocketClient* client, Generation generation,
                             AdbSmartSocketClient::OperationId operationId )
    {
        QObject::connect(
            client, &AdbSmartSocketClient::hostReplyReceived, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       const QByteArray& features ) {
                const auto found = clearOperations_.find( operationId );
                if ( found == clearOperations_.end() || found->second.client != client
                     || found->second.phase != ClearPhase::Features
                     || callbackGeneration != generation || callbackId != operationId ) {
                    return;
                }

                if ( !supportsShellV2( features ) ) {
                    completeClear(
                        operationId, false,
                        QObject::tr( "ADB server does not advertise required shell_v2 support; "
                                     "cannot clear logcat with this server." ) );
                    return;
                }

                found->second.phase = ClearPhase::Shell;
                client->startShellService( generation, operationId, transportSelection(),
                                           buildClearLogcatService() );
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::shellStderrReceived, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       const QByteArray& bytes ) {
                auto found = clearOperations_.find( operationId );
                if ( found != clearOperations_.end() && found->second.client == client
                     && callbackGeneration == generation && callbackId == operationId ) {
                    appendDiagnostic( found->second.stderrBytes, bytes );
                }
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::shellExited, &transport_,
            [ this, client, generation, operationId ]( Generation callbackGeneration,
                                                       AdbSmartSocketClient::OperationId callbackId,
                                                       std::uint8_t exitCode ) {
                const auto found = clearOperations_.find( operationId );
                if ( found == clearOperations_.end() || found->second.client != client
                     || callbackGeneration != generation || callbackId != operationId ) {
                    return;
                }

                if ( exitCode == 0u ) {
                    completeClear( operationId, true, {} );
                    return;
                }
                auto error = QObject::tr( "ADB logcat clear exited with code %1." ).arg( exitCode );
                const auto diagnostic = trimmedDiagnostic( found->second.stderrBytes );
                if ( !diagnostic.isEmpty() ) {
                    error.append( QStringLiteral( " " ) );
                    error.append( diagnostic );
                }
                completeClear( operationId, false, std::move( error ) );
            } );
        QObject::connect(
            client, &AdbSmartSocketClient::errorOccurred, &transport_,
            [ this, client, generation, operationId ](
                Generation callbackGeneration, AdbSmartSocketClient::OperationId callbackId,
                AdbSmartSocketErrorCode code, const QString& diagnostic ) {
                const auto found = clearOperations_.find( operationId );
                if ( found == clearOperations_.end() || found->second.client != client
                     || callbackGeneration != generation || callbackId != operationId ) {
                    return;
                }

                const auto operation = found->second.phase == ClearPhase::Features
                                           ? QObject::tr( "ADB logcat clear features negotiation" )
                                           : QObject::tr( "ADB logcat clear" );
                auto error = contextualError( operation, code, diagnostic );
                const auto stderrDiagnostic = trimmedDiagnostic( found->second.stderrBytes );
                if ( !stderrDiagnostic.isEmpty() ) {
                    error.append( QStringLiteral( " " ) );
                    error.append( stderrDiagnostic );
                }
                completeClear( operationId, false, std::move( error ) );
            } );
    }

    void enqueueStreamBytes( Generation generation, const QByteArray& bytes )
    {
        const auto result = queue_.tryEnqueue( LiveDataChunk{ generation, byteVector( bytes ) } );
        switch ( result ) {
        case LiveDataEnqueueResult::Accepted:
        case LiveDataEnqueueResult::StaleGeneration:
        case LiveDataEnqueueResult::Closed:
            return;
        case LiveDataEnqueueResult::Backpressure:
            failStream( generation,
                        QObject::tr( "ADB logcat queue backpressure limit was exceeded." ) );
            return;
        }
    }

    void scheduleQueuePump()
    {
        if ( queuePumpScheduled_ || !activeGeneration_.has_value() ) {
            return;
        }

        queuePumpScheduled_ = true;
        const auto epoch = queueEpoch_;
        const auto generation = *activeGeneration_;
        QTimer::singleShot( 0, &transport_, [ this, epoch, generation ] {
            if ( epoch != queueEpoch_ ) {
                return;
            }
            queuePumpScheduled_ = false;
            pumpQueue( generation );
        } );
    }

    void pumpQueue( Generation generation )
    {
        if ( !isActive( generation ) || state_ != LiveSourceTransport::State::Connected ) {
            return;
        }

        const auto batch = queue_.drain();
        if ( !batch.has_value() || batch->generation != generation ) {
            return;
        }

        const auto bytes = byteArray( batch->bytes );
        if ( bytes.isEmpty() && !batch->bytes.empty() ) {
            failStream( generation,
                        QObject::tr( "ADB logcat queue batch exceeds Qt byte-array limits." ) );
            return;
        }
        if ( !bytes.isEmpty() ) {
            Q_EMIT transport_.bytesReceived( generation, bytes );
        }
    }

    void failStream( Generation generation, QString error )
    {
        if ( !isActive( generation ) ) {
            return;
        }

        // The client can report stdout and a terminal frame/EOF from the same
        // socket read. Preserve wire order across the asynchronous queue boundary:
        // all accepted stdout must be delivered before the terminal state/error.
        pumpQueue( generation );
        if ( !isActive( generation ) ) {
            return;
        }

        terminal_ = true;
        lastError_ = std::move( error );
        const auto terminalError = lastError_;
        retireFeatureClient( generation );
        retireStreamClient( generation );
        setState( generation, LiveSourceTransport::State::Error );

        // stateChanged is synchronous and may start a replacement generation,
        // which clears lastError_. The failed generation still owns exactly one
        // correlated diagnostic, matching ProcessLiveSourceTransport's contract.
        Q_EMIT transport_.errorOccurred( generation, terminalError );
    }

    void completeClear( AdbSmartSocketClient::OperationId operationId, bool succeeded,
                        QString error )
    {
        const auto found = clearOperations_.find( operationId );
        if ( found == clearOperations_.end() ) {
            return;
        }

        const auto generation = found->second.generation;
        const auto requestId = found->second.requestId;
        auto client = found->second.client;
        clearOperations_.erase( found );
        if ( client ) {
            QObject::disconnect( client.data(), nullptr, &transport_, nullptr );
            client->deleteLater();
        }
        Q_EMIT transport_.clearRemoteFinished( generation, requestId, succeeded, error );
    }

    void retireFeatureClient( Generation generation )
    {
        retireClient( featureClient_, generation );
    }

    void retireStreamClient( Generation generation )
    {
        retireClient( streamClient_, generation );
    }

    void retireClient( QPointer<AdbSmartSocketClient>& client, Generation generation )
    {
        if ( !client ) {
            return;
        }

        auto* const retired = client.data();
        client.clear();
        QObject::disconnect( retired, nullptr, &transport_, nullptr );
        retired->cancelGeneration( generation );
        retired->deleteLater();
    }

    void cancelStreamClients( Generation generation )
    {
        retireFeatureClient( generation );
        retireStreamClient( generation );
    }

    void cancelAllClients()
    {
        if ( activeGeneration_.has_value() ) {
            cancelStreamClients( *activeGeneration_ );
        }

        for ( auto& [ operationId, operation ] : clearOperations_ ) {
            Q_UNUSED( operationId );
            if ( operation.client ) {
                QObject::disconnect( operation.client.data(), nullptr, &transport_, nullptr );
                operation.client->cancelGeneration( operation.generation );
                operation.client->deleteLater();
            }
        }
        clearOperations_.clear();
    }

    void setState( Generation generation, LiveSourceTransport::State state )
    {
        if ( stateGeneration_ == generation && state_ == state ) {
            return;
        }
        stateGeneration_ = generation;
        state_ = state;
        Q_EMIT transport_.stateChanged( generation, state );
    }

private:
    AdbSmartSocketTransport& transport_;
    AdbSmartSocketTransportConfig config_;
    AdbSmartSocketFactory* socketFactory_{ nullptr };
    AdbSmartSocketDeadlineScheduler* deadlineScheduler_{ nullptr };
    LiveDataQueue queue_;

    QPointer<AdbSmartSocketClient> featureClient_;
    QPointer<AdbSmartSocketClient> streamClient_;
    std::unordered_map<AdbSmartSocketClient::OperationId, ClearOperation> clearOperations_;
    std::optional<Generation> activeGeneration_;
    std::optional<Generation> stateGeneration_;
    LiveSourceTransport::State state_{ LiveSourceTransport::State::Disconnected };
    QString lastError_;
    QByteArray streamStderr_;
    AdbSmartSocketClient::OperationId nextOperationId_{ 0 };
    std::uint64_t queueEpoch_{ 0 };
    bool queuePumpScheduled_{ false };
    bool terminal_{ false };
};

AdbSmartSocketTransport::AdbSmartSocketTransport( AdbSmartSocketTransportConfig config,
                                                  QObject* parent )
    : LiveSourceTransport( parent )
    , impl_( std::make_unique<Impl>( *this, std::move( config ), nullptr, nullptr ) )
{
}

AdbSmartSocketTransport::AdbSmartSocketTransport(
    AdbSmartSocketTransportConfig config, AdbSmartSocketFactory& socketFactory,
    AdbSmartSocketDeadlineScheduler& deadlineScheduler, QObject* parent )
    : LiveSourceTransport( parent )
    , impl_(
          std::make_unique<Impl>( *this, std::move( config ), &socketFactory, &deadlineScheduler ) )
{
}

AdbSmartSocketTransport::~AdbSmartSocketTransport() = default;

void AdbSmartSocketTransport::start( Generation generation )
{
    impl_->start( generation );
}

void AdbSmartSocketTransport::stop( Generation generation )
{
    impl_->stop( generation );
}

void AdbSmartSocketTransport::clearRemoteAsync( Generation generation, ClearRequestId requestId )
{
    impl_->clearRemoteAsync( generation, requestId );
}

QString AdbSmartSocketTransport::lastError() const
{
    return impl_->lastError();
}

LiveDataStatistics AdbSmartSocketTransport::statistics() const
{
    return impl_->statistics();
}

} // namespace klogg::livecapture::adb
