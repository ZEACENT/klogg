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
#include <stdexcept>
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

bool postQueuedTask( QObject& context, IosNativeTransport::QueuedTask task )
{
    return QMetaObject::invokeMethod(
        &context, [ task = std::move( task ) ]() mutable { task(); }, Qt::QueuedConnection );
}

} // namespace

struct IosNativeTransport::CallbackGate final
    : public std::enable_shared_from_this<IosNativeTransport::CallbackGate> {
    explicit CallbackGate( QueuedDispatcher value )
        : dispatcher( std::move( value ) )
    {
    }

    template <typename Callback>
    bool post( Callback callback )
    {
        const auto self = shared_from_this();
        std::lock_guard<std::mutex> lock( mutex );
        if ( transport == nullptr ) {
            return true;
        }
        // Hold the gate while enqueueing so destruction cannot detach and delete
        // the QObject between target selection and dispatch. The queued closure
        // re-checks the raw pointer on the object's own Qt thread.
        QueuedTask task = [ self, callback = std::move( callback ) ]() mutable {
            IosNativeTransport* target = nullptr;
            {
                std::lock_guard<std::mutex> callbackLock( self->mutex );
                target = self->transport;
            }
            QPointer<IosNativeTransport> guard( target );
            try {
                if ( guard ) { callback( *guard ); }
            } catch ( ... ) {
                if ( !guard ) { return; }
                guard->reportDrainFailure();
                if ( guard && guard->nativeStopped_ ) { guard->completeStopped(); }
            }
        };

        constexpr unsigned DispatchAttempts = 2u;
        bool queued = false;
        for ( unsigned attempt = 0u; attempt < DispatchAttempts && !queued; ++attempt ) {
            try {
                queued = dispatcher( *transport, task );
            } catch ( ... ) {
                queued = false;
            }
        }
        if ( queued ) {
            return true;
        }
        // A rejected notification cannot consume the sole non-empty-queue
        // wakeup. Use the normal Qt dispatcher once as a bounded fallback;
        // there is no timer, polling loop, or unbounded retry path.
        return postQueuedTask( *transport, std::move( task ) );
    }

    void detach() noexcept
    {
        std::lock_guard<std::mutex> lock( mutex );
        transport = nullptr;
    }

    QueuedDispatcher dispatcher;
    std::mutex mutex;
    IosNativeTransport* transport{ nullptr };
};

IosNativeTransport::IosNativeTransport( const IosNativeStreamWorkerFactory& workerFactory,
                                        IosNativeStreamConfig config, QObject* parent )
    : IosNativeTransport( workerFactory, std::move( config ), postQueuedTask, parent )
{
}

IosNativeTransport::IosNativeTransport( const IosNativeStreamWorkerFactory& workerFactory,
                                        IosNativeStreamConfig config,
                                        QueuedDispatcher dispatcher, QObject* parent )
    : LiveSourceTransport( parent )
    , workerFactory_( workerFactory )
    , baseConfig_( std::move( config ) )
    , callbackGate_( std::make_shared<CallbackGate>( std::move( dispatcher ) ) )
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
    session_.reset();
}

