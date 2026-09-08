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
    case AdbSmartSocketErrorCode::OperationTimeout:
        return QObject::tr( "%1 timed out: %2" ).arg( operation, diagnostic );
    case AdbSmartSocketErrorCode::Connection:
    case AdbSmartSocketErrorCode::Protocol:
    case AdbSmartSocketErrorCode::RemoteFailure:
        return QObject::tr( "%1 failed: %2" ).arg( operation, diagnostic );
    }

    return QObject::tr( "%1 failed: %2" ).arg( operation, diagnostic );
}

LiveSourceError typedSmartSocketError( AdbSmartSocketErrorCode code, const QString& message,
                                       const QString& nativeDetail )
{
    ErrorCategory category = ErrorCategory::Stream;
    ErrorScope scope = ErrorScope::Stream;
    RetryPolicy retry = RetryPolicy::Backoff;
    const char* stableCode = "adb-stream-failed";
    switch ( code ) {
    case AdbSmartSocketErrorCode::Connection:
        category = ErrorCategory::Infrastructure;
        scope = ErrorScope::Infrastructure;
        retry = RetryPolicy::WaitForInfrastructure;
        stableCode = "adb-connection-failed";
        break;
    case AdbSmartSocketErrorCode::Protocol:
        category = ErrorCategory::Backend;
        stableCode = "adb-protocol-error";
        break;
    case AdbSmartSocketErrorCode::RemoteFailure:
        category = ErrorCategory::Service;
        scope = ErrorScope::Service;
        stableCode = "adb-remote-service-failed";
        break;
    case AdbSmartSocketErrorCode::UnexpectedEof:
        stableCode = "adb-unexpected-eof";
        break;
    case AdbSmartSocketErrorCode::ConnectTimeout:
        category = ErrorCategory::Infrastructure;
        scope = ErrorScope::Infrastructure;
        retry = RetryPolicy::WaitForInfrastructure;
        stableCode = "adb-connect-timeout";
        break;
    case AdbSmartSocketErrorCode::WriteTimeout:
        stableCode = "adb-write-timeout";
        break;
    case AdbSmartSocketErrorCode::ReadTimeout:
        stableCode = "adb-read-timeout";
        break;
    case AdbSmartSocketErrorCode::OperationTimeout:
        category = ErrorCategory::Service;
        scope = ErrorScope::Service;
        stableCode = "adb-operation-timeout";
        break;
    }
    return LiveSourceError{ category, stableCode, scope, retry, message.toStdString(),
                            nativeDetail.toStdString() };
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
        pendingStreamBytes_.clear();
        pendingStreamOffset_ = 0;
        streamOperationId_.reset();
        streamClientPaused_ = false;
        backpressureStatistics_ = LiveDataStatistics{};
        backpressureStatistics_.generation = generation;
        lastError_.clear();
        lastStructuredError_.reset();
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
        client->requestTransportHostService( generation, operationId, transportSelection(),
                                             TransportHostService::Features );
    }

    void stop( Generation generation, StopDisposition disposition = StopDisposition::DiscardPending )
    {
        if ( activeGeneration_ != generation ) {
            return;
        }

        QByteArray pendingTail;
        if ( pendingStreamOffset_ < pendingStreamBytes_.size() ) {
            pendingTail = pendingStreamBytes_.mid( pendingStreamOffset_ );
        }
        QByteArray clientTail;
        if ( streamClient_ && streamOperationId_.has_value() ) {
            clientTail = streamClient_->takePendingShellStdout( generation, *streamOperationId_ );
        }
        pendingStreamBytes_.clear();
        pendingStreamOffset_ = 0;
        streamClientPaused_ = false;

        // Invalidate first so synchronous cancellation callbacks and queued read
        // continuations are stale before accepted tail settlement begins.
        activeGeneration_.reset();
        terminal_ = false;
        ++queueEpoch_;
        queuePumpScheduled_ = false;
        // All ADB producers are Qt-thread clients; cancelling them closes admission.
        // LiveDataQueue::close is permanent and would poison a later explicit start.
        cancelStreamClients( generation );
        const auto tail = queue_.drain();
        quint64 discarded = 0u;
        QPointer<AdbSmartSocketTransport> guard( &transport_ );
        if ( tail && !tail->bytes.empty() ) {
            if ( disposition == StopDisposition::SettleAccepted ) {
                Q_EMIT guard->bytesReceived( generation, byteArray( tail->bytes ) );
                if ( !guard ) { return; }
            }
            else { discarded = static_cast<quint64>( tail->bytes.size() ); }
        }
        if ( !pendingTail.isEmpty() ) {
            if ( disposition == StopDisposition::SettleAccepted ) {
                Q_EMIT guard->bytesReceived( generation, pendingTail );
                if ( !guard ) { return; }
            }
            else { discarded += static_cast<quint64>( pendingTail.size() ); }
        }
        if ( !clientTail.isEmpty() ) {
            if ( disposition == StopDisposition::SettleAccepted ) {
                Q_EMIT guard->bytesReceived( generation, clientTail );
                if ( !guard ) { return; }
            }
            else { discarded += static_cast<quint64>( clientTail.size() ); }
        }
        guard->impl_->setState( generation, LiveSourceTransport::State::Disconnected );
        if ( guard ) { Q_EMIT guard->stopped( generation, discarded ); }
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
        client->requestTransportHostService( generation, operationId, transportSelection(),
                                             TransportHostService::Features );
    }

    QString lastError() const
    {
        return lastError_;
    }

    std::optional<LiveSourceError> lastStructuredError() const
    {
        return lastStructuredError_;
    }

    LiveDataStatistics statistics() const
    {
        auto result = queue_.statistics();
        accumulateLiveDataStatistics( result, backpressureStatistics_ );
        return result;
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
                    lastStructuredError_ = LiveSourceError{ ErrorCategory::Service,
                        "adb-shell-v2-unsupported", ErrorScope::Service, RetryPolicy::Never,
                        "The selected ADB device does not support shell_v2.", {} };
                    failStream(
                        generation,
                        QObject::tr(
                            "Selected ADB device does not advertise required shell_v2 support; "
                            "use a compatible ADB device." ) );
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
                const auto error
                    = contextualError( QObject::tr( "Selected ADB device features negotiation" ),
                                       code, diagnostic );
                lastStructuredError_ = typedSmartSocketError( code, error, diagnostic );
                failStream( generation, error );
            } );
    }

    void startLogcat( Generation generation )
    {
        const auto service = buildLogcatService( config_.logcatOptions );
        if ( service.error.has_value() ) {
            lastStructuredError_ = LiveSourceError{
                ErrorCategory::Configuration, "adb-logcat-options-invalid", ErrorScope::Stream,
                RetryPolicy::Never, service.error->message, service.error->message
            };
            failStream( generation, QString::fromStdString( service.error->message ) );
            return;
        }
        if ( !service.value.has_value() ) {
            const auto error = QObject::tr( "ADB logcat service builder returned no request." );
            lastStructuredError_ = LiveSourceError{
                ErrorCategory::Internal, "adb-logcat-builder-empty", ErrorScope::Stream,
                RetryPolicy::Never, error.toStdString(), {}
            };
            failStream( generation, error );
            return;
        }

        const auto operationId = nextOperationId();
        streamOperationId_ = operationId;
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
                    error.append( QString::fromStdString(
                        normalizeLogcatStreamError( diagnostic.toStdString() ) ) );
                }
                lastStructuredError_ = LiveSourceError{
                    ErrorCategory::Stream, "adb-logcat-exited", ErrorScope::Stream,
                    RetryPolicy::Backoff, error.toStdString(), diagnostic.toStdString()
                };
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
                auto nativeDetail = diagnostic;
                if ( !stderrDiagnostic.isEmpty() && stderrDiagnostic != nativeDetail ) {
                    nativeDetail.append( QLatin1Char( '\n' ) );
                    nativeDetail.append( stderrDiagnostic );
                }
                lastStructuredError_ = typedSmartSocketError( code, error, nativeDetail );
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
                        QObject::tr(
                            "Selected ADB device does not advertise required shell_v2 support; "
                            "cannot clear logcat on this device." ) );
                    return;
                }

                const auto service = buildClearLogcatService( config_.logcatOptions.buffers );
                if ( service.error.has_value() || !service.value.has_value() ) {
                    const auto diagnostic
                        = service.error.has_value()
                              ? QString::fromStdString( service.error->message )
                              : QObject::tr( "ADB logcat clear service builder returned no request." );
                    completeClear( operationId, false, diagnostic );
                    return;
                }

                found->second.phase = ClearPhase::Shell;
                client->startShellService( generation, operationId, transportSelection(),
                                           *service.value, config_.clientConfig.readTimeoutMs );
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

                const auto operation
                    = found->second.phase == ClearPhase::Features
                          ? QObject::tr( "Selected ADB device logcat clear features negotiation" )
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

    bool queueHasCapacityFor( std::size_t byteCount ) const
    {
        if ( config_.queueLimits.maxQueuedChunks == 0u
             || byteCount > config_.queueLimits.maxQueuedBytes ) {
            return false;
        }
        const auto statistics = queue_.statistics();
        return statistics.queuedChunks < config_.queueLimits.maxQueuedChunks
               && statistics.queuedBytes <= config_.queueLimits.maxQueuedBytes - byteCount;
    }

    void pauseStreamClient( Generation generation, std::size_t byteCount )
    {
        if ( streamClientPaused_ ) {
            return;
        }
        streamClientPaused_ = true;
        recordLiveDataBackpressure( backpressureStatistics_, byteCount );
        if ( streamClient_ && streamOperationId_.has_value() ) {
            streamClient_->pauseShellOutput( generation, *streamOperationId_ );
        }
    }

    void resumeStreamClient( Generation generation )
    {
        if ( !streamClientPaused_ ) {
            return;
        }
        streamClientPaused_ = false;
        if ( streamClient_ && streamOperationId_.has_value() ) {
            streamClient_->resumeShellOutput( generation, *streamOperationId_ );
        }
    }

    void drainPendingStreamBytes( Generation generation )
    {
        if ( !isActive( generation ) || pendingStreamBytes_.isEmpty() ) {
            return;
        }
        if ( config_.queueLimits.maxQueuedBytes == 0u
             || config_.queueLimits.maxQueuedChunks == 0u ) {
            lastStructuredError_ = LiveSourceError{
                ErrorCategory::Configuration, "adb-live-queue-invalid-capacity",
                ErrorScope::Stream, RetryPolicy::Never,
                "The ADB live-data queue capacity must be positive.",
                "Both byte and chunk limits must accept at least one bounded stream slice."
            };
            failStream( generation, QString::fromStdString( lastStructuredError_->message ) );
            return;
        }

        const auto maximumSlice
            = std::min( config_.queueLimits.maxQueuedBytes,
                        static_cast<std::size_t>( std::numeric_limits<int>::max() ) );
        while ( pendingStreamOffset_ < pendingStreamBytes_.size() ) {
            const auto remaining = pendingStreamBytes_.size() - pendingStreamOffset_;
            using ByteArrayIndex = decltype( pendingStreamBytes_.size() );
            const auto byteCount
                = std::min( remaining, static_cast<ByteArrayIndex>( maximumSlice ) );
            const auto chunkByteCount = static_cast<std::size_t>( byteCount );
            if ( !queueHasCapacityFor( chunkByteCount ) ) {
                pauseStreamClient( generation, chunkByteCount );
                return;
            }

            const auto chunk = pendingStreamBytes_.mid( pendingStreamOffset_, byteCount );
            const auto result
                = queue_.tryEnqueue( LiveDataChunk{ generation, byteVector( chunk ) } );
            if ( result == LiveDataEnqueueResult::StaleGeneration
                 || result == LiveDataEnqueueResult::Closed ) {
                return;
            }
            if ( result == LiveDataEnqueueResult::Backpressure ) {
                failStream( generation,
                            QObject::tr( "ADB logcat queue capacity changed unexpectedly." ) );
                return;
            }
            pendingStreamOffset_ += byteCount;
        }

        pendingStreamBytes_.clear();
        pendingStreamOffset_ = 0;
        resumeStreamClient( generation );
    }

    void enqueueStreamBytes( Generation generation, const QByteArray& bytes )
    {
        if ( !pendingStreamBytes_.isEmpty() ) {
            lastStructuredError_ = LiveSourceError{
                ErrorCategory::Internal, "adb-delivery-while-paused", ErrorScope::Stream,
                RetryPolicy::Never,
                "ADB delivered another shell frame while the bounded consumer was paused.", {}
            };
            failStream( generation, QString::fromStdString( lastStructuredError_->message ) );
            return;
        }
        pendingStreamBytes_ = bytes;
        pendingStreamOffset_ = 0;
        drainPendingStreamBytes( generation );
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
        if ( batch.has_value() && batch->generation == generation ) {
            const auto bytes = byteArray( batch->bytes );
            if ( bytes.isEmpty() && !batch->bytes.empty() ) {
                failStream( generation,
                            QObject::tr( "ADB logcat queue batch exceeds Qt byte-array limits." ) );
                return;
            }
            if ( !bytes.isEmpty() ) {
                const QPointer<AdbSmartSocketTransport> guard( &transport_ );
                Q_EMIT transport_.bytesReceived( generation, bytes );
                if ( guard.isNull() ) {
                    return;
                }
            }
        }

        drainPendingStreamBytes( generation );
    }

    void failStream( Generation generation, QString error )
    {
        if ( !isActive( generation ) ) {
            return;
        }

        // The client can report stdout and a terminal frame/EOF from the same
        // socket read. Preserve wire order across the asynchronous queue boundary:
        // all accepted stdout must be delivered before the terminal state/error.
        const QPointer<AdbSmartSocketTransport> guard( &transport_ );
        pumpQueue( generation );
        if ( guard.isNull() || !isActive( generation ) ) {
            return;
        }

        terminal_ = true;
        lastError_ = std::move( error );
        if ( !lastStructuredError_ ) {
            lastStructuredError_ = LiveSourceError{ ErrorCategory::Stream, "adb-stream-failed",
                ErrorScope::Stream, RetryPolicy::Backoff, lastError_.toStdString(), {} };
        }
        const auto terminalError = lastError_;
        retireFeatureClient( generation );
        retireStreamClient( generation );
        setState( generation, LiveSourceTransport::State::Error );
        if ( guard.isNull() ) {
            return;
        }

        // stateChanged is synchronous and may start a replacement generation,
        // which clears lastError_. The failed generation still owns exactly one
        // correlated diagnostic, matching ProcessLiveSourceTransport's contract.
        Q_EMIT guard->errorOccurred( generation, terminalError );
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
        streamOperationId_.reset();
        streamClientPaused_ = false;
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
    std::optional<LiveSourceError> lastStructuredError_;
    QByteArray streamStderr_;
    QByteArray pendingStreamBytes_;
    decltype( QByteArray{}.size() ) pendingStreamOffset_{ 0 };
    std::optional<AdbSmartSocketClient::OperationId> streamOperationId_;
    LiveDataStatistics backpressureStatistics_;
    AdbSmartSocketClient::OperationId nextOperationId_{ 0 };
    std::uint64_t queueEpoch_{ 0 };
    bool queuePumpScheduled_{ false };
    bool streamClientPaused_{ false };
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

void AdbSmartSocketTransport::requestStop( Generation generation, StopDisposition disposition )
{
    impl_->stop( generation, disposition );
}

std::optional<LiveSourceError> AdbSmartSocketTransport::lastStructuredError() const
{
    return impl_->lastStructuredError();
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
