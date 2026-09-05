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

#include "iosnativetransport.h"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace klogg::livecapture::ios {
namespace {

QString diagnosticText( const LiveSourceError& error )
{
    auto text = QString::fromStdString( error.message );
    const auto detail = QString::fromStdString( error.nativeDetail );
    if ( !detail.isEmpty() && detail != text ) {
        if ( !text.isEmpty() ) {
            text.append( QLatin1Char( '\n' ) );
        }
        text.append( detail );
    }
    return text;
}

LiveSourceError clearUnsupportedError()
{
    return LiveSourceError{ ErrorCategory::Configuration,
                            "ios-clear-unsupported",
                            ErrorScope::Stream,
                            RetryPolicy::Never,
                            "Clearing the remote iOS log stream is not supported.",
                            "The native relay is read-only and never reports fake clear success." };
}

} // namespace

struct IosNativeTransport::CallbackGate final
    : public std::enable_shared_from_this<IosNativeTransport::CallbackGate> {
    template <typename Callback>
    void post( Callback callback )
    {
        const auto self = shared_from_this();
        std::lock_guard<std::mutex> lock( mutex );
        if ( transport == nullptr ) {
            return;
        }
        // Hold the gate while enqueueing so destruction cannot detach and delete
        // the QObject between target selection and invokeMethod(). The queued
        // closure re-checks the raw pointer on the object's own Qt thread.
        QMetaObject::invokeMethod(
            transport,
            [ self, callback = std::move( callback ) ]() mutable {
                try {
                    IosNativeTransport* target = nullptr;
                    {
                        std::lock_guard<std::mutex> callbackLock( self->mutex );
                        target = self->transport;
                    }
                    if ( target != nullptr ) {
                        callback( *target );
                    }
                } catch ( ... ) { // NOLINT(bugprone-empty-catch)
                    // Qt queued callbacks are an exception boundary. A failed
                    // observer or allocation must not escape the event dispatcher.
                }
            },
            Qt::QueuedConnection );
    }

    void detach() noexcept
    {
        std::lock_guard<std::mutex> lock( mutex );
        transport = nullptr;
    }

    std::mutex mutex;
    IosNativeTransport* transport{ nullptr };
};

IosNativeTransport::IosNativeTransport( const IosNativeStreamWorkerFactory& workerFactory,
                                        IosNativeStreamConfig config, QObject* parent )
    : LiveSourceTransport( parent )
    , workerFactory_( workerFactory )
    , baseConfig_( std::move( config ) )
    , callbackGate_( std::make_shared<CallbackGate>() )
{
    callbackGate_->transport = this;
}

IosNativeTransport::~IosNativeTransport()
{
    callbackGate_->detach();
    shuttingDown_ = true;
    if ( session_ != nullptr ) {
        session_->shutdown();
    }
    for ( auto& retired : retiredSessions_ ) {
        retired.session->shutdown();
    }
    session_.reset();
    retiredSessions_.clear();
}

void IosNativeTransport::start( Generation generation )
{
    if ( shuttingDown_ || activeGeneration_ == generation ) {
        return;
    }

    retireCurrent( true );
    activeGeneration_ = generation;
    lastError_.clear();
    lastStructuredError_.reset();
    resetStatistics( generation );

    auto config = baseConfig_;
    config.generation = generation;
    const auto gate = callbackGate_;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ gate ]( Generation value ) {
        gate->post( [ value ]( IosNativeTransport& transport ) { transport.postReady( value ); } );
    };
    callbacks.bytesAvailable = [ gate ]( Generation value ) {
        gate->post(
            [ value ]( IosNativeTransport& transport ) { transport.postBytesAvailable( value ); } );
    };
    callbacks.failed = [ gate ]( Generation value, const ClassifiedIosNativeError& error ) {
        auto ownedError = error;
        gate->post(
            [ value, error = std::move( ownedError ) ]( IosNativeTransport& transport ) mutable {
                transport.postFailure( value, std::move( error ) );
            } );
    };
    callbacks.stopped = [ gate ]( Generation value ) {
        gate->post(
            [ value ]( IosNativeTransport& transport ) { transport.postStopped( value ); } );
    };

    QPointer<IosNativeTransport> guard( this );
    publishState( generation, State::Connecting );
    if ( guard == nullptr || guard->shuttingDown_ || guard->activeGeneration_ != generation ) {
        return;
    }

    auto creation = guard->workerFactory_.create( config, std::move( callbacks ) );
    guard->session_ = std::move( creation.session );
    if ( guard->session_ == nullptr ) {
        if ( creation.error.has_value() ) {
            postFailure( generation, std::move( *creation.error ) );
        }
        else {
            const LiveSourceError error{ ErrorCategory::Backend,
                                         "ios-native-worker-create-failed",
                                         ErrorScope::Stream,
                                         RetryPolicy::Backoff,
                                         "The native iOS stream worker could not be created.",
                                         "The worker factory rejected session creation." };
            postFailure( generation, ClassifiedIosNativeError{ error, std::nullopt } );
        }
    }
    else if ( !guard->session_->start() ) {
        guard->session_.reset();
        const LiveSourceError error{ ErrorCategory::Backend,
                                     "ios-native-worker-start-failed",
                                     ErrorScope::Stream,
                                     RetryPolicy::Backoff,
                                     "The native iOS stream worker could not start.",
                                     "The dedicated native worker rejected startup." };
        postFailure( generation, ClassifiedIosNativeError{ error, std::nullopt } );
    }
}