void IosNativeTransport::start( Generation generation )
{
    if ( shuttingDown_ || activeGeneration_ == generation ) {
        return;
    }

    if ( retiringGeneration_ || activeGeneration_ ) {
        pendingStart_ = generation;
        if ( activeGeneration_ ) { requestStop( *activeGeneration_, StopDisposition::DiscardPending ); }
        return;
    }
    activeGeneration_ = generation;
    discardedBytes_ = 0u;
    drainFailed_ = false;
    nativeStopped_ = false;
    drainScheduled_ = false;
    queueWorkPending_ = false;
    pendingBatch_.reset();
    pendingOffset_ = 0u;
    lastError_.clear();
    lastStructuredError_.reset();
    pendingFailure_.reset();
    resetStatistics( generation );

    if ( const auto error = validateIosLogOptions( baseConfig_.logOptions ); error.has_value() ) {
        lastStructuredError_ = *error;
        lastError_ = diagnosticText( *error );
        const auto terminalText = lastError_;
        QPointer<IosNativeTransport> guard( this );
        publishState( generation, State::Error );
        if ( guard && guard->activeGeneration_ == generation ) {
            Q_EMIT guard->errorOccurred( generation, terminalText );
        }
        return;
    }

    auto config = baseConfig_;
    config.generation = generation;
    const auto gate = callbackGate_;
    IosNativeStreamCallbacks callbacks;
    callbacks.ready = [ gate ]( Generation value ) {
        gate->post( [ value ]( IosNativeTransport& transport ) { transport.postReady( value ); } );
    };
    callbacks.bytesAvailable = [ gate ]( Generation value ) {
        if ( !gate->post(
                 [ value ]( IosNativeTransport& transport ) {
                     transport.postBytesAvailable( value );
                 } ) ) {
            throw std::runtime_error( "iOS queue notification dispatch was rejected" );
        }
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
    requestStop( generation, StopDisposition::DiscardPending );
}

void IosNativeTransport::requestStop( Generation generation, StopDisposition disposition )
{
    if ( retiringGeneration_ == generation ) {
        if ( disposition == StopDisposition::DiscardPending ) { stopDisposition_ = disposition; }
        return;
    }
    if ( activeGeneration_ != generation ) { return; }
    activeGeneration_.reset();
    retiringGeneration_ = generation;
    stopDisposition_ = disposition;
    if ( session_ ) {
        // Closes producer admission and wakes enqueueWait before native cleanup.
        // Keep session/queue/callbacks owned until the worker releases admission.
        session_->stop( generation );
    }
    else {
        postStopped( generation );
    }
}

void IosNativeTransport::clearRemoteAsync( Generation generation, ClearRequestId requestId )
{
    if ( retiringGeneration_ ) { return; }
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
            if ( guard == nullptr || guard->retiringGeneration_ ) {
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
    retiringGeneration_.reset();
    pendingBatch_.reset();
    queueWorkPending_ = false;
    pendingFailure_.reset();
    pendingStart_.reset();
    if ( generation.has_value() ) {
        publishState( *generation, State::Disconnected );
    }
}

void IosNativeTransport::postReady( Generation generation )
{
    if ( activeGeneration_ == generation && !shuttingDown_ && !pendingFailure_ ) {
        publishState( generation, State::Connected );
    }
}

void IosNativeTransport::postBytesAvailable( Generation generation )
{
    if ( ( activeGeneration_ == generation || retiringGeneration_ == generation ) && !shuttingDown_ ) {
        queueWorkPending_ = true;
        (void)drainCurrent( generation );
    }
}

void IosNativeTransport::postFailure( Generation generation, ClassifiedIosNativeError error )
{
    if ( activeGeneration_ != generation || shuttingDown_
         || ( stateGeneration_ == generation && state_ == State::Error ) || pendingFailure_ ) {
        return;
    }

    pendingFailure_ = std::move( error );
    QPointer<IosNativeTransport> guard( this );
    if ( !drainCurrent( generation ) || !guard ) {
        return;
    }
    if ( guard->pendingBatch_ || guard->queueWorkPending_ ) {
        guard->scheduleDrain( generation );
        return;
    }
    (void)guard->publishPendingFailureIfReady( generation );
}

void IosNativeTransport::postStopped( Generation generation )
{
    if ( ( activeGeneration_ != generation && retiringGeneration_ != generation ) || shuttingDown_ ) {
        return;
    }
    nativeStopped_ = true;
    QPointer<IosNativeTransport> guard( this );
    try {
        if ( !drainCurrent( generation ) || !guard ) { return; }
    }
    catch ( ... ) {
        if ( !guard ) { return; }
        guard->reportDrainFailure();
    }
    if ( guard && !guard->publishPendingFailureIfReady( generation ) ) { return; }
    if ( guard ) { guard->completeStopped(); }
}

void IosNativeTransport::completeStopped()
{
    if ( !nativeStopped_ ) { return; }
    const auto generation = activeGeneration_ ? *activeGeneration_ : retiringGeneration_.value_or( 0u );
    if ( !drainFailed_ && ( pendingBatch_ || queueWorkPending_ ) ) {
        scheduleDrain( generation );
        return;
    }
    LiveDataStatistics finalStatistics;
    if ( session_ ) {
        try {
            finalStatistics = session_->statistics();
        }
        catch ( ... ) {
            if ( !drainFailed_ ) { throw; }
            // Release the stopped worker even when exact final accounting is unavailable.
            if ( lastStructuredError_ ) {
                lastStructuredError_->nativeDetail
                    = "The final native queue statistics were unavailable during retirement.";
                lastError_ = diagnosticText( *lastStructuredError_ );
            }
        }
    }
    if ( !drainFailed_ && finalStatistics.queuedBytes != 0u ) {
        scheduleDrain( generation );
        return;
    }
    // Native stopped follows callback quiescence and native join, so rejected
    // complete records and the legacy assembler's incomplete tail are final. Add
    // both only on this terminal path, never per drain turn, and independently of
    // already-accounted queued/pending bytes.
    discardedBytes_ += static_cast<quint64>( finalStatistics.rejectedBeforeEnqueueBytes );
    discardedBytes_ += static_cast<quint64>( finalStatistics.incompleteSourceRecordBytes );
    if ( drainFailed_ ) {
        discardedBytes_ += static_cast<quint64>( finalStatistics.queuedBytes );
    }
    if ( drainFailed_ && pendingBatch_ ) {
        discardedBytes_ += static_cast<quint64>( pendingBatch_->bytes.size() - pendingOffset_ );
        pendingBatch_.reset();
    }
    nativeStopped_ = false;
    const bool preserveTerminalError = stateGeneration_ == generation && state_ == State::Error;
    activeGeneration_.reset();
    retiringGeneration_.reset();
    queueWorkPending_ = false;
    session_.reset();
    const auto successor = std::exchange( pendingStart_, std::nullopt );
    const auto discarded = discardedBytes_;
    QPointer<IosNativeTransport> guard( this );
    if ( !preserveTerminalError ) { publishState( generation, State::Disconnected ); }
    if ( !guard ) { return; }
    Q_EMIT stopped( generation, discarded );
    if ( guard && successor && !guard->activeGeneration_ && !guard->retiringGeneration_ ) {
        guard->start( *successor );
    }
}

bool IosNativeTransport::drainCurrent( Generation generation )
{
    if ( session_ == nullptr || drainFailed_
         || ( activeGeneration_ != generation && retiringGeneration_ != generation ) ) {
        return true;
    }
    if ( !pendingBatch_ ) {
        pendingBatch_ = session_->drain();
        pendingOffset_ = 0u;
        queueWorkPending_ = false;
    }
    if ( !pendingBatch_ ) { return true; }
    if ( pendingBatch_->generation != generation ) {
        reportDrainFailure();
        return true;
    }
    const auto remaining = pendingBatch_->bytes.size() - pendingOffset_;
    if ( retiringGeneration_ == generation && stopDisposition_ == StopDisposition::DiscardPending ) {
        discardedBytes_ += static_cast<quint64>( remaining );
        pendingBatch_.reset();
        if ( queueWorkPending_ ) { scheduleDrain( generation ); }
        return true;
    }
    const auto byteCount = std::min( remaining, std::size_t{ 64u } * 1024u );
    const QByteArray bytes( reinterpret_cast<const char*>( pendingBatch_->bytes.data() + pendingOffset_ ),
                            static_cast<int>( byteCount ) );
    // Advance the one authoritative delivery cursor before observers can reenter.
    pendingOffset_ += byteCount;
    if ( pendingOffset_ == pendingBatch_->bytes.size() ) { pendingBatch_.reset(); }
    if ( pendingBatch_ || queueWorkPending_ ) { scheduleDrain( generation ); }
    const QPointer<IosNativeTransport> guard( this );
    Q_EMIT bytesReceived( generation, bytes );
    return !guard.isNull();
}

bool IosNativeTransport::publishPendingFailureIfReady( Generation generation )
{
    if ( !pendingFailure_ || pendingBatch_ || queueWorkPending_ ) {
        return true;
    }

    auto error = std::move( *pendingFailure_ );
    pendingFailure_.reset();
    error.error.awaitingUserReason = error.awaitingUserReason;
    lastStructuredError_ = std::move( error.error );
    lastError_ = diagnosticText( *lastStructuredError_ );
    const auto terminalText = lastError_;
    QPointer<IosNativeTransport> guard( this );
    publishState( generation, State::Error );
    if ( guard ) {
        Q_EMIT guard->errorOccurred( generation, terminalText );
    }
    return !guard.isNull();
}

void IosNativeTransport::reportDrainFailure()
{
    if ( drainFailed_ ) { return; }
    pendingFailure_.reset();
    drainFailed_ = true;
    const auto generation = activeGeneration_ ? activeGeneration_ : retiringGeneration_;
    if ( !generation ) { return; }
    lastStructuredError_ = LiveSourceError{ ErrorCategory::Capture, "native-delivery-failed",
        ErrorScope::Capture, RetryPolicy::Never,
        "Native log delivery failed; capture completeness is uncertain.", {} };
    lastError_ = diagnosticText( *lastStructuredError_ );
    QPointer<IosNativeTransport> guard( this );
    publishState( *generation, State::Error );
    if ( guard && guard->activeGeneration_ == generation ) {
        guard->requestStop( *generation, StopDisposition::SettleAccepted );
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

void IosNativeTransport::scheduleDrain( Generation generation )
{
    if ( drainScheduled_ ) { return; }
    drainScheduled_ = true;
    callbackGate_->post( [ generation ]( IosNativeTransport& transport ) {
        const QPointer<IosNativeTransport> guard( &transport );
        transport.drainScheduled_ = false;
        if ( transport.activeGeneration_ != generation && transport.retiringGeneration_ != generation ) { return; }
        if ( !transport.drainCurrent( generation ) || !guard ) { return; }
        if ( !guard->publishPendingFailureIfReady( generation ) ) { return; }
        guard->completeStopped();
    } );
}

} // namespace klogg::livecapture::ios