void IosNativeTransport::stop( Generation generation )
{
    if ( activeGeneration_ != generation ) {
        return;
    }
    retireCurrent( true );
    publishState( generation, State::Disconnected );
}

void IosNativeTransport::clearRemoteAsync( Generation generation, ClearRequestId requestId )
{
    if ( activeGeneration_.has_value() && activeGeneration_.value() != generation ) {
        return;
    }
    const auto error = clearUnsupportedError();
    lastStructuredError_ = error;
    lastError_ = diagnosticText( error );
    QPointer<IosNativeTransport> guard( this );
    QMetaObject::invokeMethod(
        this,
        [ guard, generation, requestId ] {
            if ( guard == nullptr ) {
                return;
            }
            if ( guard->activeGeneration_.has_value()
                 && guard->activeGeneration_.value() != generation ) {
                return;
            }
            Q_EMIT guard->clearRemoteFinished(
                generation, requestId, false,
                QStringLiteral( "Clearing the remote iOS log stream is not supported." ) );
        },
        Qt::QueuedConnection );
}

QString IosNativeTransport::lastError() const
{
    return lastError_;
}

LiveDataStatistics IosNativeTransport::statistics() const
{
    LiveDataStatistics result;
    if ( activeGeneration_.has_value() ) {
        result.generation = *activeGeneration_;
    }
    if ( session_ != nullptr ) {
        result = session_->statistics();
    }
    return result;
}

std::optional<LiveSourceError> IosNativeTransport::lastStructuredError() const
{
    return lastStructuredError_;
}

void IosNativeTransport::serviceShutdown()
{
    if ( shuttingDown_ ) {
        return;
    }
    shuttingDown_ = true;
    const auto generation = activeGeneration_;
    if ( session_ != nullptr ) {
        if ( generation.has_value() ) {
            session_->stop( *generation );
        }
        session_->shutdown();
        session_.reset();
    }
    activeGeneration_.reset();
    for ( auto& retired : retiredSessions_ ) {
        retired.session->shutdown();
    }
    retiredSessions_.clear();
    if ( generation.has_value() ) {
        publishState( *generation, State::Disconnected );
    }
}

void IosNativeTransport::postReady( Generation generation )
{
    if ( activeGeneration_ == generation && !shuttingDown_ ) {
        publishState( generation, State::Connected );
    }
}

void IosNativeTransport::postBytesAvailable( Generation generation )
{
    if ( activeGeneration_ == generation && !shuttingDown_ ) {
        drainCurrent( generation );
    }
}

void IosNativeTransport::postFailure( Generation generation, ClassifiedIosNativeError error )
{
    if ( activeGeneration_ != generation || shuttingDown_
         || ( stateGeneration_ == generation && state_ == State::Error ) ) {
        return;
    }
    drainCurrent( generation );
    if ( activeGeneration_ != generation ) {
        return;
    }
    error.error.awaitingUserReason = error.awaitingUserReason;
    lastStructuredError_ = std::move( error.error );
    lastError_ = diagnosticText( *lastStructuredError_ );
    const auto terminalText = lastError_;
    QPointer<IosNativeTransport> guard( this );
    publishState( generation, State::Error );
    if ( guard != nullptr ) {
        Q_EMIT guard->errorOccurred( generation, terminalText );
    }
}

void IosNativeTransport::postStopped( Generation generation )
{
    const auto retired = std::remove_if(
        retiredSessions_.begin(), retiredSessions_.end(),
        [ generation ]( const auto& entry ) { return entry.generation == generation; } );
    if ( retired != retiredSessions_.end() ) {
        retiredSessions_.erase( retired, retiredSessions_.end() );
    }
    if ( activeGeneration_ != generation || shuttingDown_ ) {
        return;
    }
    drainCurrent( generation );
    if ( activeGeneration_ == generation ) {
        const bool preserveTerminalError = stateGeneration_ == generation && state_ == State::Error;
        activeGeneration_.reset();
        session_.reset();
        if ( !preserveTerminalError ) {
            publishState( generation, State::Disconnected );
        }
    }
}

void IosNativeTransport::drainCurrent( Generation generation )
{
    if ( session_ == nullptr || activeGeneration_ != generation ) {
        return;
    }
    while ( activeGeneration_ == generation ) {
        const auto batch = session_->drain();
        if ( !batch.has_value() || batch->generation != generation ) {
            return;
        }
        if ( batch->bytes.empty() ) {
            continue;
        }
        const auto maximum = static_cast<std::size_t>( std::numeric_limits<int>::max() );
        const auto byteCount = std::min( batch->bytes.size(), maximum );
        const QByteArray bytes( reinterpret_cast<const char*>( batch->bytes.data() ),
                                static_cast<int>( byteCount ) );
        QPointer<IosNativeTransport> guard( this );
        Q_EMIT bytesReceived( generation, bytes );
        if ( guard == nullptr || guard->activeGeneration_ != generation ) {
            return;
        }
    }
}

void IosNativeTransport::publishState( Generation generation, State state )
{
    if ( stateGeneration_ == generation && state_ == state ) {
        return;
    }
    stateGeneration_ = generation;
    state_ = state;
    Q_EMIT stateChanged( generation, state );
}

void IosNativeTransport::retireCurrent( bool requestStop )
{
    if ( session_ == nullptr ) {
        activeGeneration_.reset();
        return;
    }
    const auto generation = activeGeneration_;
    activeGeneration_.reset();
    if ( requestStop && generation.has_value() ) {
        session_->stop( *generation );
    }
    retiredSessions_.push_back(
        RetiredSession{ generation.value_or( 0u ), std::move( session_ ) } );
}

} // namespace klogg::livecapture::ios
